#include "expr/expression.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>

#include "util/strings.hpp"

namespace cpa::expr {

namespace {

constexpr std::size_t kMaxLength = 21000;
constexpr int kMaxDepth = 50;

// ---------------------------------------------------------------------------
// Lexer

enum class Tok {
  StartGroup,
  StartIndex,
  StartParameters,
  EndGroup,
  EndIndex,
  EndParameters,
  Separator,
  Dereference,
  Wildcard,
  LogicalOp,
  ComparisonOp,
  Null,
  Boolean,
  Number,
  String,
  PropertyName,
  Function,
  NamedValue,
  Unexpected,
};

struct Token {
  Tok kind;
  std::string raw;
  std::size_t pos;
  Value value;
};

bool is_ws(char c) { return util::is_space(c); }

bool is_boundary(char c) {
  switch (c) {
    case '(':
    case '[':
    case ')':
    case ']':
    case ',':
    case '.':
    case '!':
    case '>':
    case '<':
    case '=':
    case '&':
    case '|': return true;
    default: return is_ws(c);
  }
}

bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_legal_keyword(std::string_view s) {
  if (s.empty()) return false;
  if (!is_letter(s[0]) && s[0] != '_') return false;
  for (char c : s.substr(1)) {
    if (!is_letter(c) && !is_digit(c) && c != '_' && c != '-') return false;
  }
  return true;
}

class Lexer {
 public:
  explicit Lexer(std::string_view text) : text_(text) {}

  std::vector<Token> tokenize() {
    std::vector<Token> out;
    // Track open parentheses to distinguish EndGroup from EndParameters.
    std::vector<bool> paren_is_call;
    while (true) {
      while (i_ < text_.size() && is_ws(text_[i_])) ++i_;
      if (i_ >= text_.size()) break;
      Token t = next(paren_is_call);
      last_ = t.kind;
      has_last_ = true;
      out.push_back(std::move(t));
    }
    return out;
  }

 private:
  Token make(Tok kind, std::size_t start, std::size_t len, Value v = Value()) {
    return Token{kind, std::string(text_.substr(start, len)), start, std::move(v)};
  }

  Token next(std::vector<bool>& parens) {
    const char c = text_[i_];
    const std::size_t start = i_;
    switch (c) {
      case '(':
        ++i_;
        if (has_last_ && last_ == Tok::Function) {
          parens.push_back(true);
          return make(Tok::StartParameters, start, 1);
        }
        parens.push_back(false);
        return make(Tok::StartGroup, start, 1);
      case ')': {
        ++i_;
        bool call = false;
        if (!parens.empty()) {
          call = parens.back();
          parens.pop_back();
        }
        return make(call ? Tok::EndParameters : Tok::EndGroup, start, 1);
      }
      case '[': ++i_; return make(Tok::StartIndex, start, 1);
      case ']': ++i_; return make(Tok::EndIndex, start, 1);
      case ',': ++i_; return make(Tok::Separator, start, 1);
      case '*': ++i_; return make(Tok::Wildcard, start, 1);
      case '\'': return read_string();
      case '!':
        if (peek(1) == '=') {
          i_ += 2;
          return make(Tok::ComparisonOp, start, 2);
        }
        ++i_;
        return make(Tok::LogicalOp, start, 1);
      case '>':
      case '<':
        if (peek(1) == '=') {
          i_ += 2;
          return make(Tok::ComparisonOp, start, 2);
        }
        ++i_;
        return make(Tok::ComparisonOp, start, 1);
      case '=':
        if (peek(1) == '=') {
          i_ += 2;
          return make(Tok::ComparisonOp, start, 2);
        }
        ++i_;
        return make(Tok::Unexpected, start, 1);
      case '&':
        if (peek(1) == '&') {
          i_ += 2;
          return make(Tok::LogicalOp, start, 2);
        }
        ++i_;
        return make(Tok::Unexpected, start, 1);
      case '|':
        if (peek(1) == '|') {
          i_ += 2;
          return make(Tok::LogicalOp, start, 2);
        }
        ++i_;
        return make(Tok::Unexpected, start, 1);
      case '.':
        if (!has_last_ || last_ == Tok::Separator || last_ == Tok::StartGroup || last_ == Tok::StartIndex ||
            last_ == Tok::StartParameters || last_ == Tok::LogicalOp) {
          return read_number();
        }
        ++i_;
        return make(Tok::Dereference, start, 1);
      case '-':
      case '+': return read_number();
      default:
        if (is_digit(c)) return read_number();
        return read_keyword();
    }
  }

