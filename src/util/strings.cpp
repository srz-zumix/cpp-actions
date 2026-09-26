#include "util/strings.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

namespace cpa::util {

char ascii_upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string to_upper(std::string_view s) {
  std::string r(s);
  for (auto& c : r) c = ascii_upper(c);
  return r;
}

std::string to_lower(std::string_view s) {
  std::string r(s);
  for (auto& c : r) c = ascii_lower(c);
  return r;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
  }
  return true;
}

int icompare(std::string_view a, std::string_view b) noexcept {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    const auto ca = static_cast<unsigned char>(ascii_upper(a[i]));
    const auto cb = static_cast<unsigned char>(ascii_upper(b[i]));
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

bool istarts_with(std::string_view s, std::string_view prefix) noexcept {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool iends_with(std::string_view s, std::string_view suffix) noexcept {
  return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::size_t ifind(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) return 0;
  if (needle.size() > haystack.size()) return std::string_view::npos;
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (iequals(haystack.substr(i, needle.size()), needle)) return i;
  }
  return std::string_view::npos;
}

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim_left(std::string_view s) noexcept {
  std::size_t i = 0;
  while (i < s.size() && is_space(s[i])) ++i;
  return s.substr(i);
}

std::string_view trim_right(std::string_view s) noexcept {
  std::size_t n = s.size();
  while (n > 0 && is_space(s[n - 1])) --n;
  return s.substr(0, n);
}

std::string_view trim(std::string_view s) noexcept { return trim_right(trim_left(s)); }

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    const auto pos = s.find(sep, start);
    if (pos == std::string_view::npos) {
      out.emplace_back(s.substr(start));
      break;
    }
    out.emplace_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::vector<std::string> split_lines(std::string_view s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < s.size()) {
    auto pos = s.find('\n', start);
    if (pos == std::string_view::npos) pos = s.size();
    auto line = s.substr(start, pos - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    out.emplace_back(line);
    start = pos + 1;
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string r;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) r += sep;
    r += parts[i];
  }
  return r;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
  if (from.empty()) return std::string(s);
  std::string r;
  std::size_t start = 0;
  while (true) {
    const auto pos = s.find(from, start);
    if (pos == std::string_view::npos) break;
    r.append(s.substr(start, pos - start));
    r.append(to);
    start = pos + from.size();
  }
  r.append(s.substr(start));
  return r;
}

std::string format_number(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
  std::array<char, 64> buf{};
  std::snprintf(buf.data(), buf.size(), "%.15G", value);
  return std::string(buf.data());
}

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Validates [+-]?(digits)?(.digits?)?([eE][+-]?digits)? with at least one mantissa digit.
bool is_decimal_number(std::string_view s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  std::size_t digits = 0;
  while (i < s.size() && is_digit(s[i])) ++i, ++digits;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && is_digit(s[i])) ++i, ++digits;
  }
  if (digits == 0) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    std::size_t exp_digits = 0;
    while (i < s.size() && is_digit(s[i])) ++i, ++exp_digits;
    if (exp_digits == 0) return false;
  }
  return i == s.size();
}

}  // namespace

double parse_number(std::string_view text) {
  const auto s = trim(text);
  if (s.empty()) return 0.0;
  if (is_decimal_number(s)) {
    const std::string copy(s);
    errno = 0;
    const double v = std::strtod(copy.c_str(), nullptr);
    return v;
  }
  if (s.size() > 2 && s[0] == '0' && s[1] == 'x') {
    auto digits = s.substr(2);
    for (char c : digits) {
      if (!is_hex(c)) return std::nan("");
    }
    while (digits.size() > 1 && digits.front() == '0') digits.remove_prefix(1);
    if (digits.size() > 8) return std::nan("");
    std::uint32_t v = 0;
    for (char c : digits) {
      const std::uint32_t d = is_digit(c) ? static_cast<std::uint32_t>(c - '0')
                                          : static_cast<std::uint32_t>(ascii_lower(c) - 'a' + 10);
      v = (v << 4) | d;
    }
    return static_cast<double>(static_cast<std::int32_t>(v));
  }
  if (s.size() > 2 && s[0] == '0' && s[1] == 'o') {
    std::uint64_t v = 0;
    for (char c : s.substr(2)) {
      if (c < '0' || c > '7') return std::nan("");
      v = v * 8 + static_cast<std::uint64_t>(c - '0');
      if (v > 0xFFFFFFFFull) return std::nan("");
    }
    return static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v)));
  }
  if (s == "Infinity") return HUGE_VAL;
  if (s == "-Infinity") return -HUGE_VAL;
  return std::nan("");
}

