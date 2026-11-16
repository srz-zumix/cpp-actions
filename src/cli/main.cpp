#include <CLI/CLI.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <system_error>
#include <thread>

#include "config/config.hpp"
#include "engine/catalog.hpp"
#include "engine/cleanup.hpp"
#include "engine/event.hpp"
#include "engine/runner.hpp"
#include "sandbox/sandbox.hpp"
#include "util/fs.hpp"
#include "util/strings.hpp"
#include "workspace/git.hpp"

namespace {

using namespace cpa;
namespace fs = std::filesystem;

struct CommonOptions {
  std::string directory;
  std::vector<std::string> workflow_paths;
};

void add_common(CLI::App* cmd, CommonOptions& o) {
  cmd->add_option("-C,--directory", o.directory, "Repository directory (default: the current directory)");
  cmd->add_option("-W,--workflows", o.workflow_paths,
                  "Workflow files or directories (default: <repository>/.github/workflows)");
}

fs::path repository_root(const CommonOptions& o) {
  const fs::path dir = util::real_path(o.directory.empty() ? fs::current_path() : fs::path(o.directory));
  if (auto root = workspace::find_repo_root(dir)) return *root;
  return dir;
}

std::vector<engine::WorkflowFile> load_files(const CommonOptions& o, const fs::path& root) {
  std::vector<fs::path> paths;
  for (const auto& p : o.workflow_paths) paths.emplace_back(p);
  auto files = engine::load_workflow_files(root, paths);
  if (files.empty()) {
    throw std::runtime_error("no workflow files found in " + (root / ".github" / "workflows").string());
  }
  return files;
}

std::map<std::string, std::string> parse_assignments(const std::vector<std::string>& items, std::string_view what) {
  std::map<std::string, std::string> out;
  for (const auto& item : items) {
    auto [name, value] = config::split_assignment(item);
    if (name.empty() || !value) throw std::runtime_error(std::string(what) + " must be NAME=VALUE");
    out[name] = *value;
  }
  return out;
}

int cmd_validate(const CommonOptions& o) {
  const auto root = repository_root(o);
  const auto files = load_files(o, root);
  std::cout << engine::render_validation(files);
  return std::all_of(files.begin(), files.end(), [](const engine::WorkflowFile& f) { return f.valid(); }) ? 0 : 1;
}

int cmd_list(const CommonOptions& o) {
  const auto root = repository_root(o);
  std::cout << engine::render_list(load_files(o, root));
  return 0;
}

int cmd_graph(const CommonOptions& o, const std::vector<std::string>& selectors, const std::string& format) {
  const auto root = repository_root(o);
  const auto files = load_files(o, root);
  for (const auto& sel : selectors) {
    if (std::none_of(files.begin(), files.end(),
                     [&](const engine::WorkflowFile& f) { return engine::workflow_matches(f, sel); })) {
      throw std::runtime_error("No workflow matches '" + sel + "'");
    }
  }
  std::vector<const workflow::Workflow*> workflows;
  int status = 0;
  for (const auto& f : files) {
    if (!selectors.empty() && std::none_of(selectors.begin(), selectors.end(), [&](const std::string& s) {
          return engine::workflow_matches(f, s);
        })) {
      continue;
    }
    if (!f.valid()) {
      std::cerr << f.display << ": invalid workflow (run 'cpp-actions validate')\n";
      status = 1;
      continue;
    }
    workflows.push_back(f.workflow.get());
  }
  if (format == "dot") {
    std::cout << engine::render_graph_dot(workflows);
  } else {
    for (std::size_t i = 0; i < workflows.size(); ++i) {
      if (i > 0) std::cout << "\n";
      std::cout << engine::render_graph_text(*workflows[i]);
    }
  }
  return status;
}

struct RunCli {
  CommonOptions common;
  engine::EventOptions event;
  std::vector<std::string> inputs;
  std::vector<std::string> matrix;
  std::vector<std::string> selectors;
  engine::RunOptions options;
  std::string config_path;
  std::vector<std::string> allow_read;
  std::vector<std::string> secrets;
  std::vector<std::string> secret_files;
  std::vector<std::string> vars;
  std::vector<std::string> var_files;
  int parallel = -1;
  int proxy_max_connections = 0;
  bool no_sandbox = false;
  bool dry_run = false;
  bool no_needs = false;
  bool no_color = false;
};

std::atomic<unsigned> signal_requests{0};
static_assert(std::atomic<unsigned>::is_always_lock_free);

void request_cancel(int) { signal_requests.fetch_add(1, std::memory_order_relaxed); }

// Runner::cancel takes locks; only the watcher, never the signal handler, calls it.
class SignalHandlers {
 public:
  SignalHandlers() {
    signal_requests.store(0, std::memory_order_relaxed);
    struct sigaction action {};
    action.sa_handler = request_cancel;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    if (sigaction(SIGINT, &action, &old_int_) != 0) {
      throw std::system_error(errno, std::generic_category(), "install SIGINT handler");
    }
    if (sigaction(SIGTERM, &action, &old_term_) != 0) {
      const auto error = errno;
      sigaction(SIGINT, &old_int_, nullptr);
      throw std::system_error(error, std::generic_category(), "install SIGTERM handler");
    }
  }
  ~SignalHandlers() {
    sigaction(SIGTERM, &old_term_, nullptr);
    sigaction(SIGINT, &old_int_, nullptr);
  }
  SignalHandlers(const SignalHandlers&) = delete;
  SignalHandlers& operator=(const SignalHandlers&) = delete;

