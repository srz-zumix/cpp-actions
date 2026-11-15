#pragma once

#include <string>

#include "expr/expression.hpp"
#include "workflow/yaml.hpp"

namespace cpa::workflow {

// Expression sites with their available contexts (see "Contexts" /
// "Context availability" in the GitHub Actions documentation).
enum class Site {
  RunName,
  WorkflowEnv,
  WorkflowConcurrency,
  JobIf,
  JobStrategy,
  JobGeneral,  // runs-on, name, timeout-minutes, continue-on-error, environment, container, services, concurrency
  JobEnv,
  JobOutputs,
  JobDefaultsRun,
  StepIf,
  StepGeneral,  // run, env, with, name, working-directory, shell, timeout-minutes, continue-on-error
  ActionInputDefault,
  ActionStepIf,
  ActionStepGeneral,
  ActionOutputs,
};

expr::Scope scope_for(Site site);

// Converts an `if` value into the expression that is evaluated, following
// the runner: `${{ }}` templates are converted, an empty condition becomes
// `success()` and conditions without a status function are wrapped as
// `success() && (<condition>)`. Throws expr::ParseError.
std::string condition_expression(const yaml::Node* node, const expr::Scope& scope);

}  // namespace cpa::workflow
