#include "iutest.hpp"

#include "expr/template.hpp"

using namespace cpa::expr;

namespace {

Scope scope() {
  Scope s;
  s.named_values = {"github", "matrix"};
  return s;
}

EvalContext context() {
  EvalContext ctx;
  ctx.contexts.set("github", Value::parse_json(R"({"actor": "octocat", "run_number": 7})"));
  ctx.contexts.set("matrix", Value::parse_json(R"({"include": [1, 2], "flag": true})"));
  return ctx;
}

}  // namespace

IUTEST(TemplateTest, Split) {
  auto segs = split_template("Hello ${{ github.actor }}!");
  IUTEST_ASSERT_EQ(3u, segs.size());
  IUTEST_EXPECT_FALSE(segs[0].is_expression);
  IUTEST_EXPECT_EQ("Hello ", segs[0].text);
  IUTEST_EXPECT_TRUE(segs[1].is_expression);
  IUTEST_EXPECT_EQ("github.actor", segs[1].text);
  IUTEST_EXPECT_EQ("!", segs[2].text);

  // "}}" inside a string literal does not close the expression.
  segs = split_template("${{ '}}' }}x");
  IUTEST_ASSERT_EQ(2u, segs.size());
  IUTEST_EXPECT_EQ("'}}'", segs[0].text);

  IUTEST_EXPECT_THROW(split_template("${{ github.actor"), ParseError);
}

IUTEST(TemplateTest, ToExpression) {
  IUTEST_EXPECT_EQ("github.actor", template_to_expression(split_template("${{ github.actor }}")));
  IUTEST_EXPECT_EQ("format('a {0} b''s {{x}}', github.actor)",
                   template_to_expression(split_template("a ${{ github.actor }} b's {x}")));
  IUTEST_EXPECT_EQ("format('{0} && {1}', a, b)", template_to_expression(split_template("${{ a }} && ${{ b }}")));
  IUTEST_EXPECT_EQ("'plain'", template_to_expression(split_template("plain")));
}

IUTEST(TemplateTest, Evaluate) {
  const auto ctx = context();
  IUTEST_EXPECT_EQ("Hello octocat #7", evaluate_template("Hello ${{ github.actor }} #${{ github.run_number }}", ctx,
                                                         scope())
                                           .to_string());
  // A single expression keeps its type.
  const auto v = evaluate_template("${{ matrix.include }}", ctx, scope());
  IUTEST_EXPECT_TRUE(v.is_array());
  const auto b = evaluate_template("${{ matrix.flag }}", ctx, scope());
  IUTEST_EXPECT_TRUE(b.is_bool());
  // Embedded containers are converted with ConvertToString.
  IUTEST_EXPECT_EQ("x Array", evaluate_template("x ${{ matrix.include }}", ctx, scope()).to_string());
  IUTEST_EXPECT_EQ("no expressions", evaluate_template("no expressions", ctx, scope()).to_string());
  IUTEST_EXPECT_THROW(evaluate_template("${{ secrets.x }}", ctx, scope()), ParseError);
  IUTEST_EXPECT_THROW(evaluate_template("${{ }}", ctx, scope()), ParseError);
}

IUTEST(TemplateTest, Validate) {
  IUTEST_EXPECT_NO_THROW(validate_template("a ${{ github.actor }}", scope()));
  IUTEST_EXPECT_THROW(validate_template("a ${{ env.x }}", scope()), ParseError);
}
