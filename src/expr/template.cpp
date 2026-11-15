#include "expr/template.hpp"

#include "util/strings.hpp"

namespace cpa::expr {

bool contains_expression(std::string_view raw) { return raw.find("${{") != std::string_view::npos; }

std::vector<TemplateSegment> split_template(std::string_view raw) {
  std::vector<TemplateSegment> segments;
  auto start_expr = raw.find("${{");
  if (start_expr == std::string_view::npos) {
    segments.push_back({false, std::string(raw)});
    return segments;
  }
  std::size_t i = 0;
  while (i < raw.size()) {
    if (i == start_expr) {
      std::size_t end_expr = std::string_view::npos;
      bool in_string = false;
      for (i += 3; i < raw.size(); ++i) {
        if (raw[i] == '\'') {
          in_string = !in_string;
        } else if (!in_string && raw[i] == '}' && raw[i - 1] == '}') {
          end_expr = i;
          ++i;
          break;
        }
      }
      if (end_expr == std::string_view::npos) {
        throw ParseError("An expression was not closed: " + std::string(raw.substr(start_expr)));
      }
      const auto inner = raw.substr(start_expr + 3, end_expr - start_expr - 4);
      segments.push_back({true, std::string(util::trim(inner))});
      start_expr = raw.find("${{", i);
    } else if (start_expr != std::string_view::npos && i < start_expr) {
      segments.push_back({false, std::string(raw.substr(i, start_expr - i))});
      i = start_expr;
    } else {
      segments.push_back({false, std::string(raw.substr(i))});
      break;
    }
  }
  return segments;
}

std::string template_to_expression(const std::vector<TemplateSegment>& segments) {
  if (segments.size() == 1) {
    if (segments[0].is_expression) return segments[0].text;
    return "'" + escape_string_literal(segments[0].text) + "'";
  }
  std::string format;
  std::string args;
  int arg_index = 0;
  for (const auto& s : segments) {
    if (!s.is_expression) {
      auto text = escape_string_literal(s.text);
      text = util::replace_all(text, "{", "{{");
      text = util::replace_all(text, "}", "}}");
      format += text;
    } else {
      format += "{" + std::to_string(arg_index++) + "}";
      args += ", " + s.text;
    }
  }
  return "format('" + format + "'" + args + ")";
}

void validate_template(std::string_view raw, const Scope& scope) {
  if (!contains_expression(raw)) return;
  for (const auto& s : split_template(raw)) {
    if (!s.is_expression) continue;
    if (s.text.empty()) throw ParseError("An expression was expected");
    Expression::parse(s.text, scope);
  }
}

Value evaluate_template(std::string_view raw, const EvalContext& context, const Scope& scope) {
  if (!contains_expression(raw)) return Value(std::string(raw));
  const auto segments = split_template(raw);
  for (const auto& s : segments) {
    if (s.is_expression && s.text.empty()) throw ParseError("An expression was expected");
  }
  if (segments.size() == 1 && segments[0].is_expression) {
    return evaluate(Expression::parse(segments[0].text, scope), context);
  }
  std::string out;
  for (const auto& s : segments) {
    if (s.is_expression) {
      out += evaluate(Expression::parse(s.text, scope), context).to_string();
    } else {
      out += s.text;
    }
  }
  return Value(std::move(out));
}

}  // namespace cpa::expr
