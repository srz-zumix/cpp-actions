#include <CLI/CLI.hpp>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <system_error>
#include <thread>

#include "config/config.hpp"
#include "engine/catalog.hpp"
#include "engine/event.hpp"
#include "engine/runner.hpp"
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
  engine::RunOptions execution;
  std::vector<std::string> secrets;
  std::vector<std::string> vars;
  std::string secrets_file;
  std::string vars_file;
  bool dry_run = false;
  bool no_color = false;
};

// Block signals before the engine starts threads; sigwait handles cancellation
// outside a signal handler, where the runner's locks can safely be used.
class RunSignals {
 public:
  explicit RunSignals(engine::Runner& runner) {
    sigemptyset(&signals_);
    sigaddset(&signals_, SIGINT);
    sigaddset(&signals_, SIGTERM);
    const int error = pthread_sigmask(SIG_BLOCK, &signals_, &previous_);
    if (error != 0) throw std::system_error(error, std::generic_category(), "block cancellation signals");
    try {
      // Background shells can start the CLI with SIGINT ignored. sigwait must
      // receive it on macOS as well, so reset dispositions while blocked.
      struct sigaction action {};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGINT, &action, &previous_int_) != 0) {
        throw std::system_error(errno, std::generic_category(), "reset SIGINT");
      }
      reset_int_ = true;
      if (sigaction(SIGTERM, &action, &previous_term_) != 0) {
        throw std::system_error(errno, std::generic_category(), "reset SIGTERM");
      }
      reset_term_ = true;
      thread_ = std::thread([this, &runner] {
        while (!stopping_) {
          int signal = 0;
          if (sigwait(&signals_, &signal) != 0) return;
          if (stopping_) return;
          interrupted_ = true;
          runner.cancel();
        }
      });
    } catch (...) {
      restore();
      throw;
    }
  }

  ~RunSignals() {
    stopping_ = true;
    pthread_kill(thread_.native_handle(), SIGINT);
    thread_.join();
    restore();
  }

  bool interrupted() const { return interrupted_.load(); }

 private:
  void restore() {
    if (reset_term_) sigaction(SIGTERM, &previous_term_, nullptr);
    if (reset_int_) sigaction(SIGINT, &previous_int_, nullptr);
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }

  sigset_t signals_{};
  sigset_t previous_{};
  struct sigaction previous_int_ {};
  struct sigaction previous_term_ {};
  bool reset_int_ = false;
  bool reset_term_ = false;
  std::atomic<bool> interrupted_{false};
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

void load_values(std::map<std::string, std::string>& values, const std::string& file,
                 const std::vector<std::string>& items, std::string_view option) {
  if (!file.empty()) values = config::load_dotenv(config::expand_user(file));
  for (const auto& [name, value] : parse_assignments(items, option)) values[name] = value;
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

  o.execution.inputs = inputs;
  load_values(o.execution.secrets, o.secrets_file, o.secrets, "--secret");
  load_values(o.execution.vars, o.vars_file, o.vars, "--var");
  if (const auto token = o.execution.secrets.find("GITHUB_TOKEN"); token != o.execution.secrets.end()) {
    o.execution.github_token = token->second;
  }
  o.execution.sandbox.enabled = false;
  std::cerr << "warning: Phase 3 executes without a sandbox; only run trusted workflows. "
               "Steps can access all host files and the network.\n";
  engine::Console console(std::cout, !o.no_color && std::getenv("NO_COLOR") == nullptr && ::isatty(STDOUT_FILENO));
  engine::Runner runner(o.execution, console);
  RunSignals signals(runner);
  const auto report = runner.run(selection, event, git);
  engine::Masker masker;
  for (const auto& [name, value] : o.execution.secrets) masker.add(value);
  console.line("\nResult: " + report.result + " (" + util::format_duration_ms(report.duration_ms) + ")");
  for (const auto& workflow : report.workflows) {
    for (const auto& job : workflow.jobs) {
      console.line(masker.mask("  " + workflow.name + " / " + job.name + ": " + job.result));
      if (!job.summary.empty()) console.line(masker.mask(job.summary));
    }
  }
  console.line("Logs and report: " + report.run_dir);
  if (signals.interrupted() || report.result == "cancelled") return 130;
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
  run->add_option("-j,--parallel", run_opts.execution.parallel, "Maximum concurrent jobs (default: CPU count)")
      ->check(CLI::PositiveNumber);
  run->add_option("--run-dir", run_opts.execution.run_dir, "Run directory (must not already exist)");
  run->add_option("--cache-dir", run_opts.execution.cache_dir, "Shared tool and action cache directory");
  run->add_flag("--keep-workdir", run_opts.execution.keep_workdir, "Keep workspaces and temporary files after the run");
  run->add_option("-s,--secret", run_opts.secrets, "Explicit secret (NAME=VALUE); prefer --secrets-file");
  run->add_option("--secrets-file", run_opts.secrets_file, "Secrets in dotenv format");
  run->add_option("--var", run_opts.vars, "Variable (NAME=VALUE)");
  run->add_option("--vars-file", run_opts.vars_file, "Variables in dotenv format");
  run->add_option("--env-pass", run_opts.execution.pass_env, "Explicit host environment variable (NAME or NAME=VALUE)");
  run->add_option("--host-label", run_opts.execution.host_labels,
                  "Explicitly allow a runner label on this host (debugging; no image emulation)");
  run->add_flag("--debug", run_opts.execution.debug, "Show workflow debug commands");
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
