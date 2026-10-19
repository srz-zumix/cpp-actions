#include "engine/job.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "actions/action.hpp"
#include "actions/builtin.hpp"
#include "engine/commands.hpp"
#include "engine/evaluate.hpp"
#include "engine/shell.hpp"
#include "expr/template.hpp"
#include "sandbox/sandbox.hpp"
#include "util/fs.hpp"
#include "util/glob.hpp"
#include "util/process.hpp"
#include "util/sha256.hpp"
#include "util/strings.hpp"
#include "workflow/matrix.hpp"
#include "workflow/scopes.hpp"

namespace cpa::engine {

namespace {

using expr::Value;
using workflow::Site;
using workflow::scope_for;
using Clock = std::chrono::steady_clock;
using StringMap = std::map<std::string, std::string>;

constexpr std::size_t kMaxSummarySize = 1024 * 1024;
constexpr int kMaxCompositeDepth = 10;
constexpr double kDefaultJobTimeoutMinutes = 360;

std::int64_t elapsed_ms(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string runner_os() {
#if defined(__APPLE__)
  return "macOS";
#else
  return "Linux";
#endif
}

std::string runner_arch() {
  struct utsname u{};
  if (::uname(&u) == 0) {
    const std::string m = u.machine;
    if (m == "arm64" || m == "aarch64") return "ARM64";
  }
  return "X64";
}

std::string default_path() {
#if defined(__APPLE__)
  return "/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";
#else
  return "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
#endif
}

// Shallow copy of an object with additional fields.
Value with_fields(const Value& base, const std::vector<std::pair<std::string, Value>>& fields) {
  auto out = Value::object();
  if (base.is_object()) {
    for (const auto& [k, v] : base.object_data().entries()) out.set(k, v);
  }
  for (const auto& [k, v] : fields) out.set(k, v);
  return out;
}

std::optional<std::chrono::milliseconds> minutes_to_ms(double minutes) {
  if (!(minutes > 0) || !std::isfinite(minutes)) return std::nullopt;
  return std::chrono::milliseconds(static_cast<std::int64_t>(minutes * 60000.0));
}

struct StepScope {
  Value steps = Value::object();
  Value inputs = Value::object();
  std::string status = "success";  // failures of the steps of this scope
  bool composite = false;
  std::string file;
  fs::path action_path;
  std::string action_repository;
  std::string action_ref;
  StringMap env;  // env of the composite step (inherited by the nested steps)
  int depth = 0;
  std::string prefix;
  Site if_site = Site::StepIf;
  Site general_site = Site::StepGeneral;
};

struct StepResult {
  std::string outcome = "success";
  StringMap outputs;
  std::string error;
};

struct FileCommands {
  fs::path env;
  fs::path output;
  fs::path path;
  fs::path state;
  fs::path summary;
};

struct CommandState {
  StringMap outputs;
  StringMap state;
  bool failed = false;
  bool allow_unsecure = false;
};

struct ResolvedAction {
  fs::path dir;
  std::string repository;  // owner/repo ("" for local actions)
  std::string ref;
  std::string display;
};

struct PostStep {
  std::string name;
  std::string id;
  actions::Builtin builtin = actions::Builtin::None;
  const actions::Action* action = nullptr;
  ResolvedAction resolved;
  StringMap inputs;
  StringMap state;
  StringMap env;  // env context of the main step
  const StepScope* scope = nullptr;
  const yaml::Node* condition = nullptr;
  std::string condition_text = "always()";
  std::string file;
};

class JobRunner {
 public:
  explicit JobRunner(JobInstance& inst)
      : inst_(inst), run_(*inst.run), wr_(*inst.workflow), job_(*inst.job), wf_(*inst.workflow->workflow) {}

  JobReport run();

 private:
  // Setup
  bool setup();
  void create_directories();
  void build_base_environment();
  bool evaluate_job_settings();
  void load_environment(const std::string& name);
  void start_sandbox();
  void prefetch_actions();

  // Contexts
  expr::EvalContext make_context(const StepScope& scope, const StringMap& env);
  expr::EvalContext job_context(const StringMap& env);
  std::string current_status(const StepScope* scope) const;
  bool is_cancelled() const { return inst_.cancel.load() || timed_out_; }
  StringMap scope_env(const StepScope& scope) const;

  // Steps
  void run_step(StepScope& scope, const workflow::Step& step);
  StepResult run_script(StepScope& scope, const workflow::Step& step, expr::EvalContext& ctx, const StringMap& env,
                        std::optional<std::chrono::milliseconds> timeout, bool after_cancel);
  StepResult run_uses(StepScope& scope, const workflow::Step& step, const std::string& name, expr::EvalContext& ctx,
                      const StringMap& env, std::optional<std::chrono::milliseconds> timeout, bool after_cancel);
  StepResult run_node(const StepScope& scope, const std::string& step_id, const actions::Action& action,
                      const ResolvedAction& resolved, const StringMap& inputs, const std::string& file, StringMap& state,
                      const StringMap& env, std::optional<std::chrono::milliseconds> timeout, bool after_cancel);
  StepResult run_composite(StepScope& parent, const std::string& name,
                           const actions::Action& action, const ResolvedAction& resolved, const StringMap& inputs,
                           const StringMap& step_env);
  void run_post_steps();
  StringMap action_inputs(const actions::Action& action, const workflow::Step& step, const StepScope& scope,
                          expr::EvalContext& ctx);
  ResolvedAction resolve_action(const actions::ActionRef& ref);
  actions::BuiltinContext builtin_context();

  // Processes
  StepResult execute(std::vector<std::string> argv, util::Environment env, const fs::path& cwd,
                     std::optional<std::chrono::milliseconds> timeout, bool after_cancel, const FileCommands& files,
                     StringMap* state, const std::vector<fs::path>& extra_read = {});
  util::Environment step_environment(const StringMap& env, const FileCommands& files, const StepScope& scope,
                                     const std::string& action_id);
  FileCommands prepare_file_commands();
  void process_file_commands(const FileCommands& files, StepResult& result, StringMap* state);
  void handle_line(std::string_view line, CommandState& cs);
  void annotate(const std::string& level, const WorkflowCommand& cmd);

  // Results
  void record_step(StepScope& scope, const workflow::Step& step, StepReport report);
  void evaluate_outputs();
  void finish();

  JobInstance& inst_;
  RunState& run_;
  const WorkflowRun& wr_;
  const workflow::Job& job_;
  const workflow::Workflow& wf_;
  JobReport report_;
  std::unique_ptr<JobLog> log_;
  Clock::time_point started_ = Clock::now();
  std::optional<Clock::time_point> deadline_;
  double timeout_minutes_ = kDefaultJobTimeoutMinutes;
  bool continue_on_error_ = false;

  fs::path job_dir_;
  fs::path control_dir_;
  fs::path work_root_;
  fs::path workspace_;
  fs::path temp_;
  fs::path home_;
  fs::path tmp_;
  fs::path file_commands_dir_;
  fs::path event_path_;
  fs::path tool_cache_;

  util::Environment base_env_;
  StringMap global_env_;
  std::vector<std::string> path_prepend_;
  Value secrets_ = Value::object();
  Value vars_ = Value::object();
  StringMap secret_values_;
  Value github_;
  Value runner_ctx_;
  std::string status_ = "success";
  bool timed_out_ = false;
  bool running_post_ = false;
  std::unique_ptr<sandbox::Sandbox> sandbox_;
  std::mutex pgid_mutex_;
  std::vector<pid_t> pgids_;
  std::vector<PostStep> post_steps_;
  std::vector<std::unique_ptr<actions::Action>> loaded_actions_;
  std::vector<std::unique_ptr<StepScope>> composite_scopes_;
  bool echo_ = false;
  std::string stop_token_;
  StepScope top_;
};

// ------------------------------------------------------------------ setup

void JobRunner::create_directories() {
  const auto key = sanitize_key(inst_.key);
  job_dir_ = run_.options.run_dir / "jobs" / key;
  control_dir_ = run_.options.run_dir / "control" / key;
  const std::string repo_name = run_.repository.substr(run_.repository.find('/') + 1);
  work_root_ = job_dir_ / "work";
  workspace_ = work_root_ / repo_name / repo_name;
  temp_ = work_root_ / "_temp";
  home_ = job_dir_ / "home";
  tmp_ = job_dir_ / "tmp";
  file_commands_dir_ = temp_ / "_runner_file_commands";
  event_path_ = temp_ / "_github_workflow" / "event.json";
  tool_cache_ = run_.options.cache_dir / "toolcache";
  util::remove_tree(job_dir_);
  util::remove_tree(control_dir_);
  fs::create_directories(control_dir_);
  for (const auto& d : {workspace_, temp_, home_, tmp_, file_commands_dir_, event_path_.parent_path(), tool_cache_}) {
    fs::create_directories(d);
  }
  // Canonical paths (e.g. /var -> /private/var on macOS) for the sandbox rules.
  job_dir_ = util::real_path(job_dir_);
  control_dir_ = util::real_path(control_dir_);
  work_root_ = job_dir_ / "work";
  workspace_ = work_root_ / repo_name / repo_name;
  temp_ = work_root_ / "_temp";
  home_ = job_dir_ / "home";
  tmp_ = job_dir_ / "tmp";
  file_commands_dir_ = temp_ / "_runner_file_commands";
  event_path_ = temp_ / "_github_workflow" / "event.json";
  tool_cache_ = util::real_path(tool_cache_);
  const auto control_root = control_dir_.parent_path();
  for (const auto& d : {workspace_, temp_, home_, tmp_, tool_cache_,
                        run_.options.cache_dir / "cache" / run_.repository, wr_.artifacts}) {
    const auto allocated = util::real_path(d);
    if (util::is_subpath(control_root, allocated) || util::is_subpath(allocated, control_root)) {
      throw EvaluationError("Allocated action directory overlaps the protected job control directory: " +
                            allocated.string());
    }
  }
  util::write_file(event_path_, wr_.payload.to_json_string(true));
}

void JobRunner::build_base_environment() {
  const auto host = util::current_environment();
  base_env_["PATH"] = default_path();
  base_env_["HOME"] = home_.string();
  base_env_["TMPDIR"] = tmp_.string();
#if defined(__APPLE__)
  base_env_["LANG"] = "en_US.UTF-8";
#else
  base_env_["LANG"] = "C.UTF-8";
#endif
  for (const char* name : {"USER", "LOGNAME"}) {
    if (auto it = host.find(name); it != host.end()) base_env_[name] = it->second;
  }
  base_env_["SHELL"] = "/bin/bash";
  base_env_["CI"] = "true";
  for (const auto& entry : run_.options.pass_env) {
    const auto eq = entry.find('=');
    if (eq != std::string::npos) {
      base_env_[entry.substr(0, eq)] = entry.substr(eq + 1);
    } else if (auto it = host.find(entry); it != host.end()) {
      base_env_[entry] = it->second;
    }
  }
}

void JobRunner::load_environment(const std::string& name) {
  auto it = run_.options.environments.find(name);
  if (it == run_.options.environments.end()) {
    log_->info("Environment '" + name + "' is not configured; using the repository secrets and variables",
               Color::Gray);
    return;
  }
  const auto& env = it->second;
  if (!env.secrets_file.empty()) {
    for (const auto& [k, v] : config::load_dotenv(config::expand_user(env.secrets_file))) {
      secrets_.set(k, v);
      run_.masker.add(v);
    }
  }
  if (!env.vars_file.empty()) {
    for (const auto& [k, v] : config::load_dotenv(config::expand_user(env.vars_file))) vars_.set(k, v);
  }
  for (const auto& [k, v] : env.vars) vars_.set(k, v);
  log_->info("Environment: " + name, Color::Gray);
}

bool JobRunner::evaluate_job_settings() {
  auto ctx = job_context({});
  const auto general = scope_for(Site::JobGeneral);
  // runs-on
  const auto runs_on = evaluate_token(job_.runs_on, ctx, general, wf_.file);
  const auto labels = runs_on_labels(runs_on);
  const auto match = match_runner(labels, run_.options.host_labels, host_is_macos());
  if (!match.host) {
    report_.result = "skipped";
    report_.reason = match.reason;
    log_->warning("Skipping the job: " + match.reason);
    return false;
  }
  log_->info("Runner: " + util::join(labels, ", ") + " (host " + runner_os() + " " + runner_arch() + ")",
             Color::Gray);
  if (job_.container != nullptr || job_.services != nullptr) {
    throw EvaluationError("Jobs with 'container' or 'services' need Docker, which is not supported by cpp-actions");
  }
  if (job_.environment != nullptr) {
    const auto env = evaluate_token(job_.environment, ctx, general, wf_.file);
    std::string name = env.is_object() ? env.get("name").to_string() : env.to_string();
    if (!name.empty()) load_environment(name);
  }
  if (job_.timeout_minutes != nullptr) {
    timeout_minutes_ = evaluate_token(job_.timeout_minutes, ctx, general, wf_.file).to_number();
  }
  if (auto ms = minutes_to_ms(timeout_minutes_)) deadline_ = started_ + *ms;
  if (job_.continue_on_error != nullptr) {
    continue_on_error_ = evaluate_token(job_.continue_on_error, ctx, general, wf_.file).truthy();
    report_.continue_on_error = continue_on_error_;
  }
  // Workflow and job env.
  for (const auto& [k, v] : evaluate_string_map(wf_.env, ctx, scope_for(Site::WorkflowEnv), wf_.file)) {
    global_env_[k] = v;
  }
  ctx = job_context(global_env_);
  for (const auto& [k, v] : evaluate_string_map(job_.env, ctx, scope_for(Site::JobEnv), wf_.file)) {
    global_env_[k] = v;
  }
  return true;
}

void JobRunner::start_sandbox() {
  if (!run_.options.sandbox.enabled) {
    log_->warning("The sandbox is disabled: steps can access all files and the network");
    return;
  }
  sandbox::SandboxConfig config;
  config.write = {workspace_, temp_, home_, tmp_, tool_cache_};
  for (const auto& d : sandbox::darwin_user_dirs()) config.write.push_back(d);
  for (const auto& d : run_.options.sandbox.allow_write) config.write.push_back(d);
  const auto control_root = control_dir_.parent_path();
  for (const auto& d : config.write) {
    const auto writable = util::real_path(d);
    if (util::is_subpath(control_root, writable) || util::is_subpath(writable, control_root)) {
      throw EvaluationError("Writable sandbox path overlaps the protected job control directory: " +
                            writable.string());
    }
  }
  const auto actions_dir = run_.options.cache_dir / "actions";
  const auto node_dir = run_.options.cache_dir / "node";
  fs::create_directories(actions_dir);
  fs::create_directories(node_dir);
  config.read = {actions_dir, node_dir};
  for (const auto& d : run_.options.sandbox.allow_read) config.read.push_back(d);
  config.allow_hosts = run_.options.sandbox.allow_hosts;
  config.allow_localhost = run_.options.sandbox.allow_localhost;
  config.profile_file = control_dir_ / "sandbox.sb";
  config.extra_rules = "(deny file-read* file-write* (subpath " +
                       sandbox::sbpl_quote(control_root.string()) + "))";
  auto* log = log_.get();
  sandbox_ = std::make_unique<sandbox::Sandbox>(config, [log](const std::string& host, int port) {
    log->warning("Blocked a connection to " + host + ":" + std::to_string(port) +
                 " (not allowed). Use --allow-host " + host + " to allow it.");
  });
  log_->info("Sandbox: " + config.profile_file.string(), Color::Gray);
}

void JobRunner::prefetch_actions() {
  for (const auto& step : job_.steps) {
    if (step.is_run() || step.uses.empty() || expr::contains_expression(step.uses)) continue;
    const auto ref = actions::parse_action_ref(step.uses);
    if (ref.kind != actions::ActionRef::Kind::Remote || actions::find_builtin(ref) != actions::Builtin::None) {
      continue;
    }
    resolve_action(ref);
  }
}

bool JobRunner::setup() {
  create_directories();
  build_base_environment();
  report_.log_file = (run_.options.run_dir / "logs" / (sanitize_key(inst_.key) + ".log")).string();
  log_ = std::make_unique<JobLog>(run_.console, run_.masker, inst_.name, report_.log_file, run_.options.debug);
  log_->info("Set up job", Color::Bold);

  for (const auto& [k, v] : wr_.secrets) secrets_.set(k, v);
  if (wr_.vars.is_object()) {
    for (const auto& [k, v] : wr_.vars.object_data().entries()) vars_.set(k, v);
  }

  runner_ctx_ = Value::object();
  runner_ctx_.set("name", "cpp-actions");
  runner_ctx_.set("os", runner_os());
  runner_ctx_.set("arch", runner_arch());
  runner_ctx_.set("temp", temp_.string());
  runner_ctx_.set("tool_cache", tool_cache_.string());
  runner_ctx_.set("debug", run_.options.debug ? "1" : "");
  runner_ctx_.set("environment", "github-hosted");
  github_ = github_context(run_, wr_, job_.id, workspace_, event_path_);
  top_.file = wf_.file;
  top_.inputs = wr_.inputs.is_object() ? wr_.inputs : Value::object();

  if (!evaluate_job_settings()) return false;
  start_sandbox();
  prefetch_actions();
  return true;
}

// --------------------------------------------------------------- contexts

expr::EvalContext JobRunner::job_context(const StringMap& env) { return make_context(top_, env); }

std::string JobRunner::current_status(const StepScope* scope) const {
  if (is_cancelled() || status_ == "cancelled") return "cancelled";
  if (status_ == "failure" || top_.status == "failure" || (scope != nullptr && scope->status == "failure")) {
    return "failure";
  }
  return "success";
}

expr::EvalContext JobRunner::make_context(const StepScope& scope, const StringMap& env) {
  expr::EvalContext ctx;
  auto c = Value::object();
  if (scope.composite || !scope.action_path.empty()) {
    c.set("github", with_fields(github_, {{"action_path", scope.action_path.string()},
                                          {"action_repository", scope.action_repository},
                                          {"action_ref", scope.action_ref},
                                          {"action_status", current_status(&scope)}}));
  } else {
    c.set("github", github_);
  }
  c.set("env", to_object(env));
  c.set("vars", vars_);
  c.set("secrets", secrets_);
  auto job = Value::object();
  job.set("status", current_status(nullptr));
  c.set("job", job);
  c.set("steps", scope.steps);
  c.set("runner", runner_ctx_);
  c.set("strategy", inst_.strategy);
  c.set("matrix", inst_.matrix);
  c.set("needs", inst_.needs);
  c.set("inputs", scope.inputs);
  ctx.contexts = c;
  ctx.status = [this, &scope](std::string_view fn) {
    const auto status = current_status(&scope);
    if (fn == "always") return true;
    if (fn == "success") return status == "success";
    if (fn == "failure") return status == "failure";
    if (fn == "cancelled") return status == "cancelled";
    return false;
  };
  const auto root = workspace_;
  ctx.hash_files = [root](const std::vector<std::string>& patterns, bool) { return hash_files(root, patterns); };
  return ctx;
}

StringMap JobRunner::scope_env(const StepScope& scope) const {
  StringMap env = global_env_;
  for (const auto& [k, v] : scope.env) env[k] = v;
  return env;
}

// ------------------------------------------------------------------ steps

void JobRunner::record_step(StepScope& scope, const workflow::Step& step, StepReport report) {
  if (!step.id.starts_with("__")) {
    auto entry = Value::object();
    entry.set("outputs", to_object(report.outputs));
    entry.set("outcome", report.outcome);
    entry.set("conclusion", report.conclusion);
    scope.steps.set(step.id, entry);
  }
  if (report.conclusion == "failure") scope.status = "failure";
  report_.steps.push_back(std::move(report));
}

void JobRunner::run_step(StepScope& scope, const workflow::Step& step) {
  StepReport sr;
  sr.id = step.id;
  const auto step_started = Clock::now();
  if (inst_.cancel && status_ != "cancelled") status_ = "cancelled";
  const bool after_cancel = is_cancelled();
  const auto env_before = scope_env(scope);
  auto ctx = make_context(scope, env_before);
  const auto general = scope_for(scope.general_site);

  std::string name = step.default_display_name();
  if (step.name != nullptr) {
    try {
      name = evaluate_string(step.name, ctx, general, scope.file);
    } catch (const std::exception&) {
      // Keep the default name; errors are reported when the step runs.
    }
  }
  sr.name = scope.prefix + name;

  auto fail_step = [&](const std::string& message) {
    log_->error(message);
    sr.outcome = sr.conclusion = "failure";
    sr.error = message;
    sr.duration_ms = elapsed_ms(step_started);
    log_->info("x " + sr.name + " (failure)", Color::Red);
    record_step(scope, step, std::move(sr));
  };

  bool run_it = false;
  try {
    run_it = evaluate_condition(step.condition, ctx, scope_for(scope.if_site), scope.file);
  } catch (const std::exception& e) {
    fail_step(e.what());
    return;
  }
  if (!run_it) {
    sr.outcome = sr.conclusion = "skipped";
    log_->info("- " + sr.name + " (skipped)", Color::Gray);
    record_step(scope, step, std::move(sr));
    return;
  }
  log_->info("> " + sr.name, Color::Bold);

  StringMap env = env_before;
  bool continue_on_error = false;
  std::optional<std::chrono::milliseconds> timeout;
  try {
    for (const auto& [k, v] : evaluate_string_map(step.env, ctx, general, scope.file)) env[k] = v;
    ctx.contexts.set("env", to_object(env));
    if (step.continue_on_error != nullptr) {
      continue_on_error = evaluate_token(step.continue_on_error, ctx, general, scope.file).truthy();
    }
    if (step.timeout_minutes != nullptr) {
      timeout = minutes_to_ms(evaluate_token(step.timeout_minutes, ctx, general, scope.file).to_number());
    }
  } catch (const std::exception& e) {
    fail_step(e.what());
    return;
  }

  StepResult result;
  try {
    result = step.is_run() ? run_script(scope, step, ctx, env, timeout, after_cancel)
                           : run_uses(scope, step, name, ctx, env, timeout, after_cancel);
  } catch (const std::exception& e) {
    result.outcome = "failure";
    result.error = e.what();
    log_->error(e.what());
  }
  sr.outcome = result.outcome;
  sr.conclusion = result.outcome == "failure" && continue_on_error ? "success" : result.outcome;
  sr.outputs = std::move(result.outputs);
  sr.error = std::move(result.error);
  sr.duration_ms = elapsed_ms(step_started);
  const auto duration = util::format_duration_ms(sr.duration_ms);
  if (sr.outcome == "success") {
    log_->info("v " + sr.name + " (" + duration + ")", Color::Green);
  } else if (sr.outcome == "failure" && sr.conclusion == "success") {
    log_->info("! " + sr.name + " (failure, continue-on-error) (" + duration + ")", Color::Yellow);
  } else {
    log_->info("x " + sr.name + " (" + sr.outcome + ") (" + duration + ")", Color::Red);
  }
  record_step(scope, step, std::move(sr));
}

StepResult JobRunner::run_script(StepScope& scope, const workflow::Step& step, expr::EvalContext& ctx,
                                 const StringMap& env, std::optional<std::chrono::milliseconds> timeout,
                                 bool after_cancel) {
  const auto general = scope_for(scope.general_site);
  const auto body = evaluate_string(step.run, ctx, general, scope.file);
  std::string shell_name;
  std::string working_directory;
  if (step.shell != nullptr) shell_name = evaluate_string(step.shell, ctx, general, scope.file);
  if (step.working_directory != nullptr) {
    working_directory = evaluate_string(step.working_directory, ctx, general, scope.file);
  }
  if (!scope.composite) {
    // defaults.run of the job, then of the workflow.
    for (const auto* defaults : {job_.defaults, wf_.defaults}) {
      if (defaults == nullptr || !defaults->is_mapping()) continue;
      const auto* run = defaults->get("run");
      if (run == nullptr || !run->is_mapping()) continue;
      const auto site = scope_for(Site::JobDefaultsRun);
      if (shell_name.empty() && run->get("shell") != nullptr) {
        shell_name = evaluate_string(run->get("shell"), ctx, site, wf_.file);
      }
      if (working_directory.empty() && run->get("working-directory") != nullptr) {
        working_directory = evaluate_string(run->get("working-directory"), ctx, site, wf_.file);
      }
    }
  }
  const auto files = prepare_file_commands();
  auto process_env = step_environment(env, files, scope, step.id);
  const auto shell = resolve_shell(shell_name, process_env["PATH"]);
  fs::path cwd = workspace_;
  if (!working_directory.empty()) {
    const fs::path wd(working_directory);
    cwd = wd.is_absolute() ? wd : (workspace_ / wd).lexically_normal();
  }
  for (const auto& line : util::split_lines(body)) log_->info("  " + line, Color::Cyan);
  log_->info("  shell: " + shell.display, Color::Gray);
  std::error_code ec;
  if (!fs::is_directory(cwd, ec)) {
    StepResult r;
    r.outcome = "failure";
    r.error = "An error occurred trying to start process '" + shell.program + "' with working directory '" +
              cwd.string() + "'. No such file or directory";
    log_->error(r.error);
    return r;
  }
  const auto script = temp_ / (util::random_uuid() + shell.extension);
  util::write_file(script, shell.script(body));
  return execute(shell.argv(script), std::move(process_env), cwd, timeout, after_cancel, files, nullptr);
}

ResolvedAction JobRunner::resolve_action(const actions::ActionRef& ref) {
  ResolvedAction resolved;
  resolved.display = ref.display();
  if (ref.kind == actions::ActionRef::Kind::Local) {
    resolved.dir = util::real_path(workspace_ / ref.local_path);
    if (!util::is_subpath(resolved.dir, workspace_)) {
      throw actions::ActionError("The local action path '" + ref.local_path + "' is outside of the workspace");
    }
    return resolved;
  }
  auto* log = log_.get();
  const auto entry = run_.repositories.fetch(ref.owner, ref.repo, ref.ref, [log](std::string_view m) {
    log->info(std::string(m), Color::Gray);
  });
  resolved.dir = util::real_path(ref.path.empty() ? entry.dir : entry.dir / ref.path);
  if (!util::is_subpath(resolved.dir, util::real_path(entry.dir))) {
    throw actions::ActionError("The action path is outside of the repository: " + ref.path);
  }
  resolved.repository = ref.full_name();
  resolved.ref = ref.ref;
  return resolved;
}

StringMap JobRunner::action_inputs(const actions::Action& action, const workflow::Step& step,
                                   const StepScope& scope, expr::EvalContext& ctx) {
  StringMap values;
  std::vector<std::string> unexpected;
  for (const auto& [k, v] : evaluate_string_map(step.with, ctx, scope_for(scope.general_site), scope.file)) {
    if (const auto* in = action.find_input(k)) {
      values[in->name] = v;
      if (!in->deprecation_message.empty()) {
        log_->warning("Input '" + k + "' has been deprecated with message: " + in->deprecation_message);
      }
    } else {
      values[k] = v;
      unexpected.push_back(k);
    }
  }
  if (!unexpected.empty()) {
    std::vector<std::string> valid;
    for (const auto& in : action.inputs) valid.push_back("'" + in.name + "'");
    std::vector<std::string> quoted;
    for (const auto& u : unexpected) quoted.push_back("'" + u + "'");
    log_->warning("Unexpected input(s) " + util::join(quoted, ", ") + ", valid inputs are [" +
                  util::join(valid, ", ") + "]");
  }
  for (const auto& in : action.inputs) {
    if (values.count(in.name) != 0 || in.default_value == nullptr) continue;
    values[in.name] = evaluate_string(in.default_value, ctx, scope_for(Site::ActionInputDefault), action.file);
  }
  return values;
}

actions::BuiltinContext JobRunner::builtin_context() {
  actions::BuiltinContext bc;
  bc.workspace = workspace_;
  bc.home = home_;
  bc.writable = {workspace_, temp_, home_, tmp_, tool_cache_, file_commands_dir_};
  for (const auto& d : run_.options.sandbox.allow_write) {
    const auto writable = util::real_path(d);
    const auto control_root = control_dir_.parent_path();
    if (util::is_subpath(control_root, writable) || util::is_subpath(writable, control_root)) {
      throw EvaluationError("Writable builtin path overlaps the protected job control directory: " +
                            writable.string());
    }
    bc.writable.push_back(writable);
  }
  bc.cache_store = run_.options.cache_dir / "cache" / run_.repository;
  bc.artifact_store = wr_.artifacts;
  bc.repository = run_.repository;
  bc.ref = run_.event.info.ref;
  bc.sha = run_.event.sha;
  bc.token = run_.options.github_token;
  auto* run = &run_;
  bc.snapshot = [run]() -> const workspace::Snapshot& { return run->snapshot(); };
  auto* log = log_.get();
  bc.info = [log](std::string_view m) { log->info(std::string(m)); };
  bc.warning = [log](std::string_view m) { log->warning(m); };
  bc.next_artifact_id = [run]() { return ++run->artifact_ids; };
  bc.job_succeeded = current_status(nullptr) == "success";
  return bc;
}

StepResult JobRunner::run_uses(StepScope& scope, const workflow::Step& step, const std::string& name,
                               expr::EvalContext& ctx, const StringMap& env,
                               std::optional<std::chrono::milliseconds> timeout, bool after_cancel) {
  StepResult result;
  const auto uses = expr::contains_expression(step.uses)
                        ? evaluate_string(step.node != nullptr ? step.node->get("uses") : nullptr, ctx,
                                          scope_for(scope.general_site), scope.file)
                        : step.uses;
  const auto ref = actions::parse_action_ref(uses);
  StringMap overrides = scope.env;
  if (step.env != nullptr && step.env->is_mapping()) {
    for (const auto& e : step.env->entries) overrides[e.key->text] = env.at(e.key->text);
  }
  auto action_env = [&]() {
    auto values = global_env_;
    for (const auto& [k, v] : overrides) values[k] = v;
    return values;
  };
  if (ref.kind == actions::ActionRef::Kind::Docker) {
    throw actions::ActionError("Docker container actions are not supported by cpp-actions: " + uses);
  }
  if (const auto builtin = actions::find_builtin(ref); builtin != actions::Builtin::None) {
    StringMap inputs;
    for (const auto& [k, v] : evaluate_string_map(step.with, ctx, scope_for(scope.general_site), scope.file)) {
      inputs[util::to_lower(k)] = v;
    }
    log_->info("  (emulated by cpp-actions)", Color::Gray);
    const auto r = actions::run_builtin(builtin, false, inputs, {}, builtin_context());
    if (!r.ok) {
      result.outcome = "failure";
      result.error = r.error;
      log_->error(r.error);
    }
    result.outputs = r.outputs;
    if (r.ok && actions::builtin_has_post(builtin)) {
      PostStep post;
      post.name = "Post " + name;
      post.id = step.id;
      post.builtin = builtin;
      post.inputs = inputs;
      post.state = r.state;
      post.env = overrides;
      post.scope = &scope;
      post.condition_text = "success()";
      post_steps_.push_back(std::move(post));
    }
    return result;
  }

  const auto resolved = resolve_action(ref);
  auto action = std::make_unique<actions::Action>(actions::load_action(resolved.dir, resolved.display));
  for (const auto& w : action->warnings) log_->warning(w.to_string());
  const auto inputs = action_inputs(*action, step, scope, ctx);
  const auto* a = action.get();
  loaded_actions_.push_back(std::move(action));
  switch (a->kind) {
    case actions::ActionKind::Docker:
      throw actions::ActionError("Docker container actions are not supported by cpp-actions: " + uses);
    case actions::ActionKind::Composite:
      return run_composite(scope, name, *a, resolved, inputs, overrides);
    case actions::ActionKind::Node:
      break;
  }
  StringMap state;
  std::optional<std::size_t> post_index;
  if (!a->post.empty()) {
    PostStep post;
    post.name = "Post " + name;
    post.id = step.id;
    post.action = a;
    post.resolved = resolved;
    post.inputs = inputs;
    post.env = overrides;
    post.scope = &scope;
    post.condition = a->post_if;
    post.file = a->file;
    post_index = post_steps_.size();
    post_steps_.push_back(std::move(post));
  }
  try {
    if (!a->pre.empty()) {
      yaml::Node always;
      always.kind = yaml::NodeKind::String;
      always.text = "always()";
      const auto* cond = a->pre_if != nullptr ? a->pre_if : &always;
      if (evaluate_condition(cond, ctx, scope_for(scope.if_site), a->file)) {
        log_->info("  pre: " + a->pre, Color::Gray);
        result = run_node(scope, step.id, *a, resolved, inputs, a->pre, state, action_env(), timeout, after_cancel);
      }
    }
    if (result.outcome == "success") {
      result = run_node(scope, step.id, *a, resolved, inputs, a->main, state, action_env(), timeout, after_cancel);
    }
  } catch (...) {
    if (post_index) post_steps_[*post_index].state = state;
    throw;
  }
  if (post_index) post_steps_[*post_index].state = std::move(state);
  return result;
}

StepResult JobRunner::run_node(const StepScope& scope, const std::string& step_id, const actions::Action& action,
                               const ResolvedAction& resolved, const StringMap& inputs, const std::string& file,
                               StringMap& state, const StringMap& env,
                               std::optional<std::chrono::milliseconds> timeout, bool after_cancel) {
  auto* log = log_.get();
  const auto script = util::real_path(resolved.dir / file);
  if (!util::is_subpath(script, resolved.dir)) throw actions::ActionError("Invalid action entry point: " + file);
  const auto node = run_.node.get(actions::node_major_version(action.runtime),
                                  [log](std::string_view m) { log->warning(m); });
  const auto files = prepare_file_commands();
  StepScope action_scope = scope;
  action_scope.action_path = resolved.dir;
  action_scope.action_repository = resolved.repository;
  action_scope.action_ref = resolved.ref;
  auto process_env = step_environment(env, files, action_scope, step_id);
  process_env["GITHUB_ACTION_PATH"] = resolved.dir.string();
  process_env["GITHUB_ACTION_REPOSITORY"] = resolved.repository;
  process_env["GITHUB_ACTION_REF"] = resolved.ref;
  for (const auto& [k, v] : inputs) {
    process_env["INPUT_" + util::to_upper(util::replace_all(k, " ", "_"))] = v;
  }
  for (const auto& [k, v] : state) process_env["STATE_" + k] = v;
  // Cached installations are already readable; an explicit binary must not
  // implicitly grant access to its parent directories (e.g. the host HOME).
  return execute({node.string(), script.string()}, std::move(process_env), workspace_, timeout, after_cancel, files,
                 &state, {node});
}

StepResult JobRunner::run_composite(StepScope& parent, const std::string& name,
                                    const actions::Action& action, const ResolvedAction& resolved,
                                    const StringMap& inputs, const StringMap& step_env) {
  if (parent.depth + 1 > kMaxCompositeDepth) {
    throw actions::ActionError("Composite actions can be nested up to " + std::to_string(kMaxCompositeDepth) +
                               " levels");
  }
  composite_scopes_.push_back(std::make_unique<StepScope>());
  auto& child = *composite_scopes_.back();
  child.composite = true;
  child.depth = parent.depth + 1;
  child.file = action.file;
  child.action_path = resolved.dir;
  child.action_repository = resolved.repository;
  child.action_ref = resolved.ref;
  child.inputs = to_object(inputs);
  child.if_site = Site::ActionStepIf;
  child.general_site = Site::ActionStepGeneral;
  child.prefix = parent.prefix + name + " / ";
  // The env of the composite step (not the global env, which may change).
  child.env = step_env;
  for (const auto& nested : action.steps) run_step(child, nested);
  StepResult result;
  if (child.status == "failure") {
    result.outcome = "failure";
  } else if (is_cancelled()) {
    result.outcome = "cancelled";
  }
  if (!action.outputs.empty()) {
    auto ctx = make_context(child, scope_env(child));
    for (const auto& out : action.outputs) {
      if (out.value == nullptr) continue;
      try {
        result.outputs[out.name] = evaluate_string(out.value, ctx, scope_for(Site::ActionOutputs), action.file);
      } catch (const std::exception& e) {
        log_->error(e.what());
        result.outcome = "failure";
      }
    }
  }
  return result;
}

void JobRunner::run_post_steps() {
  running_post_ = true;
  for (auto it = post_steps_.rbegin(); it != post_steps_.rend(); ++it) {
    auto& post = *it;
    StepReport sr;
    sr.id = post.id;
    sr.name = post.name;
    sr.post = true;
    const auto step_started = Clock::now();
    const auto& scope = post.scope != nullptr ? *post.scope : top_;
    auto env = global_env_;
    for (const auto& [k, v] : post.env) env[k] = v;
    auto ctx = make_context(scope, env);
    yaml::Node text;
    text.kind = yaml::NodeKind::String;
    text.text = post.condition_text;
    bool run_it = false;
    try {
      run_it = evaluate_condition(post.condition != nullptr ? post.condition : &text, ctx, scope_for(scope.if_site),
                                  post.file.empty() ? wf_.file : post.file);
    } catch (const std::exception& e) {
      log_->error(e.what());
      sr.outcome = sr.conclusion = "failure";
      sr.error = e.what();
      status_ = "failure";
      report_.steps.push_back(std::move(sr));
      continue;
    }
    if (!run_it) {
      sr.outcome = sr.conclusion = "skipped";
      log_->info("- " + sr.name + " (skipped)", Color::Gray);
      report_.steps.push_back(std::move(sr));
      continue;
    }
    log_->info("> " + sr.name, Color::Bold);
    StepResult result;
    try {
      if (post.builtin != actions::Builtin::None) {
        const auto r = actions::run_builtin(post.builtin, true, post.inputs, post.state, builtin_context());
        if (!r.ok) {
          result.outcome = "failure";
          result.error = r.error;
          log_->error(r.error);
        }
      } else {
        result = run_node(scope, post.id, *post.action, post.resolved, post.inputs, post.action->post, post.state,
                          env, std::nullopt, true);
      }
    } catch (const std::exception& e) {
      result.outcome = "failure";
      result.error = e.what();
      log_->error(e.what());
    }
    sr.outcome = sr.conclusion = result.outcome;
    sr.error = result.error;
    sr.duration_ms = elapsed_ms(step_started);
    log_->info((sr.outcome == "success" ? "v " : "x ") + sr.name + " (" + util::format_duration_ms(sr.duration_ms) +
                   ")",
               sr.outcome == "success" ? Color::Green : Color::Red);
    if (sr.outcome == "failure" && status_ == "success") status_ = "failure";
    report_.steps.push_back(std::move(sr));
  }
  running_post_ = false;
}

// -------------------------------------------------------------- processes

FileCommands JobRunner::prepare_file_commands() {
  const auto id = util::random_uuid();
  FileCommands f{file_commands_dir_ / ("set_env_" + id), file_commands_dir_ / ("set_output_" + id),
                 file_commands_dir_ / ("add_path_" + id), file_commands_dir_ / ("save_state_" + id),
                 file_commands_dir_ / ("step_summary_" + id)};
  for (const auto& p : {f.env, f.output, f.path, f.state, f.summary}) util::write_file(p, "");
  return f;
}

util::Environment JobRunner::step_environment(const StringMap& env, const FileCommands& files,
                                              const StepScope& scope, const std::string& action_id) {
  util::Environment out = base_env_;
  for (const auto& [k, v] : env) out[k] = v;
  std::string path = out["PATH"];
  for (const auto& p : path_prepend_) path = p + (path.empty() ? "" : ":") + path;
  out["PATH"] = path;
  const auto& ev = run_.event;
  auto type = ref_type(ev.info.ref);
  if (type.empty() && !ev.info.ref.empty()) type = "branch";
  const StringMap runtime = {
      {"GITHUB_ACTIONS", "true"},
      {"GITHUB_ACTION", action_id},
      {"GITHUB_ACTOR", ev.actor},
      {"GITHUB_ACTOR_ID", "0"},
      {"GITHUB_API_URL", "https://api.github.com"},
      {"GITHUB_BASE_REF", ev.info.base_ref},
      {"GITHUB_ENV", files.env.string()},
      {"GITHUB_EVENT_NAME", ev.info.name},
      {"GITHUB_EVENT_PATH", event_path_.string()},
      {"GITHUB_GRAPHQL_URL", "https://api.github.com/graphql"},
      {"GITHUB_HEAD_REF", ev.head_ref},
      {"GITHUB_JOB", job_.id},
      {"GITHUB_OUTPUT", files.output.string()},
      {"GITHUB_PATH", files.path.string()},
      {"GITHUB_REF", ev.info.ref},
      {"GITHUB_REF_NAME", ref_short_name(ev.info.ref)},
      {"GITHUB_REF_PROTECTED", "false"},
      {"GITHUB_REF_TYPE", type},
      {"GITHUB_REPOSITORY", run_.repository},
      {"GITHUB_REPOSITORY_ID", "0"},
      {"GITHUB_REPOSITORY_OWNER", run_.repository.substr(0, run_.repository.find('/'))},
      {"GITHUB_REPOSITORY_OWNER_ID", "0"},
      {"GITHUB_RETENTION_DAYS", "90"},
      {"GITHUB_RUN_ATTEMPT", "1"},
      {"GITHUB_RUN_ID", wr_.run_id},
      {"GITHUB_RUN_NUMBER", wr_.run_number},
      {"GITHUB_SERVER_URL", "https://github.com"},
      {"GITHUB_SHA", ev.sha},
      {"GITHUB_STATE", files.state.string()},
      {"GITHUB_STEP_SUMMARY", files.summary.string()},
      {"GITHUB_TRIGGERING_ACTOR", ev.actor},
      {"GITHUB_WORKFLOW", wr_.name},
      {"GITHUB_WORKFLOW_REF", wr_.workflow_ref},
      {"GITHUB_WORKFLOW_SHA", ev.sha},
      {"GITHUB_WORKSPACE", workspace_.string()},
      {"RUNNER_ARCH", runner_arch()},
      {"RUNNER_ENVIRONMENT", "github-hosted"},
      {"RUNNER_NAME", "cpp-actions"},
      {"RUNNER_OS", runner_os()},
      {"RUNNER_TEMP", temp_.string()},
      {"RUNNER_TOOL_CACHE", tool_cache_.string()},
      {"RUNNER_WORKSPACE", workspace_.parent_path().string()},
      {"AGENT_TOOLSDIRECTORY", tool_cache_.string()},
  };
  for (const auto& [k, v] : runtime) out[k] = v;
  if (run_.options.debug) out["RUNNER_DEBUG"] = "1";
  if (scope.composite || !scope.action_path.empty()) out["GITHUB_ACTION_PATH"] = scope.action_path.string();
  if (sandbox_) {
    for (const auto& [k, v] : sandbox_->environment()) out[k] = v;
  }
  return out;
}

StepResult JobRunner::execute(std::vector<std::string> argv, util::Environment env, const fs::path& cwd,
                              std::optional<std::chrono::milliseconds> timeout, bool after_cancel,
                              const FileCommands& files, StringMap* state, const std::vector<fs::path>& extra_read) {
  StepResult result;
  if (sandbox_) argv = sandbox_->wrap(argv, extra_read);
  CommandState cs;
  cs.allow_unsecure = util::iequals(env["ACTIONS_ALLOW_UNSECURE_COMMANDS"], "true");
  bool job_timeout = false;
  if (deadline_ && !running_post_) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline_ - Clock::now());
    const auto limit = std::max(remaining, std::chrono::milliseconds(1));
    if (!timeout || limit < *timeout) {
      timeout = limit;
      job_timeout = true;
    }
  }
  util::ProcessSpec spec;
  spec.argv = std::move(argv);
  spec.env = std::move(env);
  spec.cwd = cwd;
  spec.timeout = timeout;
  spec.cancel = after_cancel ? &inst_.force : &inst_.cancel;
  spec.on_line = [&](std::string_view line, bool) { handle_line(line, cs); };
  spec.on_spawn = [this](pid_t pgid) {
    std::lock_guard lock(pgid_mutex_);
    pgids_.push_back(pgid);
  };
  const auto pr = util::run_process(spec);
  stop_token_.clear();
  echo_ = false;
  if (pr.spawn_failed) {
    result.outcome = "failure";
    result.error = pr.error;
    log_->error(pr.error);
    return result;
  }
  process_file_commands(files, result, state);
  for (const auto& [k, v] : cs.outputs) result.outputs.emplace(k, v);
  if (state != nullptr) {
    for (const auto& [k, v] : cs.state) (*state)[k] = v;
  }
  if (pr.cancelled) {
    result.outcome = "cancelled";
    result.error = "The operation was canceled.";
    log_->error(result.error);
  } else if (pr.timed_out) {
    result.outcome = "failure";
    if (job_timeout) {
      timed_out_ = true;
      status_ = "cancelled";
      result.error = "The job has exceeded the maximum execution time of " + util::format_number(timeout_minutes_) +
                     " minutes.";
    } else {
      result.error = "The action has timed out.";
    }
    log_->error(result.error);
  } else if (pr.exit_code != 0) {
    result.outcome = "failure";
    result.error = "Process completed with exit code " + std::to_string(pr.exit_code) + ".";
    log_->error(result.error);
  }
  if (cs.failed && result.outcome == "success") {
    result.outcome = "failure";
    result.error = "A workflow command failed";
  }
  return result;
}

void JobRunner::process_file_commands(const FileCommands& files, StepResult& result, StringMap* state) {
  try {
    for (const auto& [k, v] : parse_key_value_file(util::read_file(files.env))) {
      if (util::iequals(k, "NODE_OPTIONS")) {
        log_->warning("Can't store NODE_OPTIONS output parameter using '$GITHUB_ENV' command.");
        continue;
      }
      global_env_[k] = v;
    }
    for (const auto& [k, v] : parse_key_value_file(util::read_file(files.output))) result.outputs[k] = v;
    for (const auto& p : parse_path_file(util::read_file(files.path))) {
      std::erase(path_prepend_, p);
      path_prepend_.push_back(p);
    }
    for (const auto& [k, v] : parse_key_value_file(util::read_file(files.state))) {
      if (state != nullptr) (*state)[k] = v;
    }
    const auto summary = util::read_file(files.summary);
    if (summary.size() > kMaxSummarySize) {
      const std::string message = "$GITHUB_STEP_SUMMARY upload aborted, supports content up to a size of 1024k, got " +
                                  std::to_string(summary.size() / 1024) + "k.";
      log_->error(message);
      report_.annotations.push_back({"error", message, {}, {}, 0, 0, 0, 0});
    } else if (!util::trim(summary).empty()) {
      report_.summary += run_.masker.mask(summary);
      if (!report_.summary.ends_with("\n")) report_.summary += "\n";
    }
  } catch (const FileCommandError& e) {
    result.outcome = "failure";
    result.error = e.what();
    log_->error(e.what());
  }
  for (const auto& p : {files.env, files.output, files.path, files.state, files.summary}) {
    std::error_code ec;
    fs::remove(p, ec);
  }
}

void JobRunner::annotate(const std::string& level, const WorkflowCommand& cmd) {
  Annotation a;
  a.level = level;
  a.message = run_.masker.mask(cmd.message);
  auto prop = [&](const char* name) -> std::string {
    auto it = cmd.properties.find(name);
    return it != cmd.properties.end() ? it->second : std::string();
  };
  auto num = [&](const char* name) { return static_cast<int>(util::parse_int(prop(name)).value_or(0)); };
  a.title = prop("title");
  a.file = prop("file");
  a.line = num("line");
  a.end_line = num("endLine");
  a.column = num("col");
  a.end_column = num("endColumn");
  std::string text = cmd.message;
  if (!a.file.empty()) {
    std::string where = a.file;
    if (a.line > 0) where += ":" + std::to_string(a.line);
    if (a.column > 0) where += ":" + std::to_string(a.column);
    text = where + ": " + text;
  }
  if (!a.title.empty()) text = a.title + ": " + text;
  if (level == "error") {
    log_->error(text);
  } else if (level == "warning") {
    log_->warning(text);
  } else {
    log_->notice(text);
  }
  report_.annotations.push_back(std::move(a));
}

void JobRunner::handle_line(std::string_view line, CommandState& cs) {
  if (!stop_token_.empty()) {
    if (line == "::" + stop_token_ + "::") {
      stop_token_.clear();
    } else {
      log_->output(line);
    }
    return;
  }
  const auto cmd = parse_workflow_command(line);
  if (!cmd) {
    log_->output(line);
    return;
  }
  if (echo_ && cmd->name != "add-mask") log_->output(line);
  const auto& name = cmd->name;
  if (name == "add-mask") {
    run_.masker.add(cmd->message);
  } else if (name == "debug") {
    log_->debug(cmd->message);
  } else if (name == "warning" || name == "error" || name == "notice") {
    annotate(name, *cmd);
  } else if (name == "group") {
    log_->group(cmd->message);
  } else if (name == "endgroup") {
    log_->endgroup();
  } else if (name == "echo") {
    echo_ = util::iequals(util::trim(cmd->message), "on");
  } else if (name == "stop-commands") {
    stop_token_ = cmd->message;
  } else if (name == "set-output" || name == "save-state") {
    auto it = cmd->properties.find("name");
    if (it == cmd->properties.end() || it->second.empty()) {
      log_->error("Unable to process command '" + std::string(line) + "' successfully. Missing 'name'.");
      return;
    }
    log_->warning("The `" + name +
                  "` command is deprecated and will be disabled soon. Please upgrade to using Environment Files.");
    (name == "set-output" ? cs.outputs : cs.state)[it->second] = cmd->message;
  } else if (name == "set-env" || name == "add-path") {
    if (!cs.allow_unsecure) {
      cs.failed = true;
      log_->error("Unable to process command '" + std::string(line) + "' successfully.");
      log_->error("The `" + name +
                  "` command is disabled. Please upgrade to using Environment Files or opt into unsecure command "
                  "execution by setting the `ACTIONS_ALLOW_UNSECURE_COMMANDS` environment variable to `true`.");
      return;
    }
    if (name == "add-path") {
      std::erase(path_prepend_, cmd->message);
      path_prepend_.push_back(cmd->message);
    } else if (auto it = cmd->properties.find("name"); it != cmd->properties.end()) {
      global_env_[it->second] = cmd->message;
    }
  } else {
    log_->debug("Ignoring the workflow command '" + name + "'");
  }
}

// ---------------------------------------------------------------- results

void JobRunner::evaluate_outputs() {
  if (job_.outputs == nullptr || !job_.outputs->is_mapping()) return;
  auto ctx = make_context(top_, global_env_);
  for (const auto& e : job_.outputs->entries) {
    try {
      const auto value = evaluate_string(e.value, ctx, scope_for(Site::JobOutputs), wf_.file);
      if (run_.masker.contains_secret(value)) {
        log_->warning("Skip output '" + e.key->text + "' since it may contain secret.");
        continue;
      }
      report_.outputs[e.key->text] = value;
    } catch (const std::exception& ex) {
      log_->error(ex.what());
    }
  }
}

void JobRunner::finish() {
  {
    std::lock_guard lock(pgid_mutex_);
    for (const auto pgid : pgids_) {
      if (util::process_group_alive(pgid)) util::kill_process_group(pgid);
    }
  }
  sandbox_.reset();
  if (!run_.options.keep_workdir && !job_dir_.empty()) {
    try {
      util::remove_tree(job_dir_);
      util::remove_tree(control_dir_);
    } catch (const std::exception& e) {
      if (log_) log_->warning(std::string("Failed to remove the job directory: ") + e.what());
    }
  }
}

JobReport JobRunner::run() {
  report_.key = inst_.key;
  report_.job_id = job_.id;
  report_.name = inst_.name;
  report_.matrix = inst_.matrix;
  try {
    if (!setup()) {
      report_.duration_ms = elapsed_ms(started_);
      finish();
      return report_;
    }
  } catch (const std::exception& e) {
    if (!log_) log_ = std::make_unique<JobLog>(run_.console, run_.masker, inst_.name, fs::path{}, false);
    log_->error(e.what());
    report_.result = "failure";
    report_.reason = e.what();
    report_.duration_ms = elapsed_ms(started_);
    finish();
    return report_;
  }
  for (const auto& step : job_.steps) run_step(top_, step);
  if (top_.status == "failure" && status_ == "success") status_ = "failure";
  run_post_steps();
  evaluate_outputs();
  if (timed_out_) {
    report_.result = "failure";
    report_.reason = "The job has exceeded the maximum execution time";
  } else if (status_ == "cancelled" || is_cancelled()) {
    report_.result = "cancelled";
  } else {
    report_.result = status_ == "failure" ? "failure" : "success";
  }
  report_.duration_ms = elapsed_ms(started_);
  const auto color = report_.result == "success" ? Color::Green : Color::Red;
  log_->info("Job " + report_.result + " (" + util::format_duration_ms(report_.duration_ms) + ")", color);
  if (continue_on_error_ && report_.result == "failure") {
    log_->info("The job failed but continue-on-error is set", Color::Yellow);
  }
  finish();
  return report_;
}

}  // namespace

