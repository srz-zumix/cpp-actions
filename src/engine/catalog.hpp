#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "workflow/events.hpp"
#include "workflow/model.hpp"

namespace cpa::engine {

namespace fs = std::filesystem;

struct WorkflowFile {
  fs::path path;         // absolute path
  std::string display;   // path relative to the repository root
  std::shared_ptr<const workflow::Workflow> workflow;  // null when invalid
  std::vector<workflow::Diagnostic> errors;
  std::vector<workflow::Diagnostic> warnings;
  bool valid() const { return workflow != nullptr; }
};

// Loads workflow files. `files` may contain files or directories; when it is
// empty, `.github/workflows` of the repository is used.
std::vector<WorkflowFile> load_workflow_files(const fs::path& repo_root, const std::vector<fs::path>& files);

// True when `selector` is the path, the file name (with or without the
// extension) or the `name` of the workflow.
bool workflow_matches(const WorkflowFile& file, std::string_view selector);

struct Selection {
  const WorkflowFile* file = nullptr;
  workflow::TriggerMatch match;
};

// Matches the valid workflows against an event. When `selectors` is not
// empty, only the workflows named by a selector are considered. Throws
// std::runtime_error when a selector does not match any workflow.
std::vector<Selection> select_workflows(const std::vector<WorkflowFile>& files, const workflow::EventInfo& event,
                                        const std::vector<std::string>& selectors);

// Diagnostics of all files followed by a summary line.
std::string render_validation(const std::vector<WorkflowFile>& files);

// Workflows, triggers, jobs (with statically expanded matrices).
std::string render_list(const std::vector<WorkflowFile>& files);

// Jobs grouped by dependency level ("stages").
std::string render_graph_text(const workflow::Workflow& wf);
// Graphviz DOT for several workflows.
std::string render_graph_dot(const std::vector<const workflow::Workflow*>& workflows);

// Dependency level (1 = no needs) of every job, in document order.
std::vector<int> job_levels(const workflow::Workflow& wf);

// Literal text of a value that may contain expressions (sequences are joined
// with ", ", mappings are written as "key: value").
std::string describe_token(const yaml::Node* node);

}  // namespace cpa::engine
