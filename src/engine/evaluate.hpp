#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "expr/expression.hpp"
#include "workflow/yaml.hpp"

namespace cpa::engine {

// Evaluation failure with the source location ("file:line:col: message").
class EvaluationError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string location(std::string_view file, const yaml::Node* node);

// Evaluates a YAML token: string scalars are templates, sequences and
// mappings are evaluated recursively. A null node evaluates to null.
expr::Value evaluate_token(const yaml::Node* node, const expr::EvalContext& context, const expr::Scope& scope,
                           std::string_view file);

// Evaluates a token that must produce a scalar and converts it to a string.
std::string evaluate_string(const yaml::Node* node, const expr::EvalContext& context, const expr::Scope& scope,
                            std::string_view file);

// Evaluates an `if` condition (null = success()).
bool evaluate_condition(const yaml::Node* node, const expr::EvalContext& context, const expr::Scope& scope,
                        std::string_view file);

// Evaluates a mapping of strings (env, with, outputs). The mapping itself may
// be an expression producing an object.
std::vector<std::pair<std::string, std::string>> evaluate_string_map(const yaml::Node* node,
                                                                     const expr::EvalContext& context,
                                                                     const expr::Scope& scope, std::string_view file);

// Converts a scalar value to a string; throws EvaluationError for arrays and
// objects.
std::string scalar_to_string(const expr::Value& value, std::string_view where);

// Evaluates concurrency for diagnostics only; no locking or cancellation is applied.
std::string describe_concurrency(const yaml::Node* node, const expr::EvalContext& context,
                                 const expr::Scope& scope, const std::string& file);

}  // namespace cpa::engine
