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
#include "workspace/git.hpp"

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

std::chrono::milliseconds minutes_to_ms(double minutes) {
  const double milliseconds = minutes * 60000.0;
  const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max() / 2).count();
  if (!(minutes > 0) || !std::isfinite(milliseconds) || milliseconds >= static_cast<double>(maximum)) {
    throw EvaluationError("'timeout-minutes' must be a finite positive number within the supported duration range");
  }
  return std::chrono::milliseconds(std::max<std::int64_t>(1, static_cast<std::int64_t>(milliseconds)));
}

struct StepScope {
  Value steps = Value::object();
  Value inputs = Value::object();
  std::string status = "success";  // failures of the steps of this scope
  bool composite = false;
  bool setup = false;
  std::string file;
  fs::path action_path;
  std::string action_repository;
  std::string action_ref;
  bool allow_apple_security = false;
  bool allow_docker = false;
  bool allow_msbuild = false;
  bool allow_unity = false;
  bool allow_xcode = false;
  StringMap env;  // env of the composite step (inherited by the nested steps)
  int depth = 0;
  std::string prefix;
  Site if_site = Site::StepIf;
  Site general_site = Site::StepGeneral;
};

struct StepResult {
  std::string outcome = "success";
  StringMap outputs;
  StringMap environment;
  std::vector<std::string> paths;
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
  StringMap environment;
  std::vector<std::string> paths;
  bool failed = false;
  bool allow_unsecure = false;
};

struct ResolvedAction {
  fs::path dir;
  fs::path repository_root;
  std::string repository;  // owner/repo ("" for local actions)
  std::string ref;
  std::string display;
  bool allow_apple_security = false;
  bool allow_docker = false;
  bool allow_msbuild = false;
  bool allow_unity = false;
  bool allow_xcode = false;
  bool hosted_python = false;
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
  void inherit_unity_editor_prefs();
  bool evaluate_job_settings();
  void load_environment(const std::string& name);
  void start_sandbox();
  void prefetch_actions();
  std::vector<fs::path> writable_directories() const;
  void prepare_hosted_python_action(const ResolvedAction& resolved, const actions::Action& action);
  void validate_hosted_python_action(const ResolvedAction& resolved) const;
  void register_hosted_python_installations(const StepResult& result);

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
                     StringMap* state, const std::vector<fs::path>& extra_read = {},
                     bool allow_apple_security = false, bool allow_msbuild = false, bool allow_unity = false,
                     bool allow_docker = false, bool hosted_python = false, bool allow_xcode = false);
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
  bool setup_failed_ = false;
  bool allow_xcode_simulator_ = false;

  fs::path job_dir_;
  fs::path control_dir_;
  fs::path work_root_;
  fs::path short_work_dir_;
  fs::path workspace_;
  fs::path temp_;
  fs::path home_;
  fs::path tmp_;
  fs::path unity_tmp_;
  fs::path file_commands_dir_;
  fs::path event_path_;
  fs::path tool_cache_;

  util::Environment base_env_;
  StringMap global_env_;
  std::vector<std::string> path_prepend_;
  std::vector<fs::path> hosted_python_reads_;
  Value secrets_ = Value::object();
  Value vars_ = Value::object();
  StringMap secret_values_;
  Value github_;
  Value runner_ctx_;
  std::string status_ = "success";
  bool timed_out_ = false;
  bool running_post_ = false;
  std::unique_ptr<sandbox::Sandbox> sandbox_;
  std::vector<PostStep> post_steps_;
  std::vector<std::unique_ptr<actions::Action>> loaded_actions_;
  std::map<fs::path, fs::path> action_copies_;
  std::vector<std::unique_ptr<StepScope>> composite_scopes_;
  bool echo_ = false;
  std::string stop_token_;
  StepScope top_;
};

// ------------------------------------------------------------------ setup

