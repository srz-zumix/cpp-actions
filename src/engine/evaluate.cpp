#include "engine/evaluate.hpp"

#include "expr/template.hpp"
#include "workflow/scopes.hpp"

namespace cpa::engine {

std::string location(std::string_view file, const yaml::Node* node) {
  std::string out(file);
  if (node != nullptr && node->mark.line > 0) {
    out += ":" + std::to_string(node->mark.line) + ":" + std::to_string(node->mark.column);
  }
  return out;
}

namespace {

[[noreturn]] void fail(std::string_view file, const yaml::Node* node, const std::string& message) {
  throw EvaluationError(location(file, node) + ": " + message);
}

}  // namespace

expr::Value evaluate_token(const yaml::Node* node, const expr::EvalContext& context, const expr::Scope& scope,
                           std::string_view file) {
  if (node == nullptr) return {};
  try {
    switch (node->kind) {
      case yaml::NodeKind::String:
        return expr::evaluate_template(node->text, context, scope);
      case yaml::NodeKind::Sequence: {
        auto arr = expr::Value::array();
        for (const auto* item : node->items) arr.push(evaluate_token(item, context, scope, file));
        return arr;
      }
      case yaml::NodeKind::Mapping: {
        auto obj = expr::Value::object();
        for (const auto& e : node->entries) obj.set(e.key->text, evaluate_token(e.value, context, scope, file));
        return obj;
      }
      default:
        return yaml::to_value(*node);
    }
  } catch (const EvaluationError&) {
    throw;
  } catch (const std::exception& e) {
    fail(file, node, e.what());
  }
}

std::string scalar_to_string(const expr::Value& value, std::string_view where) {
  if (value.is_array() || value.is_object()) {
    throw EvaluationError(std::string(where) + ": " + (value.is_array() ? "A sequence" : "A mapping") +
                          " was not expected");
  }
  return value.to_string();
}

std::string evaluate_string(const yaml::Node* node, const expr::EvalContext& context, const expr::Scope& scope,
                            std::string_view file) {
  if (node == nullptr) return {};
  return scalar_to_string(evaluate_token(node, context, scope, file), location(file, node));
}

bool evaluate_condition(const yaml::Node* node, const expr::EvalContext& context, const expr::Scope& scope,
                        std::string_view file) {
  try {
    const auto text = workflow::condition_expression(node, scope);
    return expr::evaluate(expr::Expression::parse(text, scope), context).truthy();
  } catch (const std::exception& e) {
    fail(file, node, std::string("Error evaluating 'if': ") + e.what());
  }
}

std::vector<std::pair<std::string, std::string>> evaluate_string_map(const yaml::Node* node,
                                                                     const expr::EvalContext& context,
                                                                     const expr::Scope& scope, std::string_view file) {
  std::vector<std::pair<std::string, std::string>> out;
  if (node == nullptr || node->is_null()) return out;
  if (node->is_mapping()) {
    for (const auto& e : node->entries) {
      out.emplace_back(e.key->text, scalar_to_string(evaluate_token(e.value, context, scope, file),
                                                     location(file, e.value)));
    }
    return out;
  }
  const auto v = evaluate_token(node, context, scope, file);
  if (v.is_null()) return out;
  if (!v.is_object()) fail(file, node, "A mapping was expected");
  for (const auto& [k, item] : v.object_data().entries()) {
    out.emplace_back(k, scalar_to_string(item, location(file, node)));
  }
  return out;
}

}  // namespace cpa::engine
