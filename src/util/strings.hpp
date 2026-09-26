#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::util {

// ASCII helpers. GitHub compares identifiers with OrdinalIgnoreCase; ASCII
// folding is sufficient for workflow keys, context names and function names.
char ascii_upper(char c) noexcept;
char ascii_lower(char c) noexcept;
std::string to_upper(std::string_view s);
std::string to_lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b) noexcept;
// Ordinal comparison of the upper-cased strings (like OrdinalIgnoreCase).
int icompare(std::string_view a, std::string_view b) noexcept;
bool istarts_with(std::string_view s, std::string_view prefix) noexcept;
bool iends_with(std::string_view s, std::string_view suffix) noexcept;
std::size_t ifind(std::string_view haystack, std::string_view needle) noexcept;

bool is_space(char c) noexcept;
std::string_view trim(std::string_view s) noexcept;
std::string_view trim_left(std::string_view s) noexcept;
std::string_view trim_right(std::string_view s) noexcept;

std::vector<std::string> split(std::string_view s, char sep);
std::vector<std::string> split_lines(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replace_all(std::string_view s, std::string_view from, std::string_view to);

// Number formatting compatible with .NET Double.ToString("G15", Invariant).
std::string format_number(double value);
// Number parsing compatible with the Actions expression engine
// (ExpressionUtility.ParseNumber). Returns NaN when the text is not a number.
double parse_number(std::string_view text);

// Splits a command line into arguments using rules close to .NET's
// ProcessStartInfo argument parsing (used for custom shells).
std::vector<std::string> split_command_line(std::string_view command_line);

// Quotes a string for POSIX shells when needed (for display only).
std::string shell_quote(std::string_view s);

// Random identifiers.
std::string random_uuid();
std::string random_hex(std::size_t bytes);

std::string format_duration_ms(std::int64_t ms);

std::optional<std::int64_t> parse_int(std::string_view s);

}  // namespace cpa::util
