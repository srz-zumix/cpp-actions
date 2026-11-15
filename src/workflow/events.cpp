#include "workflow/events.hpp"

#include <algorithm>

#include "util/glob.hpp"
#include "util/strings.hpp"

namespace cpa::workflow {

namespace {

std::vector<std::string> string_list(const yaml::Node* n) {
  std::vector<std::string> out;
  if (n == nullptr || n->is_null()) return out;
  if (n->is_sequence()) {
    for (const auto* item : n->items) out.push_back(item->scalar_string());
  } else {
    out.push_back(n->scalar_string());
  }
  return out;
}

// Positive list: the value must match (negations with '!').
bool included(const yaml::Node* n, std::string_view value) { return util::filter_list_match(string_list(n), value); }

struct RefMatch {
  bool matched = true;
  std::string reason;
};

RefMatch check_ref_filters(const yaml::Node& config, std::string_view kind, std::string_view name) {
  const std::string pos(kind);
  const std::string neg = pos + "-ignore";
  const yaml::Node* p = config.get(pos);
  const yaml::Node* n = config.get(neg);
  if (p != nullptr && !included(p, name)) return {false, "'" + std::string(name) + "' does not match '" + pos + "'"};
  if (n != nullptr && included(n, name)) return {false, "'" + std::string(name) + "' matches '" + neg + "'"};
  return {};
}

RefMatch check_paths(const yaml::Node& config, const EventInfo& event) {
  const yaml::Node* paths = config.get("paths");
  const yaml::Node* ignore = config.get("paths-ignore");
  if (paths == nullptr && ignore == nullptr) return {};
  if (!event.changed_files) return {true, "changed files are unknown; 'paths' filters were not evaluated"};
  const auto& files = *event.changed_files;
  if (paths != nullptr) {
    const auto patterns = string_list(paths);
    const bool any = std::any_of(files.begin(), files.end(),
                                 [&](const std::string& f) { return util::filter_list_match(patterns, f); });
    if (!any) return {false, "no changed file matches 'paths'"};
  }
  if (ignore != nullptr) {
    const auto patterns = string_list(ignore);
    const bool all_ignored = !files.empty() && std::all_of(files.begin(), files.end(), [&](const std::string& f) {
      return util::filter_list_match(patterns, f);
    });
    if (all_ignored) return {false, "all changed files match 'paths-ignore'"};
  }
  return {};
}

bool type_allowed(const yaml::Node& config, const std::string& action, const std::vector<std::string>& defaults) {
  const yaml::Node* types = config.get("types");
  const auto list = types != nullptr ? string_list(types) : defaults;
  if (list.empty() || action.empty()) return true;
  return std::find(list.begin(), list.end(), action) != list.end();
}

}  // namespace

const std::vector<std::string>& default_pull_request_types() {
  static const std::vector<std::string> types = {"opened", "synchronize", "reopened"};
  return types;
}

TriggerMatch match_trigger(const Workflow& workflow, const EventInfo& event) {
  const Trigger* trigger = workflow.find_trigger(event.name);
  if (trigger == nullptr) return {false, "the workflow is not triggered by '" + event.name + "'"};
  const yaml::Node* config = trigger->config;
  if (config == nullptr || !config->is_mapping()) return {true, "triggered by '" + event.name + "'"};

  if (event.name == "push") {
    const bool branch_filters = config->get("branches") != nullptr || config->get("branches-ignore") != nullptr;
    const bool tag_filters = config->get("tags") != nullptr || config->get("tags-ignore") != nullptr;
    std::string_view ref = event.ref;
    RefMatch m;
    if (ref.starts_with("refs/tags/")) {
      const auto tag = ref.substr(10);
      if (tag_filters) {
        m = check_ref_filters(*config, "tags", tag);
      } else if (branch_filters) {
        m = {false, "only branch filters are defined and '" + std::string(ref) + "' is a tag"};
      }
    } else {
      const auto branch = ref.starts_with("refs/heads/") ? ref.substr(11) : ref;
      if (branch_filters) {
        m = check_ref_filters(*config, "branches", branch);
      } else if (tag_filters) {
        m = {false, "only tag filters are defined and '" + std::string(ref) + "' is a branch"};
      }
    }
    if (!m.matched) return {false, m.reason};
    m = check_paths(*config, event);
    if (!m.matched) return {false, m.reason};
    return {true, m.reason.empty() ? "triggered by 'push'" : m.reason};
  }

  if (event.name == "pull_request" || event.name == "pull_request_target") {
    if (!type_allowed(*config, event.action, default_pull_request_types())) {
      return {false, "activity type '" + event.action + "' is not listed in 'types'"};
    }
    auto m = check_ref_filters(*config, "branches", event.base_ref);
    if (!m.matched) return {false, m.reason};
    m = check_paths(*config, event);
    if (!m.matched) return {false, m.reason};
    return {true, m.reason.empty() ? "triggered by '" + event.name + "'" : m.reason};
  }

  if (event.name == "workflow_run") {
    const auto workflows = string_list(config->get("workflows"));
    const auto name = event.payload.get("workflow").get("name").to_string();
    if (!workflows.empty() && std::find(workflows.begin(), workflows.end(), name) == workflows.end()) {
      return {false, "workflow '" + name + "' is not listed in 'workflows'"};
    }
    if (!type_allowed(*config, event.action, {})) {
      return {false, "activity type '" + event.action + "' is not listed in 'types'"};
    }
    const auto branch = event.payload.get("workflow_run").get("head_branch").to_string();
    auto m = check_ref_filters(*config, "branches", branch);
    if (!m.matched) return {false, m.reason};
    return {true, "triggered by 'workflow_run'"};
  }

  if (!type_allowed(*config, event.action, {})) {
    return {false, "activity type '" + event.action + "' is not listed in 'types'"};
  }
  return {true, "triggered by '" + event.name + "'"};
}

}  // namespace cpa::workflow