  char peek(std::size_t offset) const { return i_ + offset < text_.size() ? text_[i_ + offset] : '\0'; }

  Token read_number() {
    const std::size_t start = i_;
    do {
      ++i_;
    } while (i_ < text_.size() && (!is_boundary(text_[i_]) || text_[i_] == '.'));
    const auto raw = text_.substr(start, i_ - start);
    const double d = util::parse_number(raw);
    if (std::isnan(d)) return make(Tok::Unexpected, start, raw.size());
    return make(Tok::Number, start, raw.size(), Value(d));
  }

  Token read_string() {
    const std::size_t start = i_;
    std::string value;
    bool closed = false;
    ++i_;
    while (i_ < text_.size()) {
      const char c = text_[i_++];
      if (c == '\'') {
        if (i_ >= text_.size() || text_[i_] != '\'') {
          closed = true;
          break;
        }
        ++i_;
      }
      value.push_back(c);
    }
    if (!closed) return make(Tok::Unexpected, start, i_ - start);
    return make(Tok::String, start, i_ - start, Value(std::move(value)));
  }

  Token read_keyword() {
    const std::size_t start = i_;
    ++i_;
    while (i_ < text_.size() && !is_boundary(text_[i_])) ++i_;
    const auto str = text_.substr(start, i_ - start);
    if (!is_legal_keyword(str)) return make(Tok::Unexpected, start, str.size());
    if (has_last_ && last_ == Tok::Dereference) return make(Tok::PropertyName, start, str.size());
    if (str == "null") return make(Tok::Null, start, str.size());
    if (str == "true") return make(Tok::Boolean, start, str.size(), Value(true));
    if (str == "false") return make(Tok::Boolean, start, str.size(), Value(false));
    if (str == "NaN") return make(Tok::Number, start, str.size(), Value(std::nan("")));
    if (str == "Infinity") return make(Tok::Number, start, str.size(), Value(HUGE_VAL));
    std::size_t j = i_;
    while (j < text_.size() && is_ws(text_[j])) ++j;
    if (j < text_.size() && text_[j] == '(') return make(Tok::Function, start, str.size());
    return make(Tok::NamedValue, start, str.size());
  }

  std::string_view text_;
  std::size_t i_ = 0;
  Tok last_ = Tok::Unexpected;
  bool has_last_ = false;
};

// ---------------------------------------------------------------------------
// Parser

struct FunctionInfo {
  const char* name;
  std::size_t min_args;
  std::size_t max_args;
  enum { Builtin, Status, HashFiles } kind;
};

constexpr FunctionInfo kFunctions[] = {
    {"contains", 2, 2, FunctionInfo::Builtin},    {"startsWith", 2, 2, FunctionInfo::Builtin},
    {"endsWith", 2, 2, FunctionInfo::Builtin},    {"format", 1, 255, FunctionInfo::Builtin},
    {"join", 1, 2, FunctionInfo::Builtin},        {"toJSON", 1, 1, FunctionInfo::Builtin},
    {"fromJSON", 1, 1, FunctionInfo::Builtin},    {"hashFiles", 1, 255, FunctionInfo::HashFiles},
    {"success", 0, 0, FunctionInfo::Status},      {"always", 0, 0, FunctionInfo::Status},
    {"cancelled", 0, 0, FunctionInfo::Status},    {"failure", 0, 0, FunctionInfo::Status},
};

const FunctionInfo* find_function(std::string_view name) {
  for (const auto& f : kFunctions) {
    if (util::iequals(f.name, name)) return &f;
  }
  return nullptr;
}

class Parser {
 public:
  Parser(std::string_view text, std::vector<Token> tokens, const Scope& scope)
      : text_(text), toks_(std::move(tokens)), scope_(scope) {}

