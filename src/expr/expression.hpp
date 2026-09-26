#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "expr/value.hpp"

namespace cpa::expr {

class ParseError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class EvalError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Set of named values (contexts) and special functions available at an
// expression site. See "Context availability" in the GitHub documentation.
struct Scope {
  std::vector<std::string> named_values;
  bool status_functions = false;  // success(), failure(), cancelled(), always()
  bool hash_files = false;        // hashFiles()
  bool any_named_value = false;   // accept every named value (tooling)
};

enum class NodeType {
  Literal,
  NamedValue,
  Wildcard,
  Index,
  Not,
  And,
  Or,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Function,
};

struct Node {
  NodeType type = NodeType::Literal;
  Value literal;
  std::string name;  // named value / function name (as written)
  std::vector<std::unique_ptr<Node>> args;
};

// A parsed expression (without the surrounding "${{ }}").
class Expression {
 public:
  static Expression parse(std::string_view text, const Scope& scope);

  const Node& root() const { return *root_; }
  const std::string& text() const { return text_; }
  bool has_status_function() const;
  // Names of the contexts referenced by the expression (lower case, unique).
  std::vector<std::string> referenced_named_values() const;

 private:
  std::shared_ptr<const Node> root_;
  std::string text_;
};

struct EvalContext {
  // Object whose keys are the named values (github, env, matrix, ...).
  Value contexts = Value::object();
  // Status functions; receives the lower-case function name.
  std::function<bool(std::string_view)> status;
  // hashFiles(patterns...). Returns the hex digest or an empty string.
  std::function<std::string(const std::vector<std::string>&, bool follow_symlinks)> hash_files;
};

Value evaluate(const Expression& expression, const EvalContext& context);

// Parses and evaluates in one step (convenience for tests and tooling).
Value evaluate(std::string_view text, const EvalContext& context, const Scope& scope);

// Escapes a string for use as a single-quoted expression literal.
std::string escape_string_literal(std::string_view s);

}  // namespace cpa::expr