 private:
  struct sigaction old_int_ {};
  struct sigaction old_term_ {};
};

config::Config load_configuration(const fs::path& root, const std::string& explicit_path) {
  const auto paths = config::configuration_paths(root, explicit_path);
  auto cfg = config::load_configs(paths);
  if (explicit_path.empty() &&
      std::find(paths.begin(), paths.end(), root / ".cpp-actions.yml") != paths.end()) {
    std::cerr << "WARNING: automatically loading repository configuration: "
              << (root / ".cpp-actions.yml").string()
              << "; trust this file before running (it can execute setup steps and relax sandbox permissions)\n";
  }
  return cfg;
}

engine::RunOptions run_options(const RunCli& o, const fs::path& root) {
  auto options = o.options;
  const auto cfg = load_configuration(root, o.config_path);
  options.sandbox.enabled = !o.no_sandbox;
  options.setup_steps = cfg.setup_steps;
  options.sandbox.allow_hosts.insert(options.sandbox.allow_hosts.begin(), cfg.allow_hosts.begin(),
                                     cfg.allow_hosts.end());
  options.sandbox.allow_apple_security_for.insert(options.sandbox.allow_apple_security_for.begin(),
                                                 cfg.allow_apple_security_for.begin(),
                                                 cfg.allow_apple_security_for.end());
  options.sandbox.allow_docker_for.insert(options.sandbox.allow_docker_for.begin(),
                                          cfg.allow_docker_for.begin(), cfg.allow_docker_for.end());
  options.sandbox.allow_docker_step.insert(options.sandbox.allow_docker_step.begin(),
                                           cfg.allow_docker_step.begin(), cfg.allow_docker_step.end());
  for (const auto& path : cfg.allow_read) options.sandbox.allow_read.emplace_back(path);
  options.sandbox.allow_msbuild_for.insert(options.sandbox.allow_msbuild_for.begin(),
                                           cfg.allow_msbuild_for.begin(), cfg.allow_msbuild_for.end());
  options.sandbox.allow_msbuild_step.insert(options.sandbox.allow_msbuild_step.begin(),
                                            cfg.allow_msbuild_step.begin(), cfg.allow_msbuild_step.end());
  options.sandbox.allow_unity_for.insert(options.sandbox.allow_unity_for.begin(),
                                         cfg.allow_unity_for.begin(), cfg.allow_unity_for.end());
  options.sandbox.allow_unity_step.insert(options.sandbox.allow_unity_step.begin(),
                                          cfg.allow_unity_step.begin(), cfg.allow_unity_step.end());
  options.sandbox.allow_xcode_for.insert(options.sandbox.allow_xcode_for.begin(),
                                         cfg.allow_xcode_for.begin(), cfg.allow_xcode_for.end());
  for (const auto& path : o.allow_read) options.sandbox.allow_read.push_back(config::expand_user(path));
  for (auto& path : options.sandbox.allow_write) path = config::expand_user(path.string());
  for (const auto& path : cfg.allow_write) options.sandbox.allow_write.emplace_back(path);
  options.sandbox.allow_localhost = options.sandbox.allow_localhost || cfg.allow_localhost.value_or(false);
  options.sandbox.allow_xcode = options.sandbox.allow_xcode || cfg.allow_xcode.value_or(false);
  options.sandbox.allow_xcode_simulator =
      options.sandbox.allow_xcode_simulator || cfg.allow_xcode_simulator.value_or(false);
  options.sandbox.allow_unity_hosts = options.sandbox.allow_unity_hosts || cfg.allow_unity_hosts.value_or(false);
  options.ignore_interactive_shell = options.ignore_interactive_shell || cfg.ignore_interactive_shell.value_or(false);
  options.inherit_unity_editor_prefs =
      options.inherit_unity_editor_prefs || cfg.inherit_unity_editor_prefs.value_or(false);
  options.pass_env.insert(options.pass_env.begin(), cfg.pass_env.begin(), cfg.pass_env.end());
  options.host_labels.insert(options.host_labels.begin(), cfg.host_labels.begin(), cfg.host_labels.end());
  options.parallel = o.parallel >= 0 ? o.parallel : cfg.parallel.value_or(0);
  options.sandbox.proxy_max_connections =
      o.proxy_max_connections > 0 ? o.proxy_max_connections : cfg.proxy_max_connections.value_or(0);
  if (options.cache_dir.empty() && cfg.cache_dir) options.cache_dir = *cfg.cache_dir;
  if (!options.cache_dir.empty()) options.cache_dir = config::expand_user(options.cache_dir.string());
  if (!options.run_dir.empty()) options.run_dir = config::expand_user(options.run_dir.string());
  if (!options.artifact_dir.empty()) options.artifact_dir = config::expand_user(options.artifact_dir.string());
  if (options.node.empty() && cfg.node) options.node = *cfg.node;
  if (!options.node.empty()) options.node = config::expand_user(options.node.string());
  options.with_needs = !o.no_needs;
  options.matrix = parse_assignments(o.matrix, "--matrix");
  options.environments = cfg.environments;
  auto load_values = [](auto& values, const fs::path& path) {
    for (const auto& [key, value] : config::load_dotenv(path)) values[key] = value;
  };
  if (cfg.secrets_file && !cfg.secrets_file->empty()) load_values(options.secrets, *cfg.secrets_file);
  for (const auto& path : o.secret_files) load_values(options.secrets, config::expand_user(path));
  for (const auto& item : o.secrets) {
    const auto [name, value] = config::split_assignment(item);
    if (name.empty()) throw std::runtime_error("--secret must be NAME=VALUE or NAME");
    if (value) {
      options.secrets[name] = *value;
    } else if (const auto* host_value = std::getenv(name.c_str())) {
      options.secrets[name] = host_value;
    } else {
      throw std::runtime_error("environment variable for --secret '" + name + "' is not set");
    }
  }
  if (const auto it = options.secrets.find("GITHUB_TOKEN");
      options.github_token.empty() && it != options.secrets.end()) {
    options.github_token = it->second;
  }
  if (cfg.vars_file && !cfg.vars_file->empty()) load_values(options.vars, *cfg.vars_file);
  for (const auto& [key, value] : cfg.vars) options.vars[key] = value;
  for (const auto& path : o.var_files) load_values(options.vars, config::expand_user(path));
  for (const auto& [key, value] : parse_assignments(o.vars, "--var")) options.vars[key] = value;
  for (const auto& item : options.pass_env) {
    const auto name = config::split_assignment(item).first;
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
      throw std::runtime_error("--pass-env requires a valid environment variable name");
    }
  }
  return options;
}