  std::unique_ptr<Node> parse() {
    if (toks_.empty()) throw ParseError("Unexpected end of expression. An expression was expected.");
    auto node = parse_or(0);
    if (i_ < toks_.size()) unexpected(toks_[i_]);
    return node;
  }

 private:
  [[noreturn]] void fail(const std::string& what, const Token& t) const {
    throw ParseError(what + ": '" + t.raw + "'. Located at position " + std::to_string(t.pos + 1) +
                     " within expression: " + std::string(text_));
  }
  [[noreturn]] void unexpected(const Token& t) const { fail("Unexpected symbol", t); }
  [[noreturn]] void unexpected_end() const {
    const auto& last = toks_.back();
    throw ParseError("Unexpected end of expression: '" + last.raw + "'. Located at position " +
                     std::to_string(last.pos + 1) + " within expression: " + std::string(text_));
  }

  const Token* peek() const { return i_ < toks_.size() ? &toks_[i_] : nullptr; }
  const Token& take() {
    if (i_ >= toks_.size()) unexpected_end();
    return toks_[i_++];
  }
  bool peek_op(Tok kind, std::string_view raw) const {
    const auto* t = peek();
    return t && t->kind == kind && t->raw == raw;
  }
  void check_depth(int depth, const Token& t) const {
    if (depth > kMaxDepth) {
      throw ParseError("Exceeded max expression depth " + std::to_string(kMaxDepth) + ". Located at position " +
                       std::to_string(t.pos + 1) + " within expression: " + std::string(text_));
    }
  }

  static std::unique_ptr<Node> make(NodeType type) {
    auto n = std::make_unique<Node>();
    n->type = type;
    return n;
  }

  std::unique_ptr<Node> parse_or(int depth) {
    auto left = parse_and(depth);
    if (!peek_op(Tok::LogicalOp, "||")) return left;
    auto node = make(NodeType::Or);
    node->args.push_back(std::move(left));
    while (peek_op(Tok::LogicalOp, "||")) {
      ++i_;
      node->args.push_back(parse_and(depth + 1));
    }
    return node;
  }

  std::unique_ptr<Node> parse_and(int depth) {
    auto left = parse_equality(depth);
    if (!peek_op(Tok::LogicalOp, "&&")) return left;
    auto node = make(NodeType::And);
    node->args.push_back(std::move(left));
    while (peek_op(Tok::LogicalOp, "&&")) {
      ++i_;
      node->args.push_back(parse_equality(depth + 1));
    }
    return node;
  }

  std::unique_ptr<Node> parse_equality(int depth) {
    auto left = parse_comparison(depth);
    while (peek_op(Tok::ComparisonOp, "==") || peek_op(Tok::ComparisonOp, "!=")) {
      const auto& op = take();
      auto node = make(op.raw == "==" ? NodeType::Equal : NodeType::NotEqual);
      node->args.push_back(std::move(left));
      node->args.push_back(parse_comparison(depth + 1));
      left = std::move(node);
    }
    return left;
  }

  std::unique_ptr<Node> parse_comparison(int depth) {
    auto left = parse_unary(depth);
    while (true) {
      const auto* t = peek();
      if (!t || t->kind != Tok::ComparisonOp || t->raw == "==" || t->raw == "!=") break;
      const auto& op = take();
      NodeType type = NodeType::Less;
      if (op.raw == "<") type = NodeType::Less;
      else if (op.raw == "<=") type = NodeType::LessEqual;
      else if (op.raw == ">") type = NodeType::Greater;
      else type = NodeType::GreaterEqual;
      auto node = make(type);
      node->args.push_back(std::move(left));
      node->args.push_back(parse_unary(depth + 1));
      left = std::move(node);
    }
    return left;
  }

