#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "expr/expression.hpp"

namespace cpa::expr {

struct TemplateSegment {
  bool is_expression = false;
  std::string text;  // literal text, or expression source without ${{ }}
};

// Splits a string into literal and ${{ }} segments following the rules of
// the Actions template reader. Throws ParseError when an expression is not
// closed.
std::vector<TemplateSegment> split_template(std::string_view raw);

bool contains_expression(std::string_view raw);

// Converts a template into a single expression:
//   "${{ x }}"          -> "x"
//   "a ${{ x }} b"      -> "format('a {0} b', x)"
//   "literal"           -> "'literal'"
std::string template_to_expression(const std::vector<TemplateSegment>& segments);

// Validates all expressions of a template against a scope. Throws ParseError.
void validate_template(std::string_view raw, const Scope& scope);

// Evaluates a template. A template consisting of exactly one expression keeps
// the type of the expression result; otherwise the result is a string.
Value evaluate_template(std::string_view raw, const EvalContext& context, const Scope& scope);

}  // namespace cpa::expr
