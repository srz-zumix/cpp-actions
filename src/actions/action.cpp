#include "actions/action.hpp"

#include <cctype>
#include <set>

#include "util/glob.hpp"
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

bool is_setup_python_root(const ActionRef& ref) {
  return ref.kind == ActionRef::Kind::Remote && ref.path.empty() && util::iequals(ref.owner, "actions") &&
         util::iequals(ref.repo, "setup-python");
}

namespace {

ActionRef parse_action_permission_selector(std::string_view selector) {
  auto ref = parse_action_ref(selector);
  bool invalid = ref.kind == ActionRef::Kind::Docker || selector.find_first_of("?[]") != std::string_view::npos ||
                 selector.find("${{") != std::string_view::npos;
  for (char c : selector) invalid = invalid || std::iscntrl(static_cast<unsigned char>(c));
  if (ref.kind == ActionRef::Kind::Local) {
    invalid = invalid || selector.find('*') != std::string_view::npos;
    for (const auto& part : fs::path(ref.local_path)) invalid = invalid || part == "..";
  }
  if (invalid) throw ActionError("Invalid action permission selector '" + std::string(selector) +
                                 "': use an exact local path or owner/repo[/path]@ref; only '*' in ref is supported");
  return ref;
}

}  // namespace

void validate_action_permission_selector(std::string_view selector) {
  (void)parse_action_permission_selector(selector);
}

bool matches_action_permission(std::string_view selector, const ActionRef& action) {
  const auto allowed = parse_action_permission_selector(selector);
  if (allowed.kind == ActionRef::Kind::Local) {
    return action.kind == ActionRef::Kind::Local && allowed.local_path == action.local_path;
  }
  return allowed.kind == ActionRef::Kind::Remote && action.kind == ActionRef::Kind::Remote &&
         allowed.name() == action.name() && util::glob_match_string(allowed.ref, action.ref);
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
    std::set<std::string> names;
    for (const auto& e : inputs->entries) {
      if (!e.key->is_string() || e.key->text.empty()) throw fail(e.key, "Input names must be non-empty strings");
      if (!names.insert(util::to_lower(e.key->text)).second) throw fail(e.key, "Duplicate input name");
      if (!e.value->is_mapping()) throw fail(e.value, "Input metadata must be a mapping");
      ActionInput in;
      in.name = e.key->text;
      if (e.value->is_mapping()) {
        if (const auto* d = e.value->get("description")) in.description = d->scalar_string();
        if (const auto* r = e.value->get("required")) {
          in.required = r->kind == yaml::NodeKind::Boolean ? r->boolean : util::iequals(r->scalar_string(), "true");
        }
        in.default_value = e.value->get("default");
        if (in.default_value != nullptr && !in.default_value->is_scalar()) {
          throw fail(in.default_value, "Input defaults must be scalar values");
        }
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
  if (!using_node->is_string()) throw fail(using_node, "'using' must be a string");
  action.runtime = util::to_lower(using_node->scalar_string());

  if (const auto* outputs = root->get("outputs"); outputs != nullptr && !outputs->is_null()) {
    if (!outputs->is_mapping()) throw fail(outputs, "'outputs' must be a mapping");
    std::set<std::string> names;
    for (const auto& e : outputs->entries) {
      if (!e.key->is_string() || e.key->text.empty()) throw fail(e.key, "Output names must be non-empty strings");
      if (!names.insert(util::to_lower(e.key->text)).second) throw fail(e.key, "Duplicate output name");
      if (!e.value->is_mapping()) throw fail(e.value, "Output metadata must be a mapping");
      ActionOutput out;
      out.name = e.key->text;
      if (e.value->is_mapping()) out.value = e.value->get("value");
      if (out.value != nullptr && !out.value->is_scalar()) throw fail(out.value, "Output values must be scalar");
      if (action.runtime == "composite" && out.value == nullptr) {
        throw fail(e.value, "Required property is missing: value");
      }
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
    if (n != nullptr && !n->is_string()) throw fail(n, std::string("'") + key + "' must be a string");
    return n != nullptr ? n->scalar_string() : std::string();
  };
  action.main = string_of("main");
  if (action.main.empty()) throw fail(runs, "Required property is missing: main");
  action.pre = string_of("pre");
  action.post = string_of("post");
  action.pre_if = runs->get("pre-if");
  action.post_if = runs->get("post-if");
  for (const auto* condition : {action.pre_if, action.post_if}) {
    if (condition != nullptr && !condition->is_scalar()) throw fail(condition, "Action conditions must be scalar");
  }
  return action;
}

}  // namespace cpa::actions
