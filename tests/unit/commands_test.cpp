#include "iutest.hpp"

#include <thread>

#include "engine/commands.hpp"
#include "engine/masker.hpp"

using namespace cpa::engine;

IUTEST(CommandsTest, EscapingRoundTripsWithoutRecursiveDecoding) {
  const std::string text = "percent%25\r\ncolon:comma,";
  IUTEST_EXPECT_EQ(text, unescape_command_data(escape_command_data(text)));
  IUTEST_EXPECT_EQ(text, unescape_command_property(escape_command_property(text)));
  IUTEST_EXPECT_EQ("%0A%3A%2C", unescape_command_property("%250A%253A%252C"));
  IUTEST_EXPECT_EQ("a:b,c\r\n%", unescape_command_property("a%3Ab%2Cc%0D%0A%25"));
  IUTEST_EXPECT_EQ("%3A%2C", unescape_command_data("%3A%2C"));
}

IUTEST(CommandsTest, ModernCommands) {
  const auto cmd = parse_workflow_command(
      "  ::warning file=src/a%2Cb.cpp,line=12,title=A%3AB,empty=,bad,name=x=y::hello%0Aworld%25");
  IUTEST_ASSERT_TRUE(cmd.has_value());
  IUTEST_EXPECT_EQ("warning", cmd->name);
  IUTEST_EXPECT_EQ("src/a,b.cpp", cmd->properties.at("file"));
  IUTEST_EXPECT_EQ("12", cmd->properties.at("line"));
  IUTEST_EXPECT_EQ("A:B", cmd->properties.at("title"));
  IUTEST_EXPECT_EQ("x=y", cmd->properties.at("name"));
  IUTEST_EXPECT_EQ("hello\nworld%", cmd->message);
  IUTEST_EXPECT_EQ(0u, cmd->properties.count("bad"));
  IUTEST_EXPECT_EQ(1u, cmd->properties.count("empty"));
}

IUTEST(CommandsTest, LegacyCommands) {
  const auto cmd = parse_workflow_command("##[error file=a%3Ab;line=3;title=hello;]message%0Asecond");
  IUTEST_ASSERT_TRUE(cmd.has_value());
  IUTEST_EXPECT_EQ("error", cmd->name);
  IUTEST_EXPECT_EQ("a:b", cmd->properties.at("file"));
  IUTEST_EXPECT_EQ("3", cmd->properties.at("line"));
  IUTEST_EXPECT_EQ("hello", cmd->properties.at("title"));
  IUTEST_EXPECT_EQ("message\nsecond", cmd->message);
}

IUTEST(CommandsTest, KnownAndMalformedCommands) {
  for (const auto* name : {"set-env", "set-output", "save-state", "add-path", "add-mask", "debug", "warning",
                           "error", "notice", "group", "endgroup", "echo", "add-matcher", "remove-matcher",
                           "stop-commands"}) {
    IUTEST_EXPECT_TRUE(parse_workflow_command("::" + std::string(name) + "::").has_value());
  }
  for (const auto* line : {"plain", "", "::unknown::x", "::warning", "##[warning", "prefix ::error::x"}) {
    IUTEST_EXPECT_FALSE(parse_workflow_command(line).has_value());
  }
}

IUTEST(CommandsTest, FileAssignments) {
  const auto values = parse_key_value_file("\nA=one=two\r\nEMPTY=\nA=last\nLITERAL=x<<EOF\n");
  IUTEST_ASSERT_EQ(4u, values.size());
  IUTEST_EXPECT_EQ("A", values[0].first);
  IUTEST_EXPECT_EQ("one=two", values[0].second);
  IUTEST_EXPECT_EQ("", values[1].second);
  IUTEST_EXPECT_EQ("last", values[2].second);
  IUTEST_EXPECT_EQ("x<<EOF", values[3].second);
  IUTEST_EXPECT_TRUE(parse_key_value_file("").empty());
}

