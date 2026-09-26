#include "iutest.hpp"

#include "expr/expression.hpp"

using namespace cpa::expr;

namespace {

Scope full_scope() {
  Scope s;
  s.named_values = {"github", "env", "matrix", "steps", "needs", "inputs", "vars", "job", "runner", "strategy"};
  s.status_functions = true;
  s.hash_files = true;
  return s;
}

EvalContext make_context() {
  EvalContext ctx;
  ctx.contexts.set("github", Value::parse_json(R"({
    "event_name": "push",
    "ref": "refs/heads/main",
    "event": {
      "commits": [
        {"message": "first", "author": {"name": "alice"}},
        {"message": "second", "author": {"name": "bob"}}
      ],
      "labels": [{"name": "bug"}, {"name": "docs"}]
    }
  })"));
  auto env = Value::object(true);
  env.set("GREETING", Value("hello"));
  ctx.contexts.set("env", env);
  ctx.contexts.set("steps", Value::parse_json(R"({"my-step": {"outputs": {"value": "42"}, "outcome": "success"}})"));
  ctx.contexts.set("matrix", Value::parse_json(R"({"os": "macos-latest", "version": 3.1})"));
  return ctx;
}

std::string eval_str(std::string_view text) { return evaluate(text, make_context(), full_scope()).to_string(); }

Value eval(std::string_view text) { return evaluate(text, make_context(), full_scope()); }

}  // namespace

IUTEST(ExpressionTest, Literals) {
  IUTEST_EXPECT_EQ("1", eval_str("1"));
  IUTEST_EXPECT_EQ("it's", eval_str("'it''s'"));
  IUTEST_EXPECT_TRUE(eval("null").is_null());
  IUTEST_EXPECT_EQ("true", eval_str("true"));
  IUTEST_EXPECT_EQ("255", eval_str("0xff"));
  IUTEST_EXPECT_EQ("-1.5", eval_str("-1.5"));
  IUTEST_EXPECT_EQ("1000", eval_str("1e3"));
  IUTEST_EXPECT_EQ("0.5", eval_str(".5"));
  IUTEST_EXPECT_EQ("NaN", eval_str("NaN"));
  IUTEST_EXPECT_EQ("-Infinity", eval_str("-Infinity"));
}

IUTEST(ExpressionTest, LogicalOperatorsReturnOperands) {
  IUTEST_EXPECT_TRUE(eval("null && 'x'").is_null());
  IUTEST_EXPECT_EQ("x", eval_str("'a' && 'x'"));
  IUTEST_EXPECT_EQ("default", eval_str("'' || 'default'"));
  IUTEST_EXPECT_EQ("0", eval_str("false || 0"));
  IUTEST_EXPECT_EQ("true", eval_str("!''"));
  IUTEST_EXPECT_EQ("false", eval_str("!'x'"));
}

IUTEST(ExpressionTest, Precedence) {
  IUTEST_EXPECT_EQ("true", eval_str("true || false && false"));
  IUTEST_EXPECT_EQ("true", eval_str("!true == false"));
  IUTEST_EXPECT_EQ("true", eval_str("1 < 2 && 3 >= 3"));
  IUTEST_EXPECT_EQ("false", eval_str("!(1 < 2)"));
  IUTEST_EXPECT_EQ("true", eval_str("1 == 1 == true"));
}

IUTEST(ExpressionTest, Comparison) {
  IUTEST_EXPECT_EQ("true", eval_str("1 == 1.0"));
  IUTEST_EXPECT_EQ("true", eval_str("'abc' == 'ABC'"));
  IUTEST_EXPECT_EQ("true", eval_str("1 == '1'"));
  IUTEST_EXPECT_EQ("true", eval_str("'' == 0"));
  IUTEST_EXPECT_EQ("true", eval_str("null == false"));
  IUTEST_EXPECT_EQ("false", eval_str("NaN == NaN"));
  IUTEST_EXPECT_EQ("true", eval_str("NaN != NaN"));
  IUTEST_EXPECT_EQ("false", eval_str("fromJSON('{}') == fromJSON('{}')"));
  IUTEST_EXPECT_EQ("true", eval_str("'b' > 'A'"));
  IUTEST_EXPECT_EQ("true", eval_str("matrix.version == 3.1"));
  IUTEST_EXPECT_EQ("true", eval_str("matrix.version == '3.10'"));
}

