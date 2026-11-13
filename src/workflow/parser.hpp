#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "workflow/model.hpp"

namespace cpa::workflow {

// Converts a parsed YAML document into a workflow and validates it. All
// problems are collected; throws WorkflowError when there is at least one
// error. Warnings are appended to `warnings` when given.
Workflow parse_workflow(std::shared_ptr<yaml::Document> document, std::string file,
                        std::vector<Diagnostic>* warnings = nullptr);

// Parses and validates the steps of a composite action (`runs.steps`).
// Throws WorkflowError.
std::vector<Step> parse_action_steps(const yaml::Node& steps, std::string file,
                                     std::vector<Diagnostic>* warnings = nullptr);

// Trusted local setup: run steps only, without continue-on-error.
std::vector<Step> parse_setup_steps(const yaml::Node& steps, const std::string& file);

// Loads and parses a workflow file (YAML errors are reported as WorkflowError).
Workflow load_workflow(const std::filesystem::path& path, std::string display_name,
                       std::vector<Diagnostic>* warnings = nullptr);

// Returns `.github/workflows/*.yml|*.yaml` below the repository root, sorted.
std::vector<std::filesystem::path> discover_workflows(const std::filesystem::path& repo_root);

// Events that can be used in `on`.
bool is_known_event(std::string_view event);

}  // namespace cpa::workflow
