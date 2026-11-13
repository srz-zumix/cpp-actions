#include "iutest.hpp"

#include <cmath>

#include "workflow/yaml.hpp"

using namespace cpa::yaml;

IUTEST(YamlTest, CoreSchemaScalars) {
  const auto doc = load(R"(
str: hello
quoted: "true"
single: '3.10'
yes: yes
on: push
t: true
f: False
n1: null
n2: ~
n3:
int: 42
neg: -7
float: 3.10
exp: 1e3
hex: 0x1F
oct: 0o17
inf: .inf
nan: .NaN
lead0: 0123
version: 1.2.3
tagged: !!str 12
)",
                        "test.yml");
  const Node& r = *doc->root;
  IUTEST_ASSERT_TRUE(r.is_mapping());
  IUTEST_EXPECT_EQ(NodeKind::String, r.get("str")->kind);
  IUTEST_EXPECT_EQ(NodeKind::String, r.get("quoted")->kind);
  IUTEST_EXPECT_TRUE(r.get("quoted")->quoted);
  IUTEST_EXPECT_EQ("3.10", r.get("single")->scalar_string());
  IUTEST_EXPECT_EQ(NodeKind::String, r.get("yes")->kind);  // YAML 1.1 booleans are strings
  IUTEST_EXPECT_EQ("push", r.get("on")->text);
  IUTEST_EXPECT_EQ(NodeKind::Boolean, r.get("t")->kind);
  IUTEST_EXPECT_FALSE(r.get("f")->boolean);
  IUTEST_EXPECT_TRUE(r.get("n1")->is_null());
  IUTEST_EXPECT_TRUE(r.get("n2")->is_null());
  IUTEST_EXPECT_TRUE(r.get("n3")->is_null());
  IUTEST_EXPECT_EQ(42.0, r.get("int")->number);
  IUTEST_EXPECT_EQ(-7.0, r.get("neg")->number);
  IUTEST_EXPECT_EQ("3.1", r.get("float")->scalar_string());
  IUTEST_EXPECT_EQ(1000.0, r.get("exp")->number);
  IUTEST_EXPECT_EQ(31.0, r.get("hex")->number);
  IUTEST_EXPECT_EQ(15.0, r.get("oct")->number);
  IUTEST_EXPECT_TRUE(std::isinf(r.get("inf")->number));
  IUTEST_EXPECT_TRUE(std::isnan(r.get("nan")->number));
  IUTEST_EXPECT_EQ(123.0, r.get("lead0")->number);
  IUTEST_EXPECT_EQ(NodeKind::String, r.get("version")->kind);
  IUTEST_EXPECT_EQ(NodeKind::String, r.get("tagged")->kind);
  IUTEST_EXPECT_EQ("12", r.get("tagged")->text);
}

IUTEST(YamlTest, Marks) {
  const auto doc = load("a: 1\nb:\n  - x\n  - y: z\n", "m.yml");
  const Node* b = doc->root->get("b");
  IUTEST_ASSERT_NOTNULL(b);
  IUTEST_EXPECT_EQ(3, b->mark.line);
  IUTEST_EXPECT_EQ(3, b->mark.column);
  IUTEST_EXPECT_EQ(4, b->items[1]->mark.line);
  IUTEST_EXPECT_EQ(5, b->items[1]->mark.column);
}

IUTEST(YamlTest, DuplicateKeysAreErrors) {
  try {
    load("jobs:\n  a: 1\n  a: 2\n", "dup.yml");
    IUTEST_FAIL() << "expected LoadError";
  } catch (const LoadError& e) {
    IUTEST_EXPECT_EQ(3, e.mark().line);
    IUTEST_EXPECT_EQ("'a' is already defined", e.message());
  }
}

IUTEST(YamlTest, SyntaxError) {
  IUTEST_EXPECT_THROW(load("a: [1, 2\n", "bad.yml"), LoadError);
  IUTEST_EXPECT_THROW(load("a: !!bool maybe\n", "bad.yml"), LoadError);
  IUTEST_EXPECT_THROW(load("a: !custom x\n", "bad.yml"), LoadError);
}

IUTEST(YamlTest, AnchorsAndAliases) {
  const auto doc = load("base: &b {x: 1}\nuse: *b\n", "a.yml");
  IUTEST_EXPECT_EQ(doc->root->get("base"), doc->root->get("use"));
}

IUTEST(YamlTest, BlockScalars) {
  const auto doc = load("run: |\n  echo a\n  echo b\nfolded: >\n  a\n  b\n", "b.yml");
  IUTEST_EXPECT_EQ("echo a\necho b\n", doc->root->get("run")->text);
  IUTEST_EXPECT_EQ("a b\n", doc->root->get("folded")->text);
  IUTEST_EXPECT_TRUE(doc->root->get("run")->is_string());
}

IUTEST(YamlTest, ToValue) {
  const auto doc = load("m: {a: [1, 'two', true, null]}\n", "v.yml");
  const auto v = to_value(*doc->root);
  IUTEST_EXPECT_EQ("two", v.get("m").get("A").items()[1].as_string());
}

IUTEST(YamlTest, EmptyDocument) {
  const auto doc = load("", "empty.yml");
  IUTEST_ASSERT_NOTNULL(doc->root);
  IUTEST_EXPECT_TRUE(doc->root->is_null());
}
