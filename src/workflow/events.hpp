#pragma once

#include <optional>
#include <string>
#include <vector>

#include "expr/value.hpp"
#include "workflow/model.hpp"

namespace cpa::workflow {

struct EventInfo {
  std::string name = "push";
  expr::Value payload = expr::Value::object();  // github.event
  std::string ref;                              // refs/heads/<b>, refs/tags/<t>, refs/pull/<n>/merge
  std::string base_ref;                         // pull_request: target branch
  std::string action;                           // activity type (payload.action)
  // Files changed by the event for `paths` filters; nullopt = unknown.
  std::optional<std::vector<std::string>> changed_files;
};

struct TriggerMatch {
  bool matched = false;
  std::string reason;  // why the workflow does (not) run
};

TriggerMatch match_trigger(const Workflow& workflow, const EventInfo& event);

// Default activity types of pull_request / pull_request_target.
const std::vector<std::string>& default_pull_request_types();

}  // namespace cpa::workflow
