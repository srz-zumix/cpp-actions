#include "actions/action.hpp"

#include <cctype>
#include <set>

#include "util/strings.hpp"

namespace cpa::actions {

namespace {

bool valid_name_part(std::string_view s) {
  if (s.empty() || s == "." || s == "..") return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}

std::string diagnostics_message(const workflow::WorkflowError& e) {
  std::string message;
  for (const auto& d : e.diagnostics()) {
    if (!message.empty()) message += "\n";
    message += d.to_string();
  }
  return message;
}

}  // namespace

std::string ActionRef::display() const {
  switch (kind) {
    case Kind::Local:
      return local_path;
    case Kind::Docker:
      return "docker://" + image;
    case Kind::Remote:
      break;
  }
  return name() + "@" + ref;
}

ActionRef parse_action_ref(std::string_view uses) {
  ActionRef ref;
  if (uses.starts_with("./") || uses == ".") {
    ref.kind = ActionRef::Kind::Local;
    ref.local_path = std::string(uses);
    return ref;
  }
  if (uses.starts_with("docker://")) {
    ref.kind = ActionRef::Kind::Docker;
    ref.image = std::string(uses.substr(9));
    return ref;
  }
  const auto at = uses.find('@');
  if (at == std::string_view::npos || at + 1 == uses.size()) {
    throw ActionError("Expected format {org}/{repo}[/path]@ref. Actual '" + std::string(uses) + "'");
  }
  ref.ref = std::string(uses.substr(at + 1));
  const auto parts = util::split(uses.substr(0, at), '/');
  if (parts.size() < 2 || !valid_name_part(parts[0]) || !valid_name_part(parts[1])) {
    throw ActionError("Expected format {org}/{repo}[/path]@ref. Actual '" + std::string(uses) + "'");
  }
  ref.owner = parts[0];
  ref.repo = parts[1];
  for (std::size_t i = 2; i < parts.size(); ++i) {
    if (!valid_name_part(parts[i])) throw ActionError("Invalid action path in '" + std::string(uses) + "'");
    if (!ref.path.empty()) ref.path += "/";
    ref.path += parts[i];
  }
  for (char c : ref.ref) {
    if (std::iscntrl(static_cast<unsigned char>(c)) || c == ' ' || c == '~' || c == '^' || c == ':' || c == '\\') {
      throw ActionError("Invalid ref in '" + std::string(uses) + "'");
    }
  }
  if (ref.ref.starts_with("-") || ref.ref.find("..") != std::string::npos) {
    throw ActionError("Invalid ref in '" + std::string(uses) + "'");
  }
  return ref;
}

const ActionInput* Action::find_input(std::string_view input) const {
  for (const auto& in : inputs) {
    if (util::iequals(in.name, input)) return &in;
  }
  return nullptr;
}

int node_major_version(std::string_view runtime) {
  if (util::iequals(runtime, "node24")) return 24;
  return 20;
}

Action load_action(const fs::path& dir, const std::string& display) {
  Action action;
  action.dir = dir;
  fs::path file = dir / "action.yml";
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) file = dir / "action.yaml";
  if (!fs::is_regular_file(file, ec)) {
    if (fs::is_regular_file(dir / "Dockerfile", ec)) {
      throw ActionError("Docker actions are not supported: '" + display + "' has a Dockerfile but no action.yml");
    }
    throw ActionError("Can't find 'action.yml' or 'action.yaml' under '" + display + "'");
  }
  action.file = display.empty() ? file.string() : display + "/" + file.filename().string();
  try {
    action.document = yaml::load_file(file, action.file);
  } catch (const yaml::LoadError& e) {
    throw ActionError(e.what());
  }
  const yaml::Node* root = action.document->root;
  auto fail = [&](const yaml::Node* node, const std::string& message) -> ActionError {
    return ActionError(workflow::Diagnostic{action.file, node != nullptr ? node->mark : yaml::Mark{}, message, false}
                           .to_string());
  };
  if (root == nullptr || !root->is_mapping()) throw fail(root, "The action metadata must be a mapping");
  if (const auto* n = root->get("name")) action.name = n->scalar_string();

  if (const auto* inputs = root->get("inputs"); inputs != nullptr && !inputs->is_null()) {
    if (!inputs->is_mapping()) throw fail(inputs, "'inputs' must be a mapping");
    for (const auto& e : inputs->entries) {
      ActionInput in;
      in.name = e.key->text;
      if (e.value->is_mapping()) {
        if (const auto* d = e.value->get("description")) in.description = d->scalar_string();
        if (const auto* r = e.value->get("required")) {
          in.required = r->kind == yaml::NodeKind::Boolean ? r->boolean : util::iequals(r->scalar_string(), "true");
        }
        in.default_value = e.value->get("default");
        if (const auto* d = e.value->get("deprecationMessage")) in.deprecation_message = d->scalar_string();
      }
      action.inputs.push_back(std::move(in));
    }
  }

  const yaml::Node* runs = root->get("runs");
  if (runs == nullptr || !runs->is_mapping()) throw fail(root, "Required property is missing: runs");
  action.runs = runs;
  const yaml::Node* using_node = runs->get("using");
  if (using_node == nullptr) throw fail(runs, "Required property is missing: using");
  action.runtime = util::to_lower(using_node->scalar_string());

  if (const auto* outputs = root->get("outputs"); outputs != nullptr && !outputs->is_null()) {
    if (!outputs->is_mapping()) throw fail(outputs, "'outputs' must be a mapping");
    for (const auto& e : outputs->entries) {
      ActionOutput out;
      out.name = e.key->text;
      if (e.value->is_mapping()) out.value = e.value->get("value");
      action.outputs.push_back(out);
    }
  }

  if (action.runtime == "docker") {
    action.kind = ActionKind::Docker;
    return action;
  }
  if (action.runtime == "composite") {
    action.kind = ActionKind::Composite;
    const yaml::Node* steps = runs->get("steps");
    if (steps == nullptr) throw fail(runs, "Required property is missing: steps");
    if (!steps->is_sequence()) throw fail(steps, "'steps' must be a sequence");
    try {
      action.steps = workflow::parse_action_steps(*steps, action.file, &action.warnings);
    } catch (const workflow::WorkflowError& e) {
      throw ActionError(diagnostics_message(e));
    }
    return action;
  }
  static const std::set<std::string> node_runtimes = {"node12", "node16", "node20", "node24"};
  if (node_runtimes.count(action.runtime) == 0) {
    throw fail(using_node, "Unsupported 'using' value '" + action.runtime +
                               "'. Expected node20, node24, composite or docker");
  }
  action.kind = ActionKind::Node;
  auto string_of = [&](const char* key) -> std::string {
    const auto* n = runs->get(key);
    return n != nullptr ? n->scalar_string() : std::string();
  };
  action.main = string_of("main");
  if (action.main.empty()) throw fail(runs, "Required property is missing: main");
  action.pre = string_of("pre");
  action.post = string_of("post");
  action.pre_if = runs->get("pre-if");
  action.post_if = runs->get("post-if");
  return action;
}

}  // namespace cpa::actions
