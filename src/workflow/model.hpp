#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "workflow/yaml.hpp"

namespace cpa::workflow {

struct Diagnostic {
  std::string file;
  yaml::Mark mark;
  std::string message;
  bool warning = false;

  // "file:line:col: error: message"
  std::string to_string() const;
};

class WorkflowError : public std::runtime_error {
 public:
  explicit WorkflowError(std::vector<Diagnostic> diagnostics);
  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

 private:
  std::vector<Diagnostic> diagnostics_;
};

// Values that may contain expressions are kept as YAML nodes and evaluated
// when the job / step runs (`const yaml::Node*`, nullptr when absent).
using Token = const yaml::Node*;

struct Trigger {
  std::string event;
  Token config = nullptr;  // null, mapping or (schedule) sequence
};

struct DispatchInput {
  std::string name;
  std::string description;
  std::string type = "string";  // string, boolean, choice, number, environment
  bool required = false;
  Token default_value = nullptr;
  std::vector<std::string> options;
};

// on.workflow_call
struct CallInput {
  std::string name;
  std::string description;
  std::string type = "string";  // string, boolean, number
  bool required = false;
  Token default_value = nullptr;
};

struct CallOutput {
  std::string name;
  std::string description;
  Token value = nullptr;
};

struct CallSecret {
  std::string name;
  bool required = false;
};

struct Step {
  int index = 0;           // 0-based position in the job
  std::string id;          // explicit id or generated ("__run", "__actions_checkout", ...)
  bool explicit_id = false;
  Token node = nullptr;    // the step mapping
  Token name = nullptr;
  Token condition = nullptr;  // `if`
  Token run = nullptr;
  std::string uses;
  Token shell = nullptr;
  Token working_directory = nullptr;
  Token env = nullptr;
  Token with = nullptr;
  Token continue_on_error = nullptr;
  Token timeout_minutes = nullptr;

  bool is_run() const { return run != nullptr; }
  // Display name used when `name` is not set ("Run echo hi", "Run actions/checkout@v4").
  std::string default_display_name() const;
};

struct Job {
  std::string id;
  Token node = nullptr;
  Token name = nullptr;
  std::vector<std::string> needs;
  Token condition = nullptr;
  Token runs_on = nullptr;
  Token environment = nullptr;
  Token concurrency = nullptr;
  Token outputs = nullptr;
  Token env = nullptr;
  Token defaults = nullptr;
  Token strategy = nullptr;
  Token timeout_minutes = nullptr;
  Token continue_on_error = nullptr;
  Token container = nullptr;
  Token services = nullptr;
  Token permissions = nullptr;
  std::string uses;  // reusable workflow call
  Token with = nullptr;
  Token secrets = nullptr;
  std::vector<Step> steps;
};

struct Workflow {
  std::shared_ptr<yaml::Document> document;
  std::string file;  // display path, e.g. ".github/workflows/ci.yml"
  Token name = nullptr;
  Token run_name = nullptr;
  std::vector<Trigger> triggers;
  std::vector<DispatchInput> dispatch_inputs;
  std::vector<CallInput> call_inputs;
  std::vector<CallOutput> call_outputs;
  std::vector<CallSecret> call_secrets;
  Token env = nullptr;
  Token defaults = nullptr;
  Token permissions = nullptr;
  Token concurrency = nullptr;
  std::vector<Job> jobs;  // document order

  const Job* find_job(std::string_view id) const;
  const Trigger* find_trigger(std::string_view event) const;
  // `name` if it is a literal, otherwise the file path.
  std::string display_name() const;
};

bool is_valid_identifier(std::string_view id);

}  // namespace cpa::workflow
