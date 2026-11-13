#include "workflow/scopes.hpp"

#include "expr/template.hpp"
#include "util/strings.hpp"

namespace cpa::workflow {

expr::Scope scope_for(Site site) {
  expr::Scope s;
  switch (site) {
    case Site::RunName:
    case Site::WorkflowConcurrency:
      s.named_values = {"github", "inputs", "vars"};
      break;
    case Site::WorkflowEnv:
      s.named_values = {"github", "secrets", "inputs", "vars"};
      break;
    case Site::JobIf:
      s.named_values = {"github", "needs", "vars", "inputs"};
      s.status_functions = true;
      break;
    case Site::JobStrategy:
      s.named_values = {"github", "needs", "vars", "inputs"};
      break;
    case Site::JobGeneral:
      s.named_values = {"github", "needs", "strategy", "matrix", "vars", "inputs"};
      break;
    case Site::JobEnv:
      s.named_values = {"github", "needs", "strategy", "matrix", "vars", "secrets", "inputs"};
      break;
    case Site::JobOutputs:
      s.named_values = {"github", "needs", "strategy", "matrix", "job", "runner", "env", "vars", "secrets", "steps",
                        "inputs"};
      break;
    case Site::JobDefaultsRun:
      s.named_values = {"github", "needs", "strategy", "matrix", "env", "vars", "inputs"};
      break;
    case Site::StepIf:
      s.named_values = {"github", "needs", "strategy", "matrix", "job", "runner", "env", "vars", "steps", "inputs"};
      s.status_functions = true;
      s.hash_files = true;
      break;
    case Site::StepGeneral:
      s.named_values = {"github", "needs", "strategy", "matrix", "job", "runner", "env", "vars", "secrets", "steps",
                        "inputs"};
      s.hash_files = true;
      break;
    case Site::ActionInputDefault:
      s.named_values = {"github", "strategy", "matrix", "job", "runner", "env", "vars", "steps", "inputs"};
      s.hash_files = true;
      break;
    case Site::ActionStepIf:
      s.named_values = {"github", "strategy", "matrix", "job", "runner", "env", "vars", "steps", "inputs"};
      s.status_functions = true;
      s.hash_files = true;
      break;
    case Site::ActionStepGeneral:
    case Site::ActionOutputs:
      s.named_values = {"github", "strategy", "matrix", "job", "runner", "env", "vars", "steps", "inputs"};
      s.hash_files = true;
      break;
  }
  return s;
}

std::string condition_expression(const yaml::Node* node, const expr::Scope& scope) {
  if (node == nullptr || node->is_null()) return "success()";
  if (!node->is_scalar()) throw expr::ParseError("A condition must be a string, boolean or number");
  std::string text(util::trim(node->scalar_string()));
  if (text.empty()) return "success()";
  if (expr::contains_expression(text)) text = expr::template_to_expression(expr::split_template(text));
  if (util::trim(text).empty()) throw expr::ParseError("An expression was expected");
  const auto parsed = expr::Expression::parse(text, scope);
  if (parsed.has_status_function()) return text;
  return "success() && (" + text + ")";
}

}  // namespace cpa::workflow