IUTEST(ExpressionTest, PropertyAccess) {
  IUTEST_EXPECT_EQ("push", eval_str("github.event_name"));
  IUTEST_EXPECT_EQ("push", eval_str("GITHUB.EVENT_NAME"));
  IUTEST_EXPECT_EQ("push", eval_str("github['event_name']"));
  IUTEST_EXPECT_EQ("second", eval_str("github.event.commits[1].message"));
  IUTEST_EXPECT_EQ("42", eval_str("steps.my-step.outputs.value"));
  IUTEST_EXPECT_TRUE(eval("github.nothing.deeper").is_null());
  IUTEST_EXPECT_EQ("hello", eval_str("env.GREETING"));
  IUTEST_EXPECT_TRUE(eval("env.greeting").is_null());  // env is case-sensitive
  IUTEST_EXPECT_EQ("20", eval_str("fromJSON('[10,20]')[1]"));
  IUTEST_EXPECT_EQ("20", eval_str("fromJSON('[10,20]')['1']"));
  IUTEST_EXPECT_EQ("10", eval_str("fromJSON('[10,20]')[0.9]"));
  IUTEST_EXPECT_TRUE(eval("fromJSON('[10,20]')[5]").is_null());
  IUTEST_EXPECT_EQ("1", eval_str("(fromJSON('{\"a\":1}')).a"));
}

IUTEST(ExpressionTest, Wildcards) {
  IUTEST_EXPECT_EQ("alice,bob", eval_str("join(github.event.commits.*.author.name)"));
  IUTEST_EXPECT_EQ("first|second", eval_str("join(github.event.commits[*].message, '|')"));
  IUTEST_EXPECT_EQ("true", eval_str("contains(github.event.labels.*.name, 'BUG')"));
  IUTEST_EXPECT_EQ("false", eval_str("contains(github.event.labels.*.name, 'feature')"));
  IUTEST_EXPECT_EQ("[]", eval_str("toJSON(github.event_name.*)"));
  IUTEST_EXPECT_EQ("[\n  \"macos-latest\",\n  3.1\n]", eval_str("toJSON(matrix.*)"));
}

IUTEST(ExpressionTest, Functions) {
  IUTEST_EXPECT_EQ("true", eval_str("contains('Hello World', 'WORLD')"));
  IUTEST_EXPECT_EQ("true", eval_str("contains(fromJSON('[1,2]'), '1')"));
  IUTEST_EXPECT_EQ("false", eval_str("contains(fromJSON('{\"a\":1}'), 'a')"));
  IUTEST_EXPECT_EQ("true", eval_str("startsWith(github.ref, 'REFS/heads/')"));
  IUTEST_EXPECT_EQ("true", eval_str("endsWith('file.TXT', '.txt')"));
  IUTEST_EXPECT_EQ("a {1} b", eval_str("format('{0} {{1}} {1}', 'a', 'b')"));
  IUTEST_EXPECT_EQ("x}y", eval_str("format('{0}}}y', 'x')"));
  IUTEST_EXPECT_EQ("a-b", eval_str("join(fromJSON('[\"a\",\"b\"]'), '-')"));
  IUTEST_EXPECT_EQ("abc", eval_str("join('abc')"));
  IUTEST_EXPECT_EQ("{\n  \"a\": 1\n}", eval_str("toJSON(fromJSON('{\"a\":1}'))"));
  IUTEST_EXPECT_EQ("\"str\"", eval_str("toJSON('str')"));
  IUTEST_EXPECT_EQ("3.1", eval_str("fromJSON('3.10')"));
  IUTEST_EXPECT_EQ("true", eval_str("toJson(true)"));  // function names are case-insensitive
  IUTEST_EXPECT_EQ("Object", eval_str("format('{0}', github)"));
}

