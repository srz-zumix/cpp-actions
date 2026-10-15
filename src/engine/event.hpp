#pragma once

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#include "expr/value.hpp"
#include "workflow/events.hpp"
#include "workflow/model.hpp"
#include "workspace/git.hpp"

namespace cpa::engine {

class EventError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct EventOptions {
  std::string name = "push";
  std::filesystem::path payload_file;  // JSON payload; generated when empty
  std::string ref;                     // full ref or branch name
  std::string base_ref;                // pull requests: target branch
  std::string head_ref;                // pull requests: source branch
  std::string activity_type;           // payload "action"
  std::string changes_base;            // revision for changed files (automatic when empty)
  std::string actor;
};

struct Event {
  workflow::EventInfo info;  // name, payload, ref, base_ref, action, changed_files
  std::string sha;           // commit of the event
  std::string head_ref;      // pull requests: source branch
  std::string actor;
  std::string changes_basis;  // how the changed files were determined
};

Event build_event(const EventOptions& options, const workspace::GitInfo& git);

// Human-readable summary (name, ref, sha, actor, changed files).
std::string render_event(const Event& event);

// "refs/heads/main" -> "main", "refs/tags/v1" -> "v1", "refs/pull/1/merge" -> "1/merge".
std::string ref_short_name(std::string_view ref);
// "branch", "tag" or "" (pull request refs).
std::string ref_type(std::string_view ref);
// Normalizes a branch name or ref to a full ref ("main" -> "refs/heads/main").
std::string normalize_ref(std::string_view ref);

// Typed `inputs` context of a workflow_dispatch run (booleans and numbers are
// converted). Throws EventError for unknown, missing or invalid inputs.
expr::Value dispatch_inputs(const workflow::Workflow& workflow, const std::map<std::string, std::string>& given);
// `github.event.inputs` (string values) for the typed inputs.
expr::Value dispatch_event_inputs(const expr::Value& typed_inputs);

}  // namespace cpa::engine
