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
  std::vector<std::string> selectors;
  engine::RunOptions options;
  std::string config_path;
  std::vector<std::string> allow_read;
  std::vector<std::string> secrets;
  std::vector<std::string> secret_files;
  std::vector<std::string> vars;
  std::vector<std::string> var_files;
  int parallel = -1;
  bool no_sandbox = false;
  bool dry_run = false;
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

engine::RunOptions run_options(const RunCli& o, const fs::path& root) {
  auto options = o.options;
  const bool explicit_config = !o.config_path.empty();
  const auto config_path = explicit_config ? config::expand_user(o.config_path) : config::default_config_path();
  config::Config cfg;
  if (explicit_config || fs::exists(config_path)) {
    if (!explicit_config && util::is_subpath(util::real_path(config_path), root)) {
      throw std::runtime_error("automatic configuration is inside the repository; use --config to trust it explicitly");
    }
    cfg = config::load_config(config_path);
  }
  const auto config_dir = fs::absolute(config_path).parent_path();
  auto config_file = [&](std::string_view value) {
    auto path = config::expand_user(value);
    return (path.is_absolute() ? path : config_dir / path).lexically_normal();
  };
  options.sandbox.enabled = !o.no_sandbox;
  options.sandbox.allow_hosts.insert(options.sandbox.allow_hosts.begin(), cfg.allow_hosts.begin(),
                                     cfg.allow_hosts.end());
  for (const auto& path : cfg.allow_read) options.sandbox.allow_read.push_back(config_file(path));
  for (const auto& path : o.allow_read) options.sandbox.allow_read.push_back(config::expand_user(path));
  for (const auto& path : cfg.allow_write) options.sandbox.allow_write.push_back(config_file(path));
  options.sandbox.allow_localhost = cfg.allow_localhost.value_or(false);
  options.pass_env.insert(options.pass_env.begin(), cfg.pass_env.begin(), cfg.pass_env.end());
  options.host_labels.insert(options.host_labels.begin(), cfg.host_labels.begin(), cfg.host_labels.end());
  options.parallel = o.parallel >= 0 ? o.parallel : cfg.parallel.value_or(0);
  if (options.cache_dir.empty() && !cfg.cache_dir.empty()) options.cache_dir = config_file(cfg.cache_dir);
  if (!options.cache_dir.empty()) options.cache_dir = config::expand_user(options.cache_dir.string());
  if (!options.run_dir.empty()) options.run_dir = config::expand_user(options.run_dir.string());
  if (!cfg.node.empty()) options.node = config_file(cfg.node);
  options.environments = cfg.environments;
  for (auto& [name, env] : options.environments) {
    (void)name;
    if (!env.secrets_file.empty()) env.secrets_file = config_file(env.secrets_file).string();
    if (!env.vars_file.empty()) env.vars_file = config_file(env.vars_file).string();
  }
  auto load_values = [](auto& values, const fs::path& path) {
    for (const auto& [key, value] : config::load_dotenv(path)) values[key] = value;
  };
  if (!cfg.secrets_file.empty()) load_values(options.secrets, config_file(cfg.secrets_file));
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
  if (const auto it = options.secrets.find("GITHUB_TOKEN"); it != options.secrets.end()) {
    options.github_token = it->second;
  }
  if (!cfg.vars_file.empty()) load_values(options.vars, config_file(cfg.vars_file));
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
  run->add_option("--config", run_opts.config_path,
                  "Explicit trusted configuration file (never auto-load repository config)");
  run->add_flag("--no-sandbox", run_opts.no_sandbox, "DANGER: disable filesystem and network isolation");
  run->add_option("--allow-host", run_opts.options.sandbox.allow_hosts, "Additional allowed network host (repeatable)");
  run->add_option("--allow-read", run_opts.allow_read, "Additional readable host path (repeatable)");
  run->add_option("--pass-env,--env-pass", run_opts.options.pass_env,
                  "Pass host variable NAME or explicit NAME=VALUE (repeatable)");
  run->add_flag("--keep-workdir", run_opts.options.keep_workdir, "Keep job workspaces after execution");
  run->add_option("--run-dir", run_opts.options.run_dir,
                  "Run output directory (must not already exist; default: a unique directory under the cache)");
  run->add_option("--cache-dir", run_opts.options.cache_dir, "Shared cache directory");
  run->add_option("-j,--parallel", run_opts.parallel, "Concurrent jobs (0 = CPU count)")->check(CLI::NonNegativeNumber);
  run->add_option("--host-label", run_opts.options.host_labels,
                  "Explicit runs-on label to execute on this host (repeatable)");
  run->add_option("-s,--secret", run_opts.secrets, "Secret NAME from host environment or NAME=VALUE (repeatable)");
  run->add_option("--secret-file,--secrets-file", run_opts.secret_files, "Explicit dotenv secrets file (repeatable)");
  run->add_option("--var", run_opts.vars, "Workflow variable NAME=VALUE (repeatable)");
  run->add_option("--var-file,--vars-file", run_opts.var_files, "Explicit dotenv variables file (repeatable)");
  run->add_flag("--debug", run_opts.options.debug, "Show workflow debug commands");
  run->add_flag("--no-color", run_opts.no_color, "Disable colored logs");

  CLI11_PARSE(app, argc, argv);

  try {
    if (validate->parsed()) return cmd_validate(validate_opts);
    if (list->parsed()) return cmd_list(list_opts);
    if (graph->parsed()) return cmd_graph(graph_opts, graph_selectors, graph_format);
    if (run->parsed()) return cmd_run(run_opts);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
  return 2;
}