void JobRunner::create_directories() {
  const auto key = sanitize_key(inst_.key);
  job_dir_ = run_.options.run_dir / "jobs" / wr_.run_id / key;
  control_dir_ = run_.options.run_dir / "control" / wr_.run_id / key;
  const std::string repo_name = run_.repository.substr(run_.repository.find('/') + 1);
#if defined(__APPLE__)
  short_work_dir_ = util::make_temp_dir("cpa-w-", "/private/tmp");
#else
  short_work_dir_ = util::make_temp_dir("cpa-w-", "/tmp");
#endif
  work_root_ = short_work_dir_ / "work";
  workspace_ = work_root_ / repo_name / repo_name;
  temp_ = work_root_ / "_temp";
  home_ = job_dir_ / "home";
  tmp_ = job_dir_ / "tmp";
  file_commands_dir_ = job_dir_ / "file_commands";
  event_path_ = temp_ / "_github_workflow" / "event.json";
  tool_cache_ = run_.options.cache_dir / "toolcache";
  util::remove_tree(job_dir_);
  util::remove_tree(control_dir_);
  fs::create_directories(control_dir_);
  for (const auto& d : {workspace_, temp_, home_, tmp_, file_commands_dir_, event_path_.parent_path(), tool_cache_,
                        job_dir_ / "scripts"}) {
    fs::create_directories(d);
  }
#if defined(__APPLE__)
  fs::create_directories(home_ / "Library" / "Preferences");
#endif
  // Canonical paths (e.g. /var -> /private/var on macOS) for the sandbox rules.
  job_dir_ = util::real_path(job_dir_);
  control_dir_ = util::real_path(control_dir_);
  work_root_ = short_work_dir_ / "work";
  workspace_ = work_root_ / repo_name / repo_name;
  temp_ = work_root_ / "_temp";
  home_ = job_dir_ / "home";
  tmp_ = job_dir_ / "tmp";
  file_commands_dir_ = job_dir_ / "file_commands";
  event_path_ = temp_ / "_github_workflow" / "event.json";
  tool_cache_ = util::real_path(tool_cache_);
  const auto control_root = util::real_path(run_.options.run_dir / "control");
  for (const auto& d : {workspace_, temp_, home_, tmp_, tool_cache_,
                        run_.options.cache_dir / "cache" / run_.repository, wr_.artifacts}) {
    const auto allocated = util::real_path(d);
    if (util::is_subpath(control_root, allocated) || util::is_subpath(allocated, control_root)) {
      throw EvaluationError("Allocated action directory overlaps the protected job control directory: " +
                            allocated.string());
    }
  }
  util::write_file(event_path_, wr_.payload.to_json_string(true));
  util::write_file(job_dir_ / "work-path", work_root_.string() + "\n");
  util::write_file(home_ / ".gitconfig", workspace::default_git_config());
  // Stop Node's package-scope lookup before it reaches host directories.
  util::write_file(job_dir_ / "package.json", "{}\n");
  util::write_file(short_work_dir_ / "package.json", "{}\n");
#if defined(__APPLE__)
  if (run_.options.sandbox.enabled) {
    const auto wrapper = job_dir_ / "scripts/bin/mktemp";
    util::write_file(wrapper, R"sh(#!/bin/bash
set -e
original=("$@")
args=()
operands=false
prefix_template=""
while (($#)); do
  case "$1" in
    --tmpdir|--tmpdir=*)
      exec /usr/bin/mktemp "${original[@]}"
      ;;
    --)
      args+=("$1")
      shift
      operands=true
      break
      ;;
    --directory) args+=("-d") ;;
    --quiet) args+=("-q") ;;
    --dry-run) args+=("-u") ;;
    -?*)
      option=${1#-}
      while test -n "$option"; do
        flag=${option:0:1}
        option=${option:1}
        case "$flag" in
          d|q|u) args+=("-$flag") ;;
          p) exec /usr/bin/mktemp "${original[@]}" ;;
          t)
            if test -n "$option"; then
              prefix=$option
            else
              shift
              if (($# == 0)); then
                exec /usr/bin/mktemp "${original[@]}"
              fi
              prefix=$1
            fi
            prefix_template="${TMPDIR:?TMPDIR must be set}/${prefix}.XXXXXXXXXX"
            option=""
            ;;
          *) exec /usr/bin/mktemp "${original[@]}" ;;
        esac
      done
      ;;
    *)
      operands=true
      break
      ;;
  esac
  shift
done
if "$operands"; then
  args+=("$@")
fi
if test -n "$prefix_template"; then
  args+=("$prefix_template")
elif ! "$operands"; then
  args+=("${TMPDIR:?TMPDIR must be set}/tmp.XXXXXXXXXX")
fi
exec /usr/bin/mktemp "${args[@]}"
)sh");
    util::make_executable(wrapper);
  }
#endif
}

