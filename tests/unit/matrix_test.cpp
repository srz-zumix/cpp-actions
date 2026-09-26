#include "iutest.hpp"

#include "workflow/matrix.hpp"

using namespace cpa::workflow;
using cpa::expr::Value;

namespace {

std::vector<std::string> expand(const char* json) {
  std::vector<std::string> out;
  for (const auto& c : expand_matrix(Value::parse_json(json))) out.push_back(c.to_json_string(false));
  return out;
}

}  // namespace

IUTEST(MatrixTest, Null) {
  const auto combos = expand_matrix(Value());
  IUTEST_ASSERT_EQ(1u, combos.size());
  IUTEST_EXPECT_TRUE(combos[0].is_object());
}

IUTEST(MatrixTest, CartesianProduct) {
  const auto c = expand(R"({"os": ["a", "b"], "v": [1, 2]})");
  IUTEST_ASSERT_EQ(4u, c.size());
  IUTEST_EXPECT_EQ(R"({"os":"a","v":1})", c[0]);
  IUTEST_EXPECT_EQ(R"({"os":"a","v":2})", c[1]);
  IUTEST_EXPECT_EQ(R"({"os":"b","v":1})", c[2]);
  IUTEST_EXPECT_EQ(R"({"os":"b","v":2})", c[3]);
}

IUTEST(MatrixTest, Exclude) {
  const auto c = expand(R"({"os": ["a", "b"], "v": [1, 2], "exclude": [{"os": "a", "v": 2}, {"os": "b"}]})");
  IUTEST_ASSERT_EQ(1u, c.size());
  IUTEST_EXPECT_EQ(R"({"os":"a","v":1})", c[0]);
  IUTEST_EXPECT_THROW(expand(R"({"os": ["a"], "exclude": [{"x": 1}]})"), MatrixError);
}

IUTEST(MatrixTest, IncludeDocumentationExample) {
  const auto c = expand(R"({
    "fruit": ["apple", "pear"],
    "animal": ["cat", "dog"],
    "include": [
      {"color": "green"},
      {"color": "pink", "animal": "cat"},
      {"fruit": "apple", "shape": "circle"},
      {"fruit": "banana"},
      {"fruit": "banana", "animal": "cat"}
    ]
  })");
  IUTEST_ASSERT_EQ(6u, c.size());
  IUTEST_EXPECT_EQ(R"({"fruit":"apple","animal":"cat","color":"pink","shape":"circle"})", c[0]);
  IUTEST_EXPECT_EQ(R"({"fruit":"apple","animal":"dog","color":"green","shape":"circle"})", c[1]);
  IUTEST_EXPECT_EQ(R"({"fruit":"pear","animal":"cat","color":"pink"})", c[2]);
  IUTEST_EXPECT_EQ(R"({"fruit":"pear","animal":"dog","color":"green"})", c[3]);
  IUTEST_EXPECT_EQ(R"({"fruit":"banana"})", c[4]);
  IUTEST_EXPECT_EQ(R"({"fruit":"banana","animal":"cat"})", c[5]);
}

IUTEST(MatrixTest, IncludeOnly) {
  const auto c = expand(R"({"include": [{"os": "a", "x": 1}, {"os": "b"}]})");
  IUTEST_ASSERT_EQ(2u, c.size());
  IUTEST_EXPECT_EQ(R"({"os":"a","x":1})", c[0]);
}

IUTEST(MatrixTest, Errors) {
  IUTEST_EXPECT_THROW(expand(R"({"os": []})"), MatrixError);
  IUTEST_EXPECT_THROW(expand(R"({"os": "a"})"), MatrixError);
  IUTEST_EXPECT_THROW(expand(R"({"os": ["a"], "exclude": [{"os": "a"}]})"), MatrixError);
  IUTEST_EXPECT_THROW(expand(R"({"a": [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16], "b": [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]})"),
                      MatrixError);
  IUTEST_EXPECT_THROW(expand_matrix(Value("x")), MatrixError);
}

IUTEST(MatrixTest, DisplaySuffix) {
  IUTEST_EXPECT_EQ("(macos-latest, 3.1, true)",
                   matrix_display_suffix(Value::parse_json(R"({"os": "macos-latest", "v": 3.1, "b": true})")));
  IUTEST_EXPECT_EQ("({\"a\":1})", matrix_display_suffix(Value::parse_json(R"({"cfg": {"a": 1}})")));
  IUTEST_EXPECT_EQ("", matrix_display_suffix(Value::object()));
}