// ------------------------------------------------------------- RunState

void Semaphore::acquire() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return count_ > 0; });
  --count_;
}

void Semaphore::release() {
  {
    std::lock_guard lock(mutex_);
    ++count_;
  }
  cv_.notify_one();
}

namespace {

std::string repository_name(const workspace::GitInfo& git) {
  if (git.github) return git.github->full_name();
  auto name = git.root.filename().string();
  if (name.empty()) name = "repository";
  return "local/" + name;
}

int default_parallel(int requested) {
  if (requested > 0) return requested;
  const auto n = static_cast<int>(std::thread::hardware_concurrency());
  return n > 0 ? n : 2;
}

}  // namespace

RunState::RunState(RunOptions opts, Console& con, Event ev, workspace::GitInfo g)
    : options(std::move(opts)),
      console(con),
      event(std::move(ev)),
      git(std::move(g)),
      repository(repository_name(git)),
      slots(default_parallel(options.parallel)),
      repositories(options.cache_dir / "actions", options.github_token),
      node(options.node, options.cache_dir / "node") {
  for (const auto& [k, v] : options.secrets) masker.add(v);
  if (!options.github_token.empty()) masker.add(options.github_token);
}

const workspace::Snapshot& RunState::snapshot() {
  std::lock_guard lock(snapshot_mutex_);
  if (snapshot_) return *snapshot_;
  if (!snapshot_error_.empty()) throw workspace::SnapshotError(snapshot_error_);
  try {
    snapshot_ = workspace::create_snapshot(git, options.run_dir / "snapshot", event.info.ref);
  } catch (const std::exception& e) {
    snapshot_error_ = e.what();
    throw workspace::SnapshotError(snapshot_error_);
  }
  return *snapshot_;
}

