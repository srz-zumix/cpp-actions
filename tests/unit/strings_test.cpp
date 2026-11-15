#include "iutest.hpp"

#include <cmath>

#include "util/strings.hpp"

using namespace cpa::util;

IUTEST(StringsTest, CaseInsensitive) {
  IUTEST_EXPECT_TRUE(iequals("GitHub", "github"));
  IUTEST_EXPECT_FALSE(iequals("GitHub", "gitlab"));
  IUTEST_EXPECT_TRUE(istarts_with("refs/heads/main", "REFS/"));
  IUTEST_EXPECT_TRUE(iends_with("Hello.TXT", ".txt"));
  IUTEST_EXPECT_EQ(2u, ifind("abCDef", "cd"));
  // OrdinalIgnoreCase compares upper-cased characters: '_' (0x5F) > 'A' (0x41).
  IUTEST_EXPECT_GT(icompare("_", "a"), 0);
  IUTEST_EXPECT_EQ(0, icompare("abc", "ABC"));
  IUTEST_EXPECT_LT(icompare("ab", "abc"), 0);
}

IUTEST(StringsTest, Trim) {
  IUTEST_EXPECT_EQ("a b", std::string(trim("  a b \t\n")));
  IUTEST_EXPECT_EQ("", std::string(trim("   ")));
}

IUTEST(StringsTest, SplitLines) {
  const auto lines = split_lines("a\r\nb\n\nc");
  IUTEST_ASSERT_EQ(4u, lines.size());
  IUTEST_EXPECT_EQ("a", lines[0]);
  IUTEST_EXPECT_EQ("b", lines[1]);
  IUTEST_EXPECT_EQ("", lines[2]);
  IUTEST_EXPECT_EQ("c", lines[3]);
}

IUTEST(StringsTest, Base64Encode) {
  IUTEST_EXPECT_EQ("", base64_encode(""));
  IUTEST_EXPECT_EQ("Zg==", base64_encode("f"));
  IUTEST_EXPECT_EQ("Zm8=", base64_encode("fo"));
  IUTEST_EXPECT_EQ("Zm9v", base64_encode("foo"));
  IUTEST_EXPECT_EQ("Zm9vYg==", base64_encode("foob"));
  IUTEST_EXPECT_EQ("Zm9vYmE=", base64_encode("fooba"));
  IUTEST_EXPECT_EQ("Zm9vYmFy", base64_encode("foobar"));
  IUTEST_EXPECT_EQ("AP/+", base64_encode(std::string_view("\x00\xff\xfe", 3)));
  IUTEST_EXPECT_EQ("w6nmvKI=", base64_encode("\xc3\xa9\xe6\xbc\xa2"));
}

IUTEST(StringsTest, FormatNumberG15) {
  IUTEST_EXPECT_EQ("0", format_number(0));
  IUTEST_EXPECT_EQ("-0", format_number(-0.0));
  IUTEST_EXPECT_EQ("1", format_number(1));
  IUTEST_EXPECT_EQ("3.1", format_number(3.10));
  IUTEST_EXPECT_EQ("0.1", format_number(0.1));
  IUTEST_EXPECT_EQ("123456789012345", format_number(123456789012345.0));
  IUTEST_EXPECT_EQ("1E+15", format_number(1e15));
  IUTEST_EXPECT_EQ("1.23456789012346E+17", format_number(123456789012345678.0));
  IUTEST_EXPECT_EQ("1E-05", format_number(0.00001));
  IUTEST_EXPECT_EQ("0.0001", format_number(0.0001));
  IUTEST_EXPECT_EQ("NaN", format_number(std::nan("")));
  IUTEST_EXPECT_EQ("Infinity", format_number(HUGE_VAL));
  IUTEST_EXPECT_EQ("-Infinity", format_number(-HUGE_VAL));
}

IUTEST(StringsTest, ParseNumber) {
  IUTEST_EXPECT_EQ(0.0, parse_number(""));
  IUTEST_EXPECT_EQ(0.0, parse_number("   "));
  IUTEST_EXPECT_EQ(12.0, parse_number(" 12 "));
  IUTEST_EXPECT_EQ(-1.5, parse_number("-1.5"));
  IUTEST_EXPECT_EQ(0.5, parse_number(".5"));
  IUTEST_EXPECT_EQ(5.0, parse_number("5."));
  IUTEST_EXPECT_EQ(1000.0, parse_number("1e3"));
  IUTEST_EXPECT_EQ(255.0, parse_number("0xff"));
  IUTEST_EXPECT_EQ(-1.0, parse_number("0xFFFFFFFF"));
  IUTEST_EXPECT_TRUE(std::isnan(parse_number("0x100000000")));
  IUTEST_EXPECT_EQ(8.0, parse_number("0o10"));
  IUTEST_EXPECT_TRUE(std::isinf(parse_number("Infinity")));
  IUTEST_EXPECT_TRUE(std::isnan(parse_number("abc")));
  IUTEST_EXPECT_TRUE(std::isnan(parse_number("1,000")));
  IUTEST_EXPECT_TRUE(std::isnan(parse_number("0X10")));
}

IUTEST(StringsTest, SplitCommandLine) {
  auto args = split_command_line(R"(bash --noprofile -e "{0}")");
  IUTEST_ASSERT_EQ(4u, args.size());
  IUTEST_EXPECT_EQ("bash", args[0]);
  IUTEST_EXPECT_EQ("{0}", args[3]);
  args = split_command_line(R"(a "b c" d\"e f\\"g h")");
  IUTEST_ASSERT_EQ(4u, args.size());
  IUTEST_EXPECT_EQ("b c", args[1]);
  IUTEST_EXPECT_EQ("d\"e", args[2]);
  IUTEST_EXPECT_EQ("f\\g h", args[3]);
}

IUTEST(StringsTest, RandomUuid) {
  const auto a = random_uuid();
  const auto b = random_uuid();
  IUTEST_EXPECT_EQ(36u, a.size());
  IUTEST_EXPECT_NE(a, b);
  IUTEST_EXPECT_EQ('4', a[14]);
}

IUTEST(StringsTest, ParseInt) {
  IUTEST_EXPECT_EQ(42, parse_int(" 42 ").value());
  IUTEST_EXPECT_EQ(-3, parse_int("-3").value());
  IUTEST_EXPECT_FALSE(parse_int("4x").has_value());
  IUTEST_EXPECT_FALSE(parse_int("").has_value());
}
