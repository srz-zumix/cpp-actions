#include "util/glob.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <regex>
#include <system_error>

#include "util/strings.hpp"

namespace cpa::util {

namespace {

std::string filter_to_regex(std::string_view pattern) {
  static const std::string kSpecial = R"(.^$|(){}\/)";
  std::string out = "^";
  bool has_atom = false;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    if (c == '\\' && i + 1 < pattern.size()) {
      const char n = pattern[++i];
      out += '\\';
      out += n;
      has_atom = true;
    } else if (c == '*') {
      if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
        out += ".*";
        ++i;
      } else {
        out += "[^/]*";
      }
      has_atom = false;
    } else if ((c == '?' || c == '+') && has_atom) {
      out += c;
      has_atom = false;
    } else if (c == '[') {
      const auto end = pattern.find(']', i + 1);
      if (end == std::string_view::npos) {
        out += "\\[";
        has_atom = true;
        continue;
      }
      out += '[';
      for (std::size_t j = i + 1; j < end; ++j) {
        if (pattern[j] == '\\') out += '\\';
        out += pattern[j];
      }
      out += ']';
      i = end;
      has_atom = true;
    } else {
      if (kSpecial.find(c) != std::string::npos || c == '?' || c == '+' || c == '[' || c == ']') out += '\\';
      out += c;
      has_atom = true;
    }
  }
  out += '$';
  return out;
}

const std::regex& cached_filter_regex(std::string_view pattern) {
  static std::mutex mutex;
  static std::map<std::string, std::regex, std::less<>> cache;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = cache.find(pattern);
  if (it == cache.end()) {
    std::regex re;
    try {
      re = std::regex(filter_to_regex(pattern), std::regex::ECMAScript);
    } catch (const std::regex_error&) {
      re = std::regex("(?!)");
    }
    it = cache.emplace(std::string(pattern), std::move(re)).first;
  }
  return it->second;
}

bool match_class(std::string_view cls, char c, bool& matched) {
  // cls excludes the surrounding brackets.
  bool negate = false;
  std::size_t i = 0;
  if (!cls.empty() && (cls[0] == '!' || cls[0] == '^')) {
    negate = true;
    i = 1;
  }
  bool hit = false;
  for (; i < cls.size(); ++i) {
    char lo = cls[i];
    if (lo == '\\' && i + 1 < cls.size()) lo = cls[++i];
    if (i + 2 < cls.size() && cls[i + 1] == '-') {
      char hi = cls[i + 2];
      i += 2;
      if (c >= lo && c <= hi) hit = true;
    } else if (c == lo) {
      hit = true;
    }
  }
  matched = negate ? !hit : hit;
  return true;
}

bool match_segment(std::string_view p, std::string_view s) {
  std::size_t pi = 0, si = 0;
  std::size_t star_p = std::string_view::npos, star_s = 0;
  while (si < s.size()) {
    if (pi < p.size()) {
      const char pc = p[pi];
      if (pc == '*') {
        star_p = pi++;
        star_s = si;
        continue;
      }
      if (pc == '?') {
        ++pi;
        ++si;
        continue;
      }
      if (pc == '[') {
        const auto end = p.find(']', pi + 1);
        if (end != std::string_view::npos) {
          bool matched = false;
          match_class(p.substr(pi + 1, end - pi - 1), s[si], matched);
          if (matched) {
            pi = end + 1;
            ++si;
            continue;
          }
        } else if (s[si] == '[') {
          ++pi;
          ++si;
          continue;
        }
      } else {
        char lit = pc;
        std::size_t adv = 1;
        if (pc == '\\' && pi + 1 < p.size()) {
          lit = p[pi + 1];
          adv = 2;
        }
        if (lit == s[si]) {
          pi += adv;
          ++si;
          continue;
        }
      }
    }
    if (star_p != std::string_view::npos) {
      pi = star_p + 1;
      si = ++star_s;
      continue;
    }
    return false;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

std::vector<std::string_view> split_path(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    auto pos = s.find('/', start);
    if (pos == std::string_view::npos) pos = s.size();
    auto seg = s.substr(start, pos - start);
    if (!seg.empty() && seg != ".") out.push_back(seg);
    start = pos + 1;
  }
  return out;
}

bool match_segments(const std::vector<std::string_view>& p, std::size_t pi, const std::vector<std::string_view>& s,
                    std::size_t si) {
  if (pi == p.size()) return si == s.size();
  if (p[pi] == "**") {
    for (std::size_t k = si; k <= s.size(); ++k) {
      if (match_segments(p, pi + 1, s, k)) return true;
    }
    return false;
  }
  if (si == s.size()) return false;
  if (!match_segment(p[pi], s[si])) return false;
  return match_segments(p, pi + 1, s, si + 1);
}

}  // namespace

bool filter_match(std::string_view pattern, std::string_view value) {
  return std::regex_match(value.begin(), value.end(), cached_filter_regex(pattern));
}

bool filter_list_match(const std::vector<std::string>& patterns, std::string_view value) {
  bool matched = false;
  for (const auto& raw : patterns) {
    std::string_view p = raw;
    const bool negate = !p.empty() && p[0] == '!';
    if (negate) p.remove_prefix(1);
    if (filter_match(p, value)) matched = !negate;
  }
  return matched;
}

bool glob_match(std::string_view pattern, std::string_view path) {
  return match_segments(split_path(pattern), 0, split_path(path), 0);
}

std::vector<std::filesystem::path> glob_files(const std::filesystem::path& root,
                                              const std::vector<std::string>& patterns) {
  namespace fs = std::filesystem;
  struct Pattern {
    std::string text;
    bool negate;
  };
  std::vector<Pattern> compiled;
  const auto root_norm = root.lexically_normal();
  for (const auto& raw : patterns) {
    std::string_view p = trim(raw);
    if (p.empty() || p[0] == '#') continue;
    const bool negate = p[0] == '!';
    if (negate) p.remove_prefix(1);
    std::string text(p);
    if (!text.empty() && text[0] == '/') {
      const auto rel = fs::path(text).lexically_normal().lexically_relative(root_norm);
      if (rel.empty() || *rel.begin() == "..") continue;
      text = rel.generic_string();
    }
    compiled.push_back({text, negate});
  }
  std::vector<fs::path> result;
  if (compiled.empty()) return result;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const auto rel = it->path().lexically_relative(root).generic_string();
    // A pattern that matches a directory implicitly matches its descendants.
    std::vector<std::string_view> candidates;
    std::string_view rv(rel);
    candidates.push_back(rv);
    for (std::size_t pos = rv.find('/'); pos != std::string_view::npos; pos = rv.find('/', pos + 1)) {
      candidates.push_back(rv.substr(0, pos));
    }
    bool included = false;
    for (const auto& pat : compiled) {
      for (const auto& cand : candidates) {
        if (glob_match(pat.text, cand)) {
          included = !pat.negate;
          break;
        }
      }
    }
    if (included) result.push_back(it->path());
  }
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace cpa::util