void RunState::register_cancel(std::atomic<bool>* cancel, std::atomic<bool>* force) {
  std::lock_guard lock(flags_mutex_);
  flags_.emplace_back(cancel, force);
  if (cancelled) cancel->store(true);
  if (force_cancelled) force->store(true);
}

void RunState::unregister_cancel(std::atomic<bool>* cancel) {
  std::lock_guard lock(flags_mutex_);
  std::erase_if(flags_, [cancel](const auto& p) { return p.first == cancel; });
}

void RunState::cancel_all(bool force) {
  std::lock_guard lock(flags_mutex_);
  cancelled = true;
  if (force) force_cancelled = true;
  for (auto& [cancel, force_flag] : flags_) {
    cancel->store(true);
    if (force) force_flag->store(true);
  }
}

// ----------------------------------------------------------------- helpers

std::string sanitize_key(std::string_view key) {
  std::string out;
  for (char c : key) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
      out += c;
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
  if (out.empty()) out = "job";
  if (out.size() > 100) out = out.substr(0, 80) + "-" + util::sha256_hex(key).substr(0, 16);
  return out;
}

std::string hash_files(const fs::path& root, const std::vector<std::string>& patterns) {
  const auto files = util::glob_files(root, patterns);
  if (files.empty()) return {};
  util::Sha256 h;
  for (const auto& f : files) {
    const auto d = util::sha256_file(f);
    h.update(d.data(), d.size());
  }
  return util::Sha256::hex(h.digest());
}