std::vector<std::string> split_command_line(std::string_view command_line) {
  std::vector<std::string> args;
  std::string current;
  bool in_quotes = false;
  bool has_token = false;
  for (std::size_t i = 0; i < command_line.size(); ++i) {
    const char c = command_line[i];
    if (c == '\\') {
      std::size_t count = 0;
      while (i < command_line.size() && command_line[i] == '\\') {
        ++count;
        ++i;
      }
      if (i < command_line.size() && command_line[i] == '"') {
        current.append(count / 2, '\\');
        if (count % 2 == 1) {
          current.push_back('"');
        } else {
          --i;  // let the quote be processed below
        }
      } else {
        current.append(count, '\\');
        --i;
      }
      has_token = true;
      continue;
    }
    if (c == '"') {
      if (in_quotes && i + 1 < command_line.size() && command_line[i + 1] == '"') {
        current.push_back('"');
        ++i;
      } else {
        in_quotes = !in_quotes;
      }
      has_token = true;
      continue;
    }
    if (!in_quotes && (c == ' ' || c == '\t')) {
      if (has_token) {
        args.push_back(std::move(current));
        current.clear();
        has_token = false;
      }
      continue;
    }
    current.push_back(c);
    has_token = true;
  }
  if (has_token) args.push_back(std::move(current));
  return args;
}

std::string shell_quote(std::string_view s) {
  if (s.empty()) return "''";
  bool safe = true;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' || c == '_' ||
                    c == '.' || c == '/' || c == '=' || c == ':' || c == ',' || c == '+' || c == '@' || c == '%';
    if (!ok) {
      safe = false;
      break;
    }
  }
  if (safe) return std::string(s);
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') {
      r += "'\\''";
    } else {
      r.push_back(c);
    }
  }
  r.push_back('\'');
  return r;
}

namespace {

std::mt19937_64& rng() {
  static std::mt19937_64 engine = [] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    return std::mt19937_64(seq);
  }();
  return engine;
}

std::mutex& rng_mutex() {
  static std::mutex m;
  return m;
}

}  // namespace

std::string random_hex(std::size_t bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string r;
  r.reserve(bytes * 2);
  std::lock_guard<std::mutex> lock(rng_mutex());
  for (std::size_t i = 0; i < bytes; ++i) {
    const auto b = static_cast<unsigned>(rng()() & 0xFFu);
    r.push_back(kHex[b >> 4]);
    r.push_back(kHex[b & 0xF]);
  }
  return r;
}

std::string random_uuid() {
  std::string h = random_hex(16);
  h[12] = '4';
  static constexpr char kVariant[] = "89ab";
  h[16] = kVariant[static_cast<unsigned char>(h[16]) % 4];
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

std::string format_duration_ms(std::int64_t ms) {
  std::array<char, 64> buf{};
  if (ms < 1000) {
    std::snprintf(buf.data(), buf.size(), "%lldms", static_cast<long long>(ms));
  } else if (ms < 60'000) {
    std::snprintf(buf.data(), buf.size(), "%.1fs", static_cast<double>(ms) / 1000.0);
  } else {
    const auto total = ms / 1000;
    std::snprintf(buf.data(), buf.size(), "%lldm%02llds", static_cast<long long>(total / 60),
                  static_cast<long long>(total % 60));
  }
  return std::string(buf.data());
}

std::optional<std::int64_t> parse_int(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') {
    neg = s[0] == '-';
    i = 1;
  }
  if (i >= s.size()) return std::nullopt;
  std::int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (!is_digit(s[i])) return std::nullopt;
    if (v > (INT64_MAX - 9) / 10) return std::nullopt;
    v = v * 10 + (s[i] - '0');
  }
  return neg ? -v : v;
}

}  // namespace cpa::util