void JobRunner::build_base_environment() {
  const auto host = util::current_environment();
  base_env_["PATH"] = default_path();
  base_env_["HOME"] = home_.string();
#if defined(__APPLE__)
  base_env_["CFFIXED_USER_HOME"] = home_.string();
#endif
  base_env_["GRADLE_USER_HOME"] = (home_ / ".gradle").string();
  base_env_["TMPDIR"] = tmp_.string();
#if defined(__APPLE__)
  base_env_["LANG"] = "en_US.UTF-8";
#else
  base_env_["LANG"] = "C.UTF-8";
#endif
  base_env_["USER"] = "runner";
  base_env_["LOGNAME"] = "runner";
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
  auto it = std::find_if(run_.options.environments.begin(), run_.options.environments.end(),
                         [&](const auto& entry) { return util::iequals(entry.first, name); });
  log_->info("Environment protection rules and deployment APIs are not emulated", Color::Gray);
  if (it == run_.options.environments.end()) {
    log_->info("Environment '" + name + "' is not configured; using the repository secrets and variables",
               Color::Gray);
    return;
  }
  const auto& env = it->second;
  if (env.secrets_file && !env.secrets_file->empty()) {
    for (const auto& [k, v] : config::load_dotenv(config::expand_user(*env.secrets_file))) {
      secrets_.set(k, v);
      run_.masker.add(v);
    }
  }
  if (env.vars_file && !env.vars_file->empty()) {
    for (const auto& [k, v] : config::load_dotenv(config::expand_user(*env.vars_file))) vars_.set(k, v);
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
  if (job_.permissions != nullptr) {
    log_->warning("'permissions' are not emulated; explicitly supplied tokens retain their original permissions");
  }
  if (job_.concurrency != nullptr) {
    log_->info(describe_concurrency(job_.concurrency, ctx, general, wf_.file), Color::Gray);
  }
  if (job_.environment != nullptr) {
    const auto* name_token = job_.environment->is_mapping() ? job_.environment->get("name") : job_.environment;
    const auto name = evaluate_string(name_token, ctx, general, wf_.file);
    if (!name.empty()) load_environment(name);
  }
  ctx = job_context({});
  if (job_.timeout_minutes != nullptr) {
    timeout_minutes_ = evaluate_token(job_.timeout_minutes, ctx, general, wf_.file).to_number();
  }
  deadline_ = started_ + minutes_to_ms(timeout_minutes_);
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

std::vector<fs::path> JobRunner::writable_directories() const {
  std::vector<fs::path> paths = {workspace_.parent_path(), temp_, home_, tmp_, file_commands_dir_, tool_cache_};
  paths.insert(paths.end(), run_.options.sandbox.allow_write.begin(), run_.options.sandbox.allow_write.end());
  return paths;
}

void JobRunner::prepare_hosted_python_action(const ResolvedAction& resolved, const actions::Action& action) {
  const auto& cache = run_.options.sandbox.hosted_python_tool_cache;
  auto writable = writable_directories();
  sandbox::validate_hosted_python_cache_path(cache);
  sandbox::require_disjoint_from_writable(cache, writable, "setup-python host tool cache");
  auto action_writable = writable;
  action_writable.push_back(cache);
  sandbox::require_disjoint_from_writable(resolved.repository_root, action_writable, "setup-python cached action");
  sandbox::require_disjoint_from_writable(cache, {run_.options.run_dir / "control", wr_.artifacts},
                                          "setup-python host tool cache");
  auto* log = log_.get();
  const auto node = run_.node.get(actions::node_major_version(action.runtime),
                                  [log](std::string_view message) { log->warning(message); });
  sandbox::require_disjoint_from_writable(node, action_writable, "setup-python Node.js executable");
  if (sandbox::ensure_hosted_python_cache(cache, writable)) {
    log_->info("Created setup-python host tool cache: " + cache.string(), Color::Gray);
  }
  validate_hosted_python_action(resolved);
}

void JobRunner::validate_hosted_python_action(const ResolvedAction& resolved) const {
  auto writable = writable_directories();
  sandbox::validate_hosted_python_cache(run_.options.sandbox.hosted_python_tool_cache, writable);
  writable.push_back(run_.options.sandbox.hosted_python_tool_cache);
  sandbox::require_disjoint_from_writable(resolved.repository_root, writable, "setup-python cached action");
}

void JobRunner::register_hosted_python_installations(const StepResult& result) {
  std::vector<fs::path> candidates(result.paths.begin(), result.paths.end());
  if (const auto it = result.environment.find("pythonLocation"); it != result.environment.end()) {
    candidates.emplace_back(it->second);
  }
  if (const auto it = result.outputs.find("python-path"); it != result.outputs.end()) {
    candidates.emplace_back(it->second);
    if (!it->second.empty() &&
        !util::is_subpath(fs::path(it->second).lexically_normal(), run_.options.sandbox.hosted_python_tool_cache)) {
      log_->warning(
          "setup-python reported a Python path outside the host tool cache; no additional read access "
          "was granted: " +
          it->second);
    }
  }
  std::vector<fs::path> selected;
  for (const auto& candidate : candidates) {
    const auto installation =
        sandbox::hosted_python_installation(run_.options.sandbox.hosted_python_tool_cache, candidate);
    if (installation && std::find(selected.begin(), selected.end(), *installation) == selected.end()) {
      selected.push_back(*installation);
    }
  }
  for (const auto& installation : selected) {
    if (std::find(hosted_python_reads_.begin(), hosted_python_reads_.end(), installation) ==
        hosted_python_reads_.end()) {
      hosted_python_reads_.push_back(installation);
      log_->info("Read-only access enabled for selected Python installation: " + installation.string(), Color::Gray);
    }
  }
}

void JobRunner::start_sandbox() {
  if (!run_.options.sandbox.enabled) {
    log_->warning("The sandbox is disabled: steps can access all files and the network");
    return;
  }
  sandbox::SandboxConfig config;
  if (!run_.options.sandbox.allow_unity_for.empty() || !run_.options.sandbox.allow_unity_step.empty()) {
    unity_tmp_ = util::make_temp_dir("cpa-u-", "/private/tmp");
    util::write_file(job_dir_ / "unity-tmp-path", unity_tmp_.string() + "\n");
    log_->info("Unity TMPDIR: " + unity_tmp_.string(), Color::Gray);
    config.unity_tmp = unity_tmp_;
  }
  config.docker_socket = run_.options.sandbox.docker_socket;
  config.write = writable_directories();
  const auto control_root = util::real_path(run_.options.run_dir / "control");
  for (const auto& d : config.write) {
    const auto writable = util::real_path(d);
    if (util::is_subpath(control_root, writable) || util::is_subpath(writable, control_root)) {
      throw EvaluationError("Writable sandbox path overlaps the protected job control directory: " +
                            writable.string());
    }
    if (util::is_subpath(wr_.artifacts, writable) || util::is_subpath(writable, wr_.artifacts)) {
      throw EvaluationError("Writable sandbox path overlaps the artifact store: " + writable.string());
    }
  }
  const auto actions_dir = run_.options.cache_dir / "actions";
  const auto node_dir = run_.options.cache_dir / "node";
  fs::create_directories(actions_dir);
  fs::create_directories(node_dir);
  config.read = {actions_dir, node_dir, job_dir_ / "scripts", job_dir_ / "package.json",
                 short_work_dir_ / "package.json"};
  for (const auto& d : run_.options.sandbox.allow_read) config.read.push_back(d);
  config.allow_hosts = run_.options.sandbox.allow_hosts;
  config.allow_unity_hosts = run_.options.sandbox.allow_unity_hosts;
  if (run_.options.sandbox.proxy_max_connections > 0)
    config.proxy_max_connections = run_.options.sandbox.proxy_max_connections;
  if (config.allow_unity_hosts) {
    log_->warning("Unity host preset enabled for all steps in this job (proxy only): " +
                  util::join(sandbox::unity_allowed_hosts(), ", "));
  }
  config.allow_localhost = run_.options.sandbox.allow_localhost;
  config.allow_xcode = run_.options.sandbox.allow_xcode;
  if (config.allow_xcode) {
    log_->warning("Xcode temporary-file and compiler-cache access enabled for all steps in this job; host Xcode "
                  "temporary entries, clang module cache and SDK stat caches may be read and modified");
  }
  allow_xcode_simulator_ = run_.options.sandbox.allow_xcode_simulator;
  const auto& simulator_jobs = run_.options.sandbox.allow_xcode_simulator_job;
  if (!allow_xcode_simulator_ && !simulator_jobs.empty()) {
    const auto root = run_.git.root.empty() ? fs::current_path() : run_.git.root;
    const auto selector = wf_.file + "::" + job_.id;
    allow_xcode_simulator_ = util::real_path(wr_.repo_root) == util::real_path(root) &&
                             std::find(simulator_jobs.begin(), simulator_jobs.end(), selector) != simulator_jobs.end();
  }
  config.allow_xcode_simulator = allow_xcode_simulator_;
  if (config.allow_xcode_simulator) {
    log_->warning("Xcode/Simulator compatibility enabled for all steps in this job; processes can read the OS user's "
                  "DeveloperDiskImages and contact CoreSimulator services, including host Simulator operations");
  }
  config.profile_file = control_dir_ / "sandbox.sb";
  config.unity_home = home_;
  config.extra_rules = "(deny file-read* file-write* (subpath " +
                       sandbox::sbpl_quote(control_root.string()) + "))";
  auto* log = log_.get();
  sandbox_ = std::make_unique<sandbox::Sandbox>(config, [log](const std::string& host, int port) {
    log->warning("Blocked a connection to " + host + ":" + std::to_string(port) +
                 " (not allowed). Use --allow-host " + host + " to allow it.");
  }, [log](std::string_view message) { log->warning(message); });
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

void JobRunner::inherit_unity_editor_prefs() {
  if (!run_.options.inherit_unity_editor_prefs) return;
#if defined(__APPLE__)
  const fs::path relative = "Library/Preferences/com.unity3d.UnityEditor5.x.plist";
  const auto source = util::home_dir() / relative;
  const auto destination = home_ / relative;
  if (!fs::is_regular_file(fs::symlink_status(source))) {
    throw EvaluationError("Cannot inherit Unity EditorPrefs: expected a regular file at " + source.string());
  }
  fs::create_directories(destination.parent_path());
  fs::copy_file(source, destination);
  fs::permissions(destination, fs::perms::owner_read | fs::perms::owner_write);
  log_->warning("Inherited host Unity EditorPrefs into the job HOME; preferences may contain sensitive values "
                "and host paths. Changes will not be copied back to the host");
#else
  throw EvaluationError("--inherit-unity-editor-prefs is only supported on macOS");
#endif
}

bool JobRunner::setup() {
  create_directories();
  build_base_environment();
  report_.log_file = (run_.options.run_dir / "logs" / wr_.run_id / (sanitize_key(inst_.key) + ".log")).string();
  log_ = std::make_unique<JobLog>(run_.console, run_.masker, inst_.name, report_.log_file, run_.options.debug);
  log_->info("Set up job", Color::Bold);
  log_->info("Job work directory: " + work_root_.string(), Color::Gray);

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
  inherit_unity_editor_prefs();
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
  job.set("status", current_status(&scope));
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
  report.name = run_.masker.mask(report.name);
  report.error = run_.masker.mask(report.error);
  for (auto& [key, value] : report.outputs) value = run_.masker.mask(value);
  report_.steps.push_back(std::move(report));
}

void JobRunner::run_step(StepScope& scope, const workflow::Step& step) {
  StepReport sr;
  sr.id = scope.setup ? "__cpp_actions_setup_" + step.id : step.id;
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
    run_it = !setup_failed_ && evaluate_condition(step.condition, ctx, scope_for(scope.if_site), scope.file);
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
  const auto files = prepare_file_commands();
  ctx.contexts.set("github", with_fields(ctx.contexts.get("github"),
                                        {{"action", step.id}, {"env", files.env.string()},
                                         {"path", files.path.string()}}));
  const auto body = evaluate_string(step.run, ctx, general, scope.file);
  std::string shell_name;
  std::string working_directory;
  if (step.shell != nullptr) shell_name = evaluate_string(step.shell, ctx, general, scope.file);
  if (step.working_directory != nullptr) {
    working_directory = evaluate_string(step.working_directory, ctx, general, scope.file);
  }
  if (!scope.composite && !scope.setup) {
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
  if (scope.setup && shell_name.empty()) shell_name = "bash";
  auto process_env = step_environment(env, files, scope, step.id);
  auto shell = resolve_shell(shell_name, process_env["PATH"]);
  if (run_.options.ignore_interactive_shell && shell.ignore_interactive()) {
    log_->warning("Interactive shell options ignored (--ignore-interactive-shell); interactive startup behavior "
                  "is disabled");
  }
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
  const auto script = job_dir_ / "scripts" / (util::random_uuid() + shell.extension);
  util::write_file(script, shell.script(body));
  const auto selector = wf_.file + "::" + job_.id + "::" + step.id;
  const auto& docker_steps = run_.options.sandbox.allow_docker_step;
  const bool allow_docker = scope.allow_docker ||
      (!scope.composite && !scope.setup && step.explicit_id &&
       std::find(docker_steps.begin(), docker_steps.end(), selector) != docker_steps.end());
  if (sandbox_ && allow_docker) {
    log_->warning("Docker daemon access enabled for '" + (scope.composite ? scope.file : selector) +
                  "'; Docker can access host files and networks and bypass the sandbox");
  }
  const auto& allowed_steps = run_.options.sandbox.allow_msbuild_step;
  const bool allow_msbuild = scope.allow_msbuild ||
      (!scope.composite && !scope.setup && step.explicit_id &&
       std::find(allowed_steps.begin(), allowed_steps.end(), selector) != allowed_steps.end());
  if (sandbox_ && allow_msbuild) {
    log_->warning("MSBuild worker IPC enabled for '" + (scope.composite ? scope.file : selector) +
                  "'; host MSBuild sockets may be accessible to this process and its children");
  }
  const auto& unity_steps = run_.options.sandbox.allow_unity_step;
  const bool allow_unity = scope.allow_unity ||
      (!scope.composite && !scope.setup && step.explicit_id &&
       std::find(unity_steps.begin(), unity_steps.end(), selector) != unity_steps.end());
  if (sandbox_ && allow_unity) {
    log_->warning("Unity compatibility enabled for '" + (scope.composite ? scope.file : selector) +
                  "'; host Unity temporary files and licensing IPC may be accessible to this process and its children");
  }
  return execute(shell.argv(script), std::move(process_env), cwd, timeout, after_cancel, files, nullptr, {},
                 scope.allow_apple_security, allow_msbuild, allow_unity, allow_docker, false, scope.allow_xcode);
}

ResolvedAction JobRunner::resolve_action(const actions::ActionRef& ref) {
  ResolvedAction resolved;
  resolved.display = ref.display();
  if (ref.kind == actions::ActionRef::Kind::Local) {
    resolved.repository_root = util::real_path(workspace_);
    resolved.dir = util::real_path(workspace_ / ref.local_path);
    if (!util::is_subpath(resolved.repository_root, workspace_.parent_path()) ||
        !util::is_subpath(resolved.dir, resolved.repository_root)) {
      throw actions::ActionError("The local action path '" + ref.local_path + "' is outside of the workspace");
    }
    return resolved;
  }
  auto* log = log_.get();
  const auto entry = run_.repositories.fetch(ref.owner, ref.repo, ref.ref, [log](std::string_view m) {
    log->info(std::string(m), Color::Gray);
  });
  const auto source = util::real_path(ref.path.empty() ? entry.dir : entry.dir / ref.path);
  if (!util::is_subpath(source, util::real_path(entry.dir))) {
    throw actions::ActionError("The action path is outside of the repository: " + ref.path);
  }
  resolved.repository = ref.full_name();
  resolved.ref = ref.ref;
  resolved.hosted_python = sandbox_ && actions::is_setup_python_root(ref);
  if (resolved.hosted_python) {
    resolved.repository_root = util::real_path(entry.dir);
    resolved.dir = source;
    return resolved;
  }
  auto copy = action_copies_.find(entry.dir);
  if (copy == action_copies_.end()) {
    const auto dir = util::make_temp_dir("action-", temp_);
    fs::copy(entry.dir, dir, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    copy = action_copies_.emplace(entry.dir, dir).first;
  }
  resolved.repository_root = util::real_path(copy->second);
  resolved.dir = util::real_path(ref.path.empty() ? resolved.repository_root : resolved.repository_root / ref.path);
  if (!util::is_subpath(resolved.dir, resolved.repository_root)) {
    throw actions::ActionError("The action path is outside of the repository: " + ref.path);
  }
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
  bc.writable = {workspace_.parent_path(), temp_, home_, tmp_, tool_cache_, file_commands_dir_};
  for (const auto& d : run_.options.sandbox.allow_write) {
    const auto writable = util::real_path(d);
    const auto control_root = util::real_path(run_.options.run_dir / "control");
    if (util::is_subpath(control_root, writable) || util::is_subpath(writable, control_root)) {
      throw EvaluationError("Writable builtin path overlaps the protected job control directory: " +
                            writable.string());
    }
    bc.writable.push_back(writable);
  }
  if (run_.options.sandbox.enabled) {
    bc.readable = bc.writable;
    for (const auto& d : run_.options.sandbox.allow_read) bc.readable.push_back(util::real_path(d));
    bc.scoped_readable = hosted_python_reads_;
  }
  bc.cache_store = run_.options.cache_dir / "cache" / run_.repository;
  bc.lfs_store = run_.options.cache_dir / "lfs" / "github.com";
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

  auto resolved = resolve_action(ref);
  const auto permitted = [&](const std::vector<std::string>& selectors) {
    return std::any_of(selectors.begin(), selectors.end(), [&](const auto& selector) {
      return actions::matches_action_permission(selector, ref);
    });
  };
  resolved.allow_docker = permitted(run_.options.sandbox.allow_docker_for);
  if (sandbox_ && resolved.allow_docker) {
    log_->warning("Docker daemon access enabled for action '" + resolved.display +
                  "'; Docker can access host files and networks and bypass the sandbox");
  }
  resolved.allow_unity = permitted(run_.options.sandbox.allow_unity_for);
  if (sandbox_ && resolved.allow_unity) {
    log_->warning("Unity compatibility enabled for action '" + resolved.display +
                  "'; host Unity temporary files and licensing IPC may be accessible to its processes and children");
  }
  resolved.allow_msbuild = permitted(run_.options.sandbox.allow_msbuild_for);
  if (sandbox_ && resolved.allow_msbuild) {
    log_->warning("MSBuild worker IPC enabled for action '" + resolved.display +
                  "'; host MSBuild sockets may be accessible to its processes and children");
  }
  resolved.allow_xcode = permitted(run_.options.sandbox.allow_xcode_for);
  if (sandbox_ && resolved.allow_xcode) {
    log_->warning("Xcode temporary-file access enabled for action '" + resolved.display +
                  "'; matching host " + (allow_xcode_simulator_ ? "Xcode/Simulator" : "Xcode") +
                  " temporary entries may be accessible to its processes and children, including unrelated "
                  "distribution-pipeline and UUID-named entries");
  }
  resolved.allow_apple_security = permitted(run_.options.sandbox.allow_apple_security_for);
  if (sandbox_ && resolved.allow_apple_security) {
    log_->warning("Apple security services enabled for '" + resolved.display +
                  "'; its processes can contact SecurityServer and trustd.agent, including Keychain-related services");
  }
  if (resolved.hosted_python) {
    for (const auto* metadata_name : {"action.yml", "action.yaml"}) {
      const auto metadata = resolved.dir / metadata_name;
      if (!fs::is_regular_file(metadata)) continue;
      if (!util::is_subpath(util::real_path(metadata), resolved.repository_root)) {
        throw actions::ActionError("setup-python action metadata is outside its cached repository");
      }
      break;
    }
  }
  auto action = std::make_unique<actions::Action>(actions::load_action(resolved.dir, resolved.display));
  if (resolved.hosted_python) {
    if (action->kind != actions::ActionKind::Node) {
      throw actions::ActionError("Hosted Python tool cache access requires a JavaScript setup-python action");
    }
    prepare_hosted_python_action(resolved, *action);
    log_->warning("Host Python tool cache read/write enabled for '" + resolved.display +
                  "': " + run_.options.sandbox.hosted_python_tool_cache.string() +
                  "; the action and its child processes can modify this shared host cache");
  }
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
      if (result.outcome == "success" && resolved.hosted_python) register_hosted_python_installations(result);
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
  if (!util::is_subpath(script, resolved.repository_root)) throw actions::ActionError("Invalid action entry point: " + file);
  const auto node = run_.node.get(actions::node_major_version(action.runtime),
                                  [log](std::string_view m) { log->warning(m); });
  if (resolved.hosted_python) {
    validate_hosted_python_action(resolved);
    auto writable = writable_directories();
    writable.push_back(run_.options.sandbox.hosted_python_tool_cache);
    sandbox::require_disjoint_from_writable(node, writable, "setup-python Node.js executable");
  }
  const auto files = prepare_file_commands();
  StepScope action_scope = scope;
  action_scope.action_path = resolved.dir;
  action_scope.action_repository = resolved.repository;
  action_scope.action_ref = resolved.ref;
  action_scope.allow_apple_security = resolved.allow_apple_security;
  action_scope.allow_docker = resolved.allow_docker;
  action_scope.allow_msbuild = resolved.allow_msbuild;
  action_scope.allow_unity = resolved.allow_unity;
  action_scope.allow_xcode = resolved.allow_xcode;
  auto process_env = step_environment(env, files, action_scope, step_id);
  process_env["GITHUB_ACTION_PATH"] = resolved.dir.string();
  process_env["GITHUB_ACTION_REPOSITORY"] = resolved.repository;
  process_env["GITHUB_ACTION_REF"] = resolved.ref;
  for (const auto& [k, v] : inputs) {
    process_env["INPUT_" + util::to_upper(util::replace_all(k, " ", "_"))] = v;
  }
  for (const auto& [k, v] : state) process_env["STATE_" + k] = v;
  // Cached installations are already readable; a host or explicit binary must not
  // implicitly grant access to its parent directories (e.g. the host HOME).
  auto result = execute({node.string(), script.string()}, std::move(process_env), workspace_, timeout, after_cancel,
                        files, &state, {node}, resolved.allow_apple_security, resolved.allow_msbuild,
                        resolved.allow_unity, resolved.allow_docker, resolved.hosted_python, resolved.allow_xcode);
  if (resolved.hosted_python && result.outcome == "failure") {
    const auto version = inputs.find("python-version");
    const auto architecture = inputs.find("architecture");
    const auto freethreaded = inputs.find("freethreaded");
    const bool standard = freethreaded == inputs.end() || !util::iequals(freethreaded->second, "true");
    log_->warning(
        "setup-python failed; a macOS cache miss may require privileged installation, which the "
        "sandbox does not permit. " +
        sandbox::hosted_python_provisioning_hint(
            standard && version != inputs.end() ? std::string_view(version->second) : std::string_view{},
            architecture != inputs.end() ? std::string_view(architecture->second) : std::string_view{}));
  }
  return result;
}

StepResult JobRunner::run_composite(StepScope& parent, const std::string& name, const actions::Action& action,
                                    const ResolvedAction& resolved, const StringMap& inputs,
                                    const StringMap& step_env) {
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
  child.allow_apple_security = resolved.allow_apple_security;
  child.allow_docker = resolved.allow_docker;
  child.allow_msbuild = resolved.allow_msbuild;
  child.allow_unity = resolved.allow_unity;
  child.allow_xcode = resolved.allow_xcode;
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
                          env, std::chrono::minutes(5), true);
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
      {"CPP_ACTIONS", "true"},
      {"CPP_ACTIONS_VERSION", CPA_VERSION},
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
  workspace::set_git_config_environment(out, home_ / ".gitconfig");
  if (sandbox_) {
    out["HOME"] = home_.string();
    out["TMPDIR"] = tmp_.string();
#if defined(__APPLE__)
    out["CFFIXED_USER_HOME"] = home_.string();
    // xcrun ignores TMPDIR for its lookup cache; the host cache would be shared with unsandboxed xcrun.
    out["xcrun_db"] = (tmp_ / "xcrun_db").string();
    out["PATH"] = (job_dir_ / "scripts/bin").string() + ":" + out["PATH"];
#endif
    for (const auto& [k, v] : sandbox_->environment()) out[k] = v;
  }
  auto& java_options = out["JDK_JAVA_OPTIONS"];
  if (!java_options.empty()) java_options += " ";
  // The Java launcher's option tokenizer supports quotes, not shell backslash escapes.
  const auto java_home = util::replace_all(home_.string(), "\"", "\"'\"'\"");
  java_options += "\"-Duser.home=" + java_home + "\" \"-Dmaven.repo.local=" + java_home + "/.m2/repository\"";
  return out;
}

StepResult JobRunner::execute(std::vector<std::string> argv, util::Environment env, const fs::path& cwd,
                              std::optional<std::chrono::milliseconds> timeout, bool after_cancel,
                              const FileCommands& files, StringMap* state, const std::vector<fs::path>& extra_read,
                              bool allow_apple_security, bool allow_msbuild, bool allow_unity, bool allow_docker,
                              bool hosted_python, bool allow_xcode) {
  StepResult result;
  if (sandbox_) env.try_emplace("DOTNET_USE_POLLING_FILE_WATCHER", "1");
  if (sandbox_ && allow_unity) {
    env["DOTNET_USE_POLLING_FILE_WATCHER"] = "1";
    auto& java_options = env["JDK_JAVA_OPTIONS"];
    if (!java_options.empty()) java_options += " ";
    java_options += "-Djava.net.preferIPv4Stack=true " + sandbox_->java_proxy_options();
    env["TMPDIR"] = unity_tmp_.string() + "/";
  }
  const auto java_tmp = sandbox_ && allow_unity ? unity_tmp_.string() + "/" : tmp_.string();
  env["JDK_JAVA_OPTIONS"] += " \"-Djava.io.tmpdir=" + util::replace_all(java_tmp, "\"", "\"'\"'\"") + "\"";
  if (sandbox_) {
    const std::vector<fs::path> scoped_write =
        hosted_python ? std::vector<fs::path>{run_.options.sandbox.hosted_python_tool_cache} : std::vector<fs::path>{};
    argv = sandbox_->wrap(argv, extra_read, allow_apple_security, allow_msbuild, allow_unity, hosted_python_reads_,
                          scoped_write, allow_docker, allow_xcode);
  }
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
  spec.force_cancel = &inst_.force;
  spec.on_line = [&](std::string_view line, bool) { handle_line(line, cs); };
  struct ProcessGroupGuard {
    pid_t pgid = 0;
    ~ProcessGroupGuard() { util::kill_process_group(pgid); }
  } process_group;
  spec.on_spawn = [&](pid_t pgid) { process_group.pgid = pgid; };
  const auto pr = util::run_process(spec);
  util::kill_process_group(process_group.pgid);
  process_group.pgid = 0;
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
  for (const auto& [k, v] : cs.environment) result.environment.emplace(k, v);
  result.paths.insert(result.paths.end(), cs.paths.begin(), cs.paths.end());
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
    for (const auto& [k, v] : parse_key_value_file(read_command_file(files.env))) {
      if (util::iequals(k, "NODE_OPTIONS")) {
        log_->warning("Can't store NODE_OPTIONS output parameter using '$GITHUB_ENV' command.");
        continue;
      }
      global_env_[k] = v;
      result.environment[k] = v;
    }
    for (const auto& [k, v] : parse_key_value_file(read_command_file(files.output))) result.outputs[k] = v;
    for (const auto& p : parse_path_file(read_command_file(files.path))) {
      std::erase(path_prepend_, p);
      path_prepend_.push_back(p);
      result.paths.push_back(p);
    }
    for (const auto& [k, v] : parse_key_value_file(read_command_file(files.state))) {
      if (state != nullptr) (*state)[k] = v;
    }
    const auto summary = read_command_file(files.summary);
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
  a.title = run_.masker.mask(prop("title"));
  a.file = run_.masker.mask(prop("file"));
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
      cs.paths.push_back(cmd->message);
    } else if (auto it = cmd->properties.find("name"); it != cmd->properties.end()) {
      global_env_[it->second] = cmd->message;
      cs.environment[it->second] = cmd->message;
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
  sandbox_.reset();
  if (!short_work_dir_.empty()) {
    if (run_.options.keep_workdir) {
      if (log_) log_->info("Retained job work directory: " + work_root_.string(), Color::Gray);
    } else {
      try {
        util::remove_tree(short_work_dir_);
      } catch (const std::exception& e) {
        if (log_) log_->warning(std::string("Failed to remove the job work directory: ") + e.what());
      }
    }
  }
  if (!unity_tmp_.empty()) {
    if (run_.options.keep_workdir) {
      if (log_) log_->info("Retained Unity TMPDIR: " + unity_tmp_.string(), Color::Gray);
    } else {
      try {
        util::remove_tree(unity_tmp_);
      } catch (const std::exception& e) {
        if (log_) log_->warning(std::string("Failed to remove Unity temporary directory: ") + e.what());
      }
    }
  }
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
  if (!run_.options.setup_steps.empty()) {
    StepScope setup_scope;
    setup_scope.setup = true;
    setup_scope.inputs = top_.inputs;
    setup_scope.prefix = "cpp-actions setup / ";
    for (const auto& group : run_.options.setup_steps) {
      setup_scope.file = group.document ? group.document->file : "<setup-steps>";
      for (const auto& step : group.steps) run_step(setup_scope, step);
    }
    setup_failed_ = setup_scope.status == "failure";
    if (setup_failed_) {
      status_ = "failure";
      log_->error("cpp-actions setup failed; workflow steps will be skipped");
    }
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
  return out.substr(0, 32) + "-" + util::sha256_hex(key);
}

std::string hash_files(const fs::path& root, const std::vector<std::string>& patterns) {
  const auto canonical_root = util::real_path(root);
  if (canonical_root != fs::absolute(root).lexically_normal()) return {};
  const auto files = util::glob_files(root, patterns);
  if (files.empty()) return {};
  util::Sha256 h;
  bool matched = false;
  for (const auto& f : files) {
    if (!util::is_subpath(util::real_path(f), canonical_root)) continue;
    const auto d = util::sha256_file(f);
    h.update(d.data(), d.size());
    matched = true;
  }
  return matched ? util::Sha256::hex(h.digest()) : std::string{};
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
