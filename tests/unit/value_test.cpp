#include "iutest.hpp"

#include <cmath>

#include "expr/value.hpp"

using cpa::expr::Kind;
using cpa::expr::Value;

IUTEST(ValueTest, Truthiness) {
  IUTEST_EXPECT_FALSE(Value().truthy());
  IUTEST_EXPECT_FALSE(Value(false).truthy());
  IUTEST_EXPECT_FALSE(Value(0).truthy());
  IUTEST_EXPECT_FALSE(Value(-0.0).truthy());
  IUTEST_EXPECT_FALSE(Value(std::nan("")).truthy());
  IUTEST_EXPECT_FALSE(Value("").truthy());
  IUTEST_EXPECT_TRUE(Value("false").truthy());
  IUTEST_EXPECT_TRUE(Value(0.1).truthy());
  IUTEST_EXPECT_TRUE(Value::array().truthy());
  IUTEST_EXPECT_TRUE(Value::object().truthy());
}

IUTEST(ValueTest, ToString) {
  IUTEST_EXPECT_EQ("", Value().to_string());
  IUTEST_EXPECT_EQ("true", Value(true).to_string());
  IUTEST_EXPECT_EQ("3.1", Value(3.10).to_string());
  IUTEST_EXPECT_EQ("Array", Value::array().to_string());
  IUTEST_EXPECT_EQ("Object", Value::object().to_string());
}

IUTEST(ValueTest, ObjectIsCaseInsensitiveByDefault) {
  auto o = Value::object();
  o.set("Foo", Value(1));
  IUTEST_EXPECT_EQ(1.0, o.get("foo").as_number());
  o.set("FOO", Value(2));
  IUTEST_ASSERT_EQ(1u, o.object_data().size());
  IUTEST_EXPECT_EQ("Foo", o.object_data().entries()[0].first);
  IUTEST_EXPECT_EQ(2.0, o.get("foo").as_number());

  auto env = Value::object(true);
  env.set("PATH", Value("/bin"));
  IUTEST_EXPECT_TRUE(env.get("path").is_null());
  IUTEST_EXPECT_EQ("/bin", env.get("PATH").as_string());
}

IUTEST(ValueTest, LooseEquality) {
  using cpa::expr::loose_equals;
  IUTEST_EXPECT_TRUE(loose_equals(Value(), Value()));
  IUTEST_EXPECT_TRUE(loose_equals(Value(), Value(0)));
  IUTEST_EXPECT_TRUE(loose_equals(Value(), Value("")));
  IUTEST_EXPECT_TRUE(loose_equals(Value(true), Value(1)));
  IUTEST_EXPECT_TRUE(loose_equals(Value(true), Value("1")));
  IUTEST_EXPECT_FALSE(loose_equals(Value(true), Value("true")));
  IUTEST_EXPECT_TRUE(loose_equals(Value("ABC"), Value("abc")));
  IUTEST_EXPECT_TRUE(loose_equals(Value("0x10"), Value(16)));
  IUTEST_EXPECT_FALSE(loose_equals(Value(std::nan("")), Value(std::nan(""))));
  IUTEST_EXPECT_FALSE(loose_equals(Value(), Value::object()));
  auto o = Value::object();
  IUTEST_EXPECT_TRUE(loose_equals(o, o));
  IUTEST_EXPECT_FALSE(loose_equals(o, Value::object()));
}

IUTEST(ValueTest, LooseComparison) {
  using cpa::expr::loose_greater;
  using cpa::expr::loose_less;
  IUTEST_EXPECT_TRUE(loose_greater(Value(2), Value("1")));
  IUTEST_EXPECT_TRUE(loose_less(Value("a"), Value("B")));
  IUTEST_EXPECT_TRUE(loose_greater(Value(true), Value(false)));
  IUTEST_EXPECT_FALSE(loose_greater(Value("abc"), Value(1)));  // NaN
  IUTEST_EXPECT_FALSE(loose_less(Value::array(), Value(1)));
}

IUTEST(ValueTest, Json) {
  const auto v = Value::parse_json(R"({"a": [1, 2.5, "x", null, true], "B": {"c": {}}})");
  IUTEST_ASSERT_TRUE(v.is_object());
  IUTEST_EXPECT_EQ(2.5, v.get("a").items()[1].as_number());
  IUTEST_EXPECT_TRUE(v.get("b").get("C").is_object());
  IUTEST_EXPECT_EQ("{\n  \"a\": [\n    1,\n    2.5,\n    \"x\",\n    null,\n    true\n  ],\n  \"B\": {\n    \"c\": {}\n  }\n}",
                   v.to_json_string());
  IUTEST_EXPECT_THROW(Value::parse_json("{"), std::runtime_error);
}

IUTEST(ValueTest, DeepCopy) {
  auto v = Value::parse_json(R"({"a": {"b": 1}})");
  auto shallow = v;
  auto deep = v.deep_copy();
  v.get("a").set("b", Value(2));
  IUTEST_EXPECT_EQ(2.0, shallow.get("a").get("b").as_number());
  IUTEST_EXPECT_EQ(1.0, deep.get("a").get("b").as_number());
}
