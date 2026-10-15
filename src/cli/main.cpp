#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>

#include "config/config.hpp"
#include "engine/catalog.hpp"
#include "engine/event.hpp"
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
    if (name.empty() || !value) throw std::runtime_error(std::string(what) + " must be NAME=VALUE: '" + item + "'");
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
  bool dry_run = false;
};

int cmd_run(RunCli& o) {
  const auto root = repository_root(o.common);
  const auto files = load_files(o.common, root);
  const auto git = workspace::read_git_info(root);
  const auto event = engine::build_event(o.event, git);
  const auto selection = engine::select_workflows(files, event.info, o.selectors);
  const auto inputs = parse_assignments(o.inputs, "--input");
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
  if (!o.dry_run) {
    std::cerr << "error: workflow execution is not implemented yet (use --dry-run)\n";
    return 2;
  }
  return 0;
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