  std::unique_ptr<Node> parse_unary(int depth) {
    const auto* t = peek();
    if (!t) unexpected_end();
    check_depth(depth, *t);
    if (t->kind == Tok::LogicalOp && t->raw == "!") {
      ++i_;
      auto node = make(NodeType::Not);
      node->args.push_back(parse_unary(depth + 1));
      return node;
    }
    return parse_postfix(depth);
  }

  std::unique_ptr<Node> parse_postfix(int depth) {
    auto node = parse_primary(depth);
    while (const auto* t = peek()) {
      if (t->kind == Tok::Dereference) {
        ++i_;
        const auto& name = take();
        auto index = make(NodeType::Index);
        index->args.push_back(std::move(node));
        if (name.kind == Tok::PropertyName) {
          auto lit = make(NodeType::Literal);
          lit->literal = Value(name.raw);
          index->args.push_back(std::move(lit));
        } else if (name.kind == Tok::Wildcard) {
          index->args.push_back(make(NodeType::Wildcard));
        } else {
          unexpected(name);
        }
        node = std::move(index);
        check_depth(++depth, name);
      } else if (t->kind == Tok::StartIndex) {
        ++i_;
        auto index = make(NodeType::Index);
        index->args.push_back(std::move(node));
        const auto* inner = peek();
        if (!inner) unexpected_end();
        if (inner->kind == Tok::Wildcard) {
          ++i_;
          index->args.push_back(make(NodeType::Wildcard));
        } else {
          index->args.push_back(parse_or(depth + 1));
        }
        const auto& close = take();
        if (close.kind != Tok::EndIndex) unexpected(close);
        node = std::move(index);
        check_depth(++depth, close);
      } else {
        break;
      }
    }
    return node;
  }

  std::unique_ptr<Node> parse_primary(int depth) {
    const auto& t = take();
    switch (t.kind) {
      case Tok::Null:
      case Tok::Boolean:
      case Tok::Number:
      case Tok::String: {
        auto n = make(NodeType::Literal);
        n->literal = t.value;
        return n;
      }
      case Tok::NamedValue: {
        if (!scope_.any_named_value) {
          const bool ok = std::any_of(scope_.named_values.begin(), scope_.named_values.end(),
                                      [&](const std::string& nv) { return util::iequals(nv, t.raw); });
          if (!ok) fail("Unrecognized named-value", t);
        }
        auto n = make(NodeType::NamedValue);
        n->name = t.raw;
        return n;
      }
      case Tok::Function: return parse_call(t, depth);
      case Tok::StartGroup: {
        auto inner = parse_or(depth + 1);
        const auto& close = take();
        if (close.kind != Tok::EndGroup) unexpected(close);
        return inner;
      }
      default: unexpected(t);
    }
  }

  std::unique_ptr<Node> parse_call(const Token& name, int depth) {
    const auto* info = find_function(name.raw);
    if (!info || (info->kind == FunctionInfo::Status && !scope_.status_functions) ||
        (info->kind == FunctionInfo::HashFiles && !scope_.hash_files)) {
      fail("Unrecognized function", name);
    }
    const auto& open = take();
    if (open.kind != Tok::StartParameters) unexpected(open);
    auto n = make(NodeType::Function);
    n->name = info->name;
    if (const auto* t = peek(); t && t->kind == Tok::EndParameters) {
      ++i_;
    } else {
      while (true) {
        n->args.push_back(parse_or(depth + 1));
        const auto& sep = take();
        if (sep.kind == Tok::EndParameters) break;
        if (sep.kind != Tok::Separator) unexpected(sep);
      }
    }
    if (n->args.size() < info->min_args) fail("Too few parameters supplied", name);
    if (n->args.size() > info->max_args) fail("Too many parameters supplied", name);
    return n;
  }

