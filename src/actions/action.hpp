#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "workflow/model.hpp"
#include "workflow/parser.hpp"
#include "workflow/yaml.hpp"

namespace cpa::actions {

namespace fs = std::filesystem;

class ActionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A parsed `uses:` value of a step.
struct ActionRef {
  enum class Kind { Local, Remote, Docker };
  Kind kind = Kind::Remote;
  std::string owner;
  std::string repo;
  std::string path;  // sub directory in the repository ("" for the root)
  std::string ref;
  std::string local_path;  // Kind::Local: "./path"
  std::string image;       // Kind::Docker

  std::string full_name() const { return owner + "/" + repo; }
  // owner/repo[/path] (lower case comparisons are up to the caller)
  std::string name() const { return path.empty() ? full_name() : full_name() + "/" + path; }
  std::string display() const;
};

// Parses "./path", "docker://image" or "owner/repo[/path]@ref".
ActionRef parse_action_ref(std::string_view uses);

// Only the root action in the official repository receives hosted Python cache access.
bool is_setup_python_root(const ActionRef& ref);

// Non-Docker action selector; '*' is allowed only in a remote ref.
void validate_action_permission_selector(std::string_view selector);
bool matches_action_permission(std::string_view selector, const ActionRef& action);

struct ActionInput {
  std::string name;
  std::string description;
  bool required = false;
  const yaml::Node* default_value = nullptr;
  std::string deprecation_message;
};

struct ActionOutput {
  std::string name;
  const yaml::Node* value = nullptr;  // composite actions only
};

enum class ActionKind { Node, Composite, Docker };

// action.yml / action.yaml
struct Action {
  std::shared_ptr<yaml::Document> document;
  fs::path dir;
  std::string file;  // display path of action.yml
  std::string name;
  std::vector<ActionInput> inputs;
  std::vector<ActionOutput> outputs;
  ActionKind kind = ActionKind::Node;
  std::string runtime;  // runs.using
  std::string main;
  std::string pre;
  std::string post;
  const yaml::Node* pre_if = nullptr;
  const yaml::Node* post_if = nullptr;
  const yaml::Node* runs = nullptr;
  std::vector<workflow::Step> steps;  // composite
  std::vector<workflow::Diagnostic> warnings;

  const ActionInput* find_input(std::string_view name) const;
};

// Loads action.yml (or action.yaml) from `dir`. Throws ActionError.
Action load_action(const fs::path& dir, const std::string& display);

// Node.js major version for runs.using (node12/node16 are run with node20).
int node_major_version(std::string_view runtime);

}  // namespace cpa::actions