IUTEST(CommandsTest, HeredocsPreserveInteriorNewlines) {
  const auto values = parse_key_value_file(
      "MULTI<<END\nfirst\n\nlast\nEND\nCRLF<<X\r\none\r\ntwo\r\nX\r\nEMPTY<<Y\nY\nFINAL=ok");
  IUTEST_ASSERT_EQ(4u, values.size());
  IUTEST_EXPECT_EQ("first\n\nlast", values[0].second);
  IUTEST_EXPECT_EQ("one\r\ntwo", values[1].second);
  IUTEST_EXPECT_EQ("", values[2].second);
  IUTEST_EXPECT_EQ("ok", values[3].second);
  const auto terminal = parse_key_value_file("A<<X\nvalue\nX");
  IUTEST_ASSERT_EQ(1u, terminal.size());
  IUTEST_EXPECT_EQ("value", terminal[0].second);
}

IUTEST(CommandsTest, InvalidFileCommands) {
  for (const auto* text : {"broken", "=value", "<<EOF\nEOF\n", "NAME<<\n", "A<<EOF\nvalue\n",
                           "A<<EOF\nunterminated", "A<<EOF\nEOF-not-exact\n"}) {
    IUTEST_EXPECT_THROW(parse_key_value_file(text), FileCommandError);
  }
}

IUTEST(CommandsTest, PathFileSkipsBlankLinesWithoutTrimmingPaths) {
  const auto paths = parse_path_file("\n/a\r\n  \n/path with spaces\n /padded \n/last");
  IUTEST_ASSERT_EQ(4u, paths.size());
  IUTEST_EXPECT_EQ("/a", paths[0]);
  IUTEST_EXPECT_EQ("/path with spaces", paths[1]);
  IUTEST_EXPECT_EQ(" /padded ", paths[2]);
  IUTEST_EXPECT_EQ("/last", paths[3]);
}

IUTEST(MaskerTest, EmptyAndRepeatedValues) {
  Masker masker;
  IUTEST_EXPECT_TRUE(masker.empty());
  masker.add("");
  masker.add(" \r\n\t");
  IUTEST_EXPECT_TRUE(masker.empty());
  masker.add("secret");
  masker.add("secret");
  IUTEST_EXPECT_FALSE(masker.empty());
  IUTEST_EXPECT_TRUE(masker.contains_secret("a secret b"));
  IUTEST_EXPECT_FALSE(masker.contains_secret("public"));
  IUTEST_EXPECT_EQ("a *** b ***", masker.mask("a secret b secret"));
  IUTEST_EXPECT_EQ("", masker.mask(""));
}

IUTEST(MaskerTest, OverlappingAndAdjacentSecrets) {
  Masker masker;
  masker.add("abc");
  masker.add("bcd");
  masker.add("xyz");
  IUTEST_EXPECT_EQ("***!", masker.mask("abcdxyz!"));
  IUTEST_EXPECT_EQ("x***x", masker.mask("xabcdx"));
}

IUTEST(MaskerTest, MultilineAndJsonEscapedSecrets) {
  Masker masker;
  masker.add("first\nsecond");
  masker.add("a\"b\\c\t");
  IUTEST_EXPECT_EQ("***", masker.mask("first\nsecond"));
  IUTEST_EXPECT_EQ("*** / ***", masker.mask("first / second"));
  IUTEST_EXPECT_EQ("***", masker.mask("first\\nsecond"));
  IUTEST_EXPECT_EQ("***", masker.mask("a\\\"b\\\\c\\t"));
}

IUTEST(MaskerTest, ConcurrentRegistrationAndMasking) {
  Masker masker;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&masker, i] {
      masker.add("secret-" + std::to_string(i));
      for (int j = 0; j < 20; ++j) (void)masker.mask("secret-0 secret-7");
    });
  }
  for (auto& thread : threads) thread.join();
  IUTEST_EXPECT_EQ("*** ***", masker.mask("secret-0 secret-7"));
}