  std::string_view text_;
  std::vector<Token> toks_;
  const Scope& scope_;
  std::size_t i_ = 0;
};

// ---------------------------------------------------------------------------
// Evaluator

Value filtered_array() {
  auto v = Value::array();
  v.array_data().filtered = true;
  return v;
}

Value eval(const Node& n, const EvalContext& ctx);

Value eval_index(const Node& n, const EvalContext& ctx) {
  const Value left = eval(*n.args[0], ctx);
  const Node& idx = *n.args[1];
  const bool wildcard = idx.type == NodeType::Wildcard;
  if (!left.is_array() && !left.is_object()) return wildcard ? filtered_array() : Value();

  Value index_value;
  if (!wildcard) index_value = eval(idx, ctx);
  auto int_index = [&]() -> std::optional<std::size_t> {
    double d = index_value.to_number();
    if (std::isnan(d) || d < 0) return std::nullopt;
    d = std::floor(d);
    if (d > static_cast<double>(INT_MAX)) return std::nullopt;
    return static_cast<std::size_t>(d);
  };
  auto str_index = [&]() -> std::optional<std::string> {
    if (!index_value.is_primitive()) return std::nullopt;
    return index_value.to_string();
  };

  if (left.is_array() && left.array_data().filtered) {
    auto result = filtered_array();
    const auto si = wildcard ? std::nullopt : str_index();
    const auto ii = wildcard ? std::nullopt : int_index();
    for (const auto& item : left.items()) {
      if (item.is_object()) {
        if (wildcard) {
          for (const auto& [k, v] : item.object_data().entries()) result.push(v);
        } else if (si) {
          if (const auto* v = item.object_data().find(*si)) result.push(*v);
        }
      } else if (item.is_array()) {
        if (wildcard) {
          for (const auto& v : item.items()) result.push(v);
        } else if (ii && *ii < item.items().size()) {
          result.push(item.items()[*ii]);
        }
      }
    }
    return result;
  }
  if (left.is_object()) {
    if (wildcard) {
      auto result = filtered_array();
      for (const auto& [k, v] : left.object_data().entries()) result.push(v);
      return result;
    }
    if (const auto si = str_index()) {
      if (const auto* v = left.object_data().find(*si)) return *v;
    }
    return Value();
  }
  if (wildcard) {
    auto result = filtered_array();
    for (const auto& v : left.items()) result.push(v);
    return result;
  }
  if (const auto ii = int_index(); ii && *ii < left.items().size()) return left.items()[*ii];
  return Value();
}

std::string format_string(const Node& n, const EvalContext& ctx) {
  const std::string format = eval(*n.args[0], ctx).to_string();
  std::vector<std::optional<std::string>> cache(n.args.size());
  auto arg = [&](std::size_t i) -> const std::string& {
    if (!cache[i]) cache[i] = eval(*n.args[i], ctx).to_string();
    return *cache[i];
  };
  auto invalid = [&]() { return EvalError("The following format string is invalid: " + format); };
  std::string out;
  std::size_t index = 0;
  while (index < format.size()) {
    const auto lbrace = format.find('{', index);
    const auto rbrace = format.find('}', index);
    if (lbrace != std::string::npos && (rbrace == std::string::npos || rbrace > lbrace)) {
      if (lbrace + 1 < format.size() && format[lbrace + 1] == '{') {
        out.append(format, index, lbrace - index + 1);
        index = lbrace + 2;
        continue;
      }
      // {N} or {N:specifiers}
      std::size_t j = lbrace + 1;
      while (j < format.size() && is_digit(format[j])) ++j;
      if (j == lbrace + 1 || j - lbrace - 1 > 3) throw invalid();
      const int arg_index = std::stoi(format.substr(lbrace + 1, j - lbrace - 1));
      if (arg_index > 255) throw invalid();
      std::string specifiers;
      std::size_t close = std::string::npos;
      if (j < format.size() && format[j] == '}') {
        close = j;
      } else if (j < format.size() && format[j] == ':') {
        std::size_t k = j + 1;
        while (true) {
          if (k >= format.size()) throw invalid();
          if (format[k] != '}') {
            specifiers.push_back(format[k++]);
          } else if (k + 1 < format.size() && format[k + 1] == '}') {
            specifiers.push_back('}');
            k += 2;
          } else {
            close = k;
            break;
          }
        }
      } else {
        throw invalid();
      }
      if (static_cast<std::size_t>(arg_index) + 1 >= n.args.size()) {
        throw EvalError("The following format string references more arguments than were supplied: " + format);
      }
      if (!specifiers.empty()) {
        throw EvalError("The format specifiers '" + specifiers + "' are not valid for objects of type '" +
                        std::string(kind_name(eval(*n.args[static_cast<std::size_t>(arg_index) + 1], ctx).kind())) +
                        "'");
      }
      out.append(format, index, lbrace - index);
      out += arg(static_cast<std::size_t>(arg_index) + 1);
      index = close + 1;
    } else if (rbrace != std::string::npos) {
      if (rbrace + 1 < format.size() && format[rbrace + 1] == '}') {
        out.append(format, index, rbrace - index + 1);
        index = rbrace + 2;
      } else {
        throw invalid();
      }
    } else {
      out.append(format, index, std::string::npos);
      break;
    }
  }
  return out;
}

Value call_function(const Node& n, const EvalContext& ctx) {
  const std::string name = util::to_lower(n.name);
  auto arg = [&](std::size_t i) { return eval(*n.args[i], ctx); };

  if (name == "contains") {
    const Value left = arg(0);
    if (left.is_primitive()) {
      const Value right = arg(1);
      if (right.is_primitive()) {
        return Value(util::ifind(left.to_string(), right.to_string()) != std::string_view::npos);
      }
    } else if (left.is_array() && !left.items().empty()) {
      const Value right = arg(1);
      for (const auto& item : left.items()) {
        if (loose_equals(right, item)) return Value(true);
      }
    }
    return Value(false);
  }
  if (name == "startswith" || name == "endswith") {
    const Value left = arg(0);
    if (!left.is_primitive()) return Value(false);
    const Value right = arg(1);
    if (!right.is_primitive()) return Value(false);
    const auto l = left.to_string();
    const auto r = right.to_string();
    return Value(name == "startswith" ? util::istarts_with(l, r) : util::iends_with(l, r));
  }
  if (name == "format") return Value(format_string(n, ctx));
  if (name == "join") {
    const Value items = arg(0);
    if (items.is_array() && !items.items().empty()) {
      std::string sep = ",";
      if (items.items().size() > 1 && n.args.size() > 1) {
        const Value s = arg(1);
        if (s.is_primitive()) sep = s.to_string();
      }
      std::string out;
      for (std::size_t i = 0; i < items.items().size(); ++i) {
        if (i) out += sep;
        out += items.items()[i].to_string();
      }
      return Value(std::move(out));
    }
    if (items.is_primitive()) return Value(items.to_string());
    return Value(std::string());
  }
  if (name == "tojson") return Value(arg(0).to_json_string(true));
  if (name == "fromjson") {
    const auto text = arg(0).to_string();
    try {
      return Value::parse_json(text);
    } catch (const std::exception& e) {
      throw EvalError(e.what());
    }
  }
  if (name == "hashfiles") {
    std::vector<std::string> patterns;
    bool follow = false;
    for (std::size_t i = 0; i < n.args.size(); ++i) {
      auto s = arg(i).to_string();
      if (i == 0 && s == "--follow-symbolic-links") {
        follow = true;
        continue;
      }
      patterns.push_back(std::move(s));
    }
    if (!ctx.hash_files) throw EvalError("hashFiles() is not available in this context");
    return Value(ctx.hash_files(patterns, follow));
  }
  if (name == "success" || name == "failure" || name == "cancelled" || name == "always") {
    if (ctx.status) return Value(ctx.status(name));
    return Value(name == "success" || name == "always");
  }
  throw EvalError("Unknown function: " + n.name);
}

Value eval(const Node& n, const EvalContext& ctx) {
  switch (n.type) {
    case NodeType::Literal: return n.literal;
    case NodeType::NamedValue: return ctx.contexts.get(n.name);
    case NodeType::Wildcard: return Value();
    case NodeType::Index: return eval_index(n, ctx);
    case NodeType::Not: return Value(!eval(*n.args[0], ctx).truthy());
    case NodeType::And: {
      Value v;
      for (const auto& a : n.args) {
        v = eval(*a, ctx);
        if (!v.truthy()) return v;
      }
      return v;
    }
    case NodeType::Or: {
      Value v;
      for (const auto& a : n.args) {
        v = eval(*a, ctx);
        if (v.truthy()) return v;
      }
      return v;
    }
    case NodeType::Equal: return Value(loose_equals(eval(*n.args[0], ctx), eval(*n.args[1], ctx)));
    case NodeType::NotEqual: return Value(!loose_equals(eval(*n.args[0], ctx), eval(*n.args[1], ctx)));
    case NodeType::Less: return Value(loose_less(eval(*n.args[0], ctx), eval(*n.args[1], ctx)));
    case NodeType::LessEqual: {
      const auto l = eval(*n.args[0], ctx);
      const auto r = eval(*n.args[1], ctx);
      return Value(loose_less(l, r) || loose_equals(l, r));
    }
    case NodeType::Greater: return Value(loose_greater(eval(*n.args[0], ctx), eval(*n.args[1], ctx)));
    case NodeType::GreaterEqual: {
      const auto l = eval(*n.args[0], ctx);
      const auto r = eval(*n.args[1], ctx);
      return Value(loose_greater(l, r) || loose_equals(l, r));
    }
    case NodeType::Function: return call_function(n, ctx);
  }
  return Value();
}

void traverse(const Node& n, const std::function<void(const Node&)>& fn) {
  fn(n);
  for (const auto& a : n.args) traverse(*a, fn);
}

}  // namespace

Expression Expression::parse(std::string_view text, const Scope& scope) {
  const auto trimmed = util::trim(text);
  if (trimmed.size() > kMaxLength) {
    throw ParseError("Exceeded max expression length " + std::to_string(kMaxLength));
  }
  Lexer lexer(trimmed);
  auto tokens = lexer.tokenize();
  Parser parser(trimmed, std::move(tokens), scope);
  Expression e;
  e.root_ = parser.parse();
  e.text_ = std::string(trimmed);
  return e;
}

bool Expression::has_status_function() const {
  bool found = false;
  traverse(*root_, [&](const Node& n) {
    if (n.type == NodeType::Function &&
        (util::iequals(n.name, "success") || util::iequals(n.name, "failure") || util::iequals(n.name, "cancelled") ||
         util::iequals(n.name, "always"))) {
      found = true;
    }
  });
  return found;
}

std::vector<std::string> Expression::referenced_named_values() const {
  std::vector<std::string> names;
  traverse(*root_, [&](const Node& n) {
    if (n.type == NodeType::NamedValue) {
      auto lower = util::to_lower(n.name);
      if (std::find(names.begin(), names.end(), lower) == names.end()) names.push_back(std::move(lower));
    }
  });
  return names;
}

Value evaluate(const Expression& expression, const EvalContext& context) { return eval(expression.root(), context); }

Value evaluate(std::string_view text, const EvalContext& context, const Scope& scope) {
  return evaluate(Expression::parse(text, scope), context);
}

std::string escape_string_literal(std::string_view s) { return util::replace_all(s, "'", "''"); }

}  // namespace cpa::expr