IUTEST(ExpressionTest, FormatErrors) {
  IUTEST_EXPECT_THROW(eval("format('{0', 1)"), EvalError);
  IUTEST_EXPECT_THROW(eval("format('{2}', 1)"), EvalError);
  IUTEST_EXPECT_THROW(eval("format('}', 1)"), EvalError);
  IUTEST_EXPECT_THROW(eval("format('{a}', 1)"), EvalError);
  IUTEST_EXPECT_THROW(eval("fromJSON('{')"), EvalError);
}

IUTEST(ExpressionTest, StatusFunctions) {
  auto ctx = make_context();
  ctx.status = [](std::string_view name) { return name == "failure" || name == "always"; };
  IUTEST_EXPECT_EQ("true", evaluate("failure()", ctx, full_scope()).to_string());
  IUTEST_EXPECT_EQ("false", evaluate("success()", ctx, full_scope()).to_string());

  IUTEST_EXPECT_TRUE(Expression::parse("always() || x", Scope{{}, true, false, true}).has_status_function());
  IUTEST_EXPECT_TRUE(Expression::parse("!Cancelled()", full_scope()).has_status_function());
  IUTEST_EXPECT_FALSE(Expression::parse("github.event_name == 'push'", full_scope()).has_status_function());
}

IUTEST(ExpressionTest, HashFilesCallback) {
  auto ctx = make_context();
  std::vector<std::string> seen;
  bool follow = false;
  ctx.hash_files = [&](const std::vector<std::string>& patterns, bool f) {
    seen = patterns;
    follow = f;
    return std::string("abc");
  };
  IUTEST_EXPECT_EQ("abc", evaluate("hashFiles('--follow-symbolic-links', '**/*.lock', 'x')", ctx, full_scope())
                              .to_string());
  IUTEST_ASSERT_EQ(2u, seen.size());
  IUTEST_EXPECT_EQ("**/*.lock", seen[0]);
  IUTEST_EXPECT_TRUE(follow);
}

IUTEST(ExpressionTest, ParseErrors) {
  const auto scope = full_scope();
  const char* bad[] = {"1 +", "a ==", "'unterminated", "github.", "(1", "github github", "1 = 1", "a & b",
                       "contains('a')", "contains('a', 'b', 'c')", "foo()", "github.*.x[", "[1]", ""};
  for (const char* text : bad) {
    IUTEST_SCOPED_TRACE(text);
    IUTEST_EXPECT_THROW(Expression::parse(text, scope), ParseError);
  }
}

IUTEST(ExpressionTest, ParseErrorMessages) {
  Scope scope;
  scope.named_values = {"github"};
  try {
    Expression::parse("secrets.TOKEN", scope);
    IUTEST_FAIL() << "expected ParseError";
  } catch (const ParseError& e) {
    IUTEST_EXPECT_STREQ(
        "Unrecognized named-value: 'secrets'. Located at position 1 within expression: secrets.TOKEN", e.what());
  }
  IUTEST_EXPECT_THROW(Expression::parse("success()", scope), ParseError);
  IUTEST_EXPECT_THROW(Expression::parse("hashFiles('x')", scope), ParseError);
}

IUTEST(ExpressionTest, MaxDepth) {
  std::string deep;
  for (int i = 0; i < 60; ++i) deep += "(";
  deep += "1";
  for (int i = 0; i < 60; ++i) deep += ")";
  IUTEST_EXPECT_THROW(Expression::parse(deep, full_scope()), ParseError);
  std::string ok = "((((1))))";
  IUTEST_EXPECT_NO_THROW(Expression::parse(ok, full_scope()));
}

IUTEST(ExpressionTest, ReferencedNamedValues) {
  const auto e = Expression::parse("github.x && Env.Y || github.z", full_scope());
  const auto names = e.referenced_named_values();
  IUTEST_ASSERT_EQ(2u, names.size());
  IUTEST_EXPECT_EQ("github", names[0]);
  IUTEST_EXPECT_EQ("env", names[1]);
}