std::function<bool(std::string_view)> job_status_functions(const JobStatus& status) {
  return [status](std::string_view fn) {
    if (fn == "always") return true;
    if (fn == "cancelled") return status.cancelled;
    if (fn == "failure") return status.failure && !status.cancelled;
    if (fn == "success") return status.success && !status.cancelled;
    return false;
  };
}

Value to_object(const std::map<std::string, std::string>& values) {
  auto obj = Value::object();
  for (const auto& [k, v] : values) obj.set(k, v);
  return obj;
}

Value github_context(const RunState& run, const WorkflowRun& wr, const std::string& job_id,
                     const fs::path& workspace, const fs::path& event_path) {
  auto g = Value::object();
  const auto& ev = run.event;
  const auto& repo = run.repository;
  auto type = ref_type(ev.info.ref);
  if (type.empty() && !ev.info.ref.empty()) type = "branch";
  g.set("action", "");
  g.set("action_path", "");
  g.set("action_ref", "");
  g.set("action_repository", "");
  g.set("action_status", "");
  g.set("actor", ev.actor);
  g.set("actor_id", "0");
  g.set("api_url", "https://api.github.com");
  g.set("base_ref", ev.info.base_ref);
  g.set("env", "");
  g.set("event", wr.payload);
  g.set("event_name", ev.info.name);
  g.set("event_path", event_path.string());
  g.set("graphql_url", "https://api.github.com/graphql");
  g.set("head_ref", ev.head_ref);
  g.set("job", job_id);
  g.set("path", "");
  g.set("ref", ev.info.ref);
  g.set("ref_name", ref_short_name(ev.info.ref));
  g.set("ref_protected", false);
  g.set("ref_type", type);
  g.set("repository", repo);
  g.set("repository_id", "0");
  g.set("repository_owner", repo.substr(0, repo.find('/')));
  g.set("repository_owner_id", "0");
  g.set("repositoryUrl", "git://github.com/" + repo + ".git");
  g.set("retention_days", "90");
  g.set("run_attempt", "1");
  g.set("run_id", wr.run_id);
  g.set("run_number", wr.run_number);
  g.set("secret_source", "Actions");
  g.set("server_url", "https://github.com");
  g.set("sha", ev.sha);
  g.set("token", run.options.github_token);
  g.set("triggering_actor", ev.actor);
  g.set("workflow", wr.name);
  g.set("workflow_ref", wr.workflow_ref);
  g.set("workflow_sha", ev.sha);
  g.set("workspace", workspace.string());
  return g;
}

JobReport run_job_instance(JobInstance& instance) {
  JobRunner runner(instance);
  return runner.run();
}

}  // namespace cpa::engine