int cmd_run(RunCli& o) {
  const auto root = repository_root(o.common);
  const auto files = load_files(o.common, root);
  const auto git = workspace::read_git_info(root);
  const auto event = engine::build_event(o.event, git);
  const auto selection = engine::select_workflows(files, event.info, o.selectors);
  if (!o.dry_run) {
    for (const auto& selected : selection) {
      if (!selected.file->valid()) {
        throw std::runtime_error(selected.file->display + ": invalid workflow (run 'cpp-actions validate')");
      }
    }
  }
  auto inputs = parse_assignments(o.inputs, "--input");
  if (event.info.name == "workflow_dispatch") {
    const auto payload_inputs = event.info.payload.get("inputs");
    if (!payload_inputs.is_null()) {
      if (!payload_inputs.is_object()) throw engine::EventError("event.inputs must be an object");
      for (const auto& [name, value] : payload_inputs.object_data().entries()) {
        if (!value.is_string() && !value.is_bool() && !value.is_number()) {
          throw engine::EventError("event.inputs values must be strings, booleans or numbers");
        }
        inputs.try_emplace(name, value.to_string());
      }
    }
  }
  std::cout << engine::render_event(event);
  std::size_t triggered = 0;
  for (const auto& s : selection) {
    std::cout << "\n";
    if (!s.match.matched) {
      std::cout << "- " << s.file->display << ": not triggered (" << s.match.reason << ")\n";
      continue;
    }
    ++triggered;
    if (event.info.name == "workflow_dispatch") {
      engine::dispatch_inputs(*s.file->workflow, inputs);
    }
    std::cout << "+ " << s.file->display << ": " << s.match.reason << "\n";
    std::cout << engine::render_graph_text(*s.file->workflow);
  }
  if (triggered == 0) std::cout << "\nNo workflow is triggered by '" << event.info.name << "'.\n";
  if (o.dry_run || triggered == 0) return 0;
  auto options = run_options(o, root);
  options.inputs = inputs;
  if (options.sandbox.enabled) {
    std::string reason;
    if (!sandbox::supported(&reason)) {
      throw std::runtime_error(reason + "; refusing execution (use --no-sandbox only for trusted local debugging)");
    }
  } else {
    std::cerr << "WARNING: --no-sandbox disables filesystem and network isolation. "
                 "Workflow code can access host files, credentials and network services.\n";
  }
  engine::Masker masker;
  for (const auto& [name, value] : options.secrets) {
    (void)name;
    masker.add(value);
  }
  engine::Console console(std::cout, !o.no_color && std::getenv("NO_COLOR") == nullptr && ::isatty(STDOUT_FILENO));
  masker.add(options.github_token);
  engine::Runner runner(std::move(options), console);
  SignalHandlers handlers;
  std::jthread watcher([&runner](std::stop_token stop) {
    unsigned handled = 0;
    while (!stop.stop_requested()) {
      const auto requested = signal_requests.load(std::memory_order_relaxed);
      while (handled < requested) {
        runner.cancel();
        ++handled;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });
  const auto report = runner.run(selection, event, git);
  watcher.request_stop();
  watcher.join();
  console.line("\nResult: " + report.result + " (" + util::format_duration_ms(report.duration_ms) + ")");
  for (const auto& workflow : report.workflows) {
    for (const auto& job : workflow.jobs) {
      console.line(masker.mask("  " + workflow.name + " / " + job.name + ": " + job.result));
      if (!job.summary.empty()) console.line(masker.mask(job.summary));
    }
  }
  console.line("Logs and report: " + report.run_dir);
  if (signal_requests.load(std::memory_order_relaxed) != 0 || report.result == "cancelled") return 130;
  return report.result == "failure" ? 1 : 0;
}

struct CleanCli {
  std::vector<std::string> runs;
  std::string cache_dir;
  std::string config_path;
  bool all = false;
  bool dry_run = false;
  bool force = false;
};

fs::path clean_cache_dir(const CleanCli& o) {
  if (!o.cache_dir.empty()) return config::expand_user(o.cache_dir);
  const auto cwd = util::real_path(fs::current_path());
  const auto cfg = load_configuration(workspace::find_repo_root(cwd).value_or(cwd), o.config_path);
  if (cfg.cache_dir && !cfg.cache_dir->empty()) return *cfg.cache_dir;
  return engine::default_cache_dir();
}

int cmd_clean(const CleanCli& o) {
  if (o.runs.empty() == !o.all) throw std::runtime_error("specify run directories or --all (but not both)");
  const auto is_path = [](const std::string& run) {
    return run.find('/') != std::string::npos || run == "." || run == ".." || run.starts_with("~");
  };
  const bool needs_cache_dir =
      o.all || std::any_of(o.runs.begin(), o.runs.end(), [&](const auto& run) { return !is_path(run); });
  const auto runs_root = needs_cache_dir ? clean_cache_dir(o) / "runs" : fs::path{};
  std::vector<fs::path> runs;
  for (const auto& run : o.runs) {
    const bool path = is_path(run);
    if (!path && (run.empty() || run.find('\0') != std::string::npos))
      throw std::runtime_error("invalid run name '" + run + "'");
    const auto run_path = path ? fs::absolute(config::expand_user(run)) : runs_root / run;
    if (!fs::exists(fs::symlink_status(run_path)))
      throw std::runtime_error("run directory not found: " + run_path.string());
    runs.push_back(run_path.lexically_normal());
  }
  if (o.all && fs::is_directory(runs_root)) {
    for (const auto& entry : fs::directory_iterator(runs_root)) {
      if (!entry.is_symlink() && entry.is_directory() && engine::has_retained_data(entry.path()))
        runs.push_back(entry.path());
    }
    std::sort(runs.begin(), runs.end());
  }
  if (runs.empty()) {
    std::cout << "No retained work directories under " << runs_root.string() << "\n";
    return 0;
  }
  engine::CleanOptions options;
  options.dry_run = o.dry_run;
  options.force = o.force;
  const std::string verb = o.dry_run ? "would remove " : "removed ";
  int status = 0;
  for (const auto& run : runs) {
    const auto report = engine::clean_retained_run(run, options);
    const auto name = report.run_dir.string();
    for (const auto& path : report.removed) std::cout << name << ": " << verb << path.string() << "\n";
    if (report.skipped) {
      std::cout << name << ": skipped: " << report.skip_reason << "\n";
      if (!o.all) status = 1;
    }
    for (const auto& error : report.errors) std::cerr << name << ": error: " << error << "\n";
    if (!report.errors.empty()) status = 1;
  }
  return status;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"cpp-actions - run GitHub Actions workflows locally in a sandbox"};
  app.set_version_flag("--version", std::string(CPA_VERSION));
  app.require_subcommand(1);

  CommonOptions validate_opts;
  auto* validate = app.add_subcommand("validate", "Validate workflow files");
  add_common(validate, validate_opts);

  CommonOptions list_opts;
  auto* list = app.add_subcommand("list", "List workflows, triggers and jobs");
  add_common(list, list_opts);

  CommonOptions graph_opts;
  std::vector<std::string> graph_selectors;
  std::string graph_format = "text";
  auto* graph = app.add_subcommand("graph", "Show the job dependency graph");
  add_common(graph, graph_opts);
  graph->add_option("-w,--workflow", graph_selectors, "Workflow (path, file name or name)");
  graph->add_option("-f,--format", graph_format, "Output format")->check(CLI::IsMember({"text", "dot"}));

  RunCli run_opts;
  auto* run = app.add_subcommand("run", "Run the workflows triggered by an event");
  add_common(run, run_opts.common);
  run->add_option("-e,--event", run_opts.event.name, "Event name")->capture_default_str();
  run->add_option("--event-path", run_opts.event.payload_file, "Event payload (JSON)");
  run->add_option("--ref", run_opts.event.ref, "github.ref (branch, tag or full ref)");
  run->add_option("--base-ref", run_opts.event.base_ref, "Pull requests: the target branch");
  run->add_option("--head-ref", run_opts.event.head_ref, "Pull requests: the source branch");
  run->add_option("--activity-type", run_opts.event.activity_type, "Activity type (github.event.action)");
  run->add_option("--changes-base", run_opts.event.changes_base,
                  "Revision to compute the changed files from (for paths filters)");
  run->add_option("--actor", run_opts.event.actor, "github.actor");
  run->add_option("-i,--input", run_opts.inputs, "workflow_dispatch input (NAME=VALUE)");
  run->add_option("-w,--workflow", run_opts.selectors, "Workflow to run (path, file name or name)");
  run->add_flag("-n,--dry-run", run_opts.dry_run, "Show what would run without running it");
  run->add_option("--node", run_opts.options.node, "Node executable for JavaScript actions");
  run->add_option("-j,--job", run_opts.options.jobs, "Job id to run (includes dependencies by default)");
  run->add_flag("--no-needs", run_opts.no_needs, "Do not run dependencies of the selected jobs");
  run->add_option("--matrix", run_opts.matrix, "Matrix filter (NAME=VALUE)");
  run->add_option("--github-token,--token", run_opts.options.github_token, "Explicit GitHub API token");
  run->add_option("--allow-write", run_opts.options.sandbox.allow_write, "Additional sandbox writable path");
  run->add_flag("--allow-localhost", run_opts.options.sandbox.allow_localhost, "Allow loopback connections and listening on any port (listeners are not limited to loopback)");
  run->add_flag("--allow-xcode", run_opts.options.sandbox.allow_xcode,
                "Allow Xcode temporary files and compiler caches for all steps of all jobs (shared with host Xcode)");
  run->add_flag("--allow-xcode-simulator", run_opts.options.sandbox.allow_xcode_simulator,
                "Allow Xcode/Simulator services for all jobs (permits host Simulator operations)");
  run->add_option("--config", run_opts.config_path,
                  "Use only this trusted configuration file instead of user and repository defaults");
  run->add_flag("--no-sandbox", run_opts.no_sandbox, "DANGER: disable filesystem and network isolation");
  run->add_option("--allow-host", run_opts.options.sandbox.allow_hosts, "Additional allowed network host (repeatable)");
  run->add_flag("--allow-unity-hosts", run_opts.options.sandbox.allow_unity_hosts,
                "Allow the fixed public Unity/TLS host preset through the proxy for all jobs");
  run->add_option("--allow-apple-security-for", run_opts.options.sandbox.allow_apple_security_for,
                  "Allow Apple security services for an action selector (ref permits '*'; repeatable; expands Keychain access)");
  run->add_option("--allow-read", run_opts.allow_read, "Additional readable host path (repeatable)");
  run->add_option("--allow-docker-for", run_opts.options.sandbox.allow_docker_for,
                  "Allow host Docker daemon access for an action selector (ref permits '*'; repeatable; bypasses sandbox)");
  run->add_option("--allow-docker-step", run_opts.options.sandbox.allow_docker_step,
                  "Allow host Docker daemon access for workflow-path::job-id::step-id (run steps only; repeatable; bypasses sandbox)");
  run->add_option("--allow-msbuild-for", run_opts.options.sandbox.allow_msbuild_for,
                  "Allow MSBuild Unix sockets for an action selector (ref permits '*'; repeatable)");
  run->add_option("--allow-msbuild-step", run_opts.options.sandbox.allow_msbuild_step,
                  "Allow MSBuild Unix sockets for workflow-path::job-id::step-id (run steps only; repeatable)");
  run->add_option("--allow-unity-for", run_opts.options.sandbox.allow_unity_for,
                  "Allow Unity temporary files and IPC for an action selector (ref permits '*'; repeatable)");
  run->add_option("--allow-unity-step", run_opts.options.sandbox.allow_unity_step,
                  "Allow Unity temporary files and IPC for workflow-path::job-id::step-id (run steps only; repeatable)");
  run->add_option("--allow-xcode-for", run_opts.options.sandbox.allow_xcode_for,
                  "Allow Xcode temporary files for an action selector (ref permits '*'; repeatable)");
  run->add_option("--pass-env,--env-pass,--env", run_opts.options.pass_env,
                  "Pass host variable NAME or explicit NAME=VALUE (repeatable)");
  run->add_flag("--keep-workdir", run_opts.options.keep_workdir, "Keep job workspaces after execution");
  run->add_flag("--ignore-interactive-shell", run_opts.options.ignore_interactive_shell,
                "Ignore bash/sh interactive options in run steps (warns when changed)");
  run->add_flag("--inherit-unity-editor-prefs", run_opts.options.inherit_unity_editor_prefs,
                "Copy host Unity EditorPrefs into each job HOME (macOS only; may expose sensitive preferences)");
  run->add_option("--run-dir", run_opts.options.run_dir,
                  "Run output directory (must not already exist; default: a unique directory under the cache)");
  run->add_option("--artifact-dir", run_opts.options.artifact_dir,
                  "Artifact output root (files are saved under <directory>/<workflow-run-id>/<artifact-name>)");
  run->add_option("--cache-dir", run_opts.options.cache_dir, "Shared cache directory");
  run->add_option("--parallel", run_opts.parallel, "Concurrent jobs (0 = CPU count)")->check(CLI::NonNegativeNumber);
  run->add_option("--proxy-max-connections", run_opts.proxy_max_connections,
                  "Concurrent connections per job network proxy (default: " +
                      std::to_string(cpa::sandbox::kDefaultProxyConnections) + ")")
      ->check(CLI::Range(1, cpa::sandbox::kMaxProxyConnections));
  run->add_option("--host-label", run_opts.options.host_labels,
                  "Explicit runs-on label to execute on this host (repeatable)");
  run->add_option("-s,--secret", run_opts.secrets, "Secret NAME from host environment or NAME=VALUE (repeatable)");
  run->add_option("--secret-file,--secrets-file", run_opts.secret_files, "Explicit dotenv secrets file (repeatable)");
  run->add_option("--var", run_opts.vars, "Workflow variable NAME=VALUE (repeatable)");
  run->add_option("--var-file,--vars-file", run_opts.var_files, "Explicit dotenv variables file (repeatable)");
  run->add_flag("--debug", run_opts.options.debug, "Show workflow debug commands");
  run->add_flag("--no-color", run_opts.no_color, "Disable colored logs");

  CleanCli clean_opts;
  auto* clean = app.add_subcommand("clean", "Remove work directories retained by run --keep-workdir");
  clean->add_option("run", clean_opts.runs, "Run directory path, or run name under <cache>/runs");
  clean->add_flag("-a,--all", clean_opts.all, "Clean every completed run under <cache>/runs");
  clean->add_option("--cache-dir", clean_opts.cache_dir, "Shared cache directory");
  clean->add_option("--config", clean_opts.config_path,
                    "Use only this trusted configuration file instead of user and repository defaults (for cache-dir)");
  clean->add_flag("-n,--dry-run", clean_opts.dry_run, "Show what would be removed without removing it");
  clean->add_flag("--force", clean_opts.force,
                  "Also clean runs without report.json (interrupted runs; never runs that are still in progress)");

  CLI11_PARSE(app, argc, argv);

  try {
    if (validate->parsed()) return cmd_validate(validate_opts);
    if (list->parsed()) return cmd_list(list_opts);
    if (graph->parsed()) return cmd_graph(graph_opts, graph_selectors, graph_format);
    if (run->parsed()) return cmd_run(run_opts);
    if (clean->parsed()) return cmd_clean(clean_opts);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
  return 2;
}
