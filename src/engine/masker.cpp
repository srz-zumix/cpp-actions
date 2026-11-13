#include "engine/masker.hpp"

#include <algorithm>

#include "util/strings.hpp"

namespace cpa::engine {

namespace {

std::string json_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

}  // namespace

void Masker::add_one(std::string value) {
  if (util::trim(value).empty()) return;
  if (std::find(values_.begin(), values_.end(), value) != values_.end()) return;
  values_.push_back(std::move(value));
}

void Masker::add_value(std::string_view value) {
  if (util::trim(value).empty()) return;
  add_one(std::string(value));
  // Match GitHub's byte shifts for Base64(user:secret), regardless of user length.
  for (std::size_t shift = 0; shift < 3 && shift < value.size(); ++shift) {
    add_one(util::base64_encode(value.substr(shift)));
  }
}

void Masker::add(std::string_view value) {
  if (util::trim(value).empty()) return;
  std::lock_guard lock(mutex_);
  add_value(value);
  if (value.find_first_of("\r\n") != std::string_view::npos) {
    for (const auto& line : util::split_lines(value)) add_value(util::trim(line));
  }
  const auto escaped = json_escape(value);
  if (escaped != value) add_one(escaped);
}

bool Masker::empty() const {
  std::lock_guard lock(mutex_);
  return values_.empty();
}

std::string Masker::mask(std::string_view text) const {
  std::lock_guard lock(mutex_);
  if (values_.empty() || text.empty()) return std::string(text);
  // Collect the ranges of all occurrences and merge the overlapping ones.
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  for (const auto& v : values_) {
    for (auto pos = text.find(v); pos != std::string_view::npos; pos = text.find(v, pos + 1)) {
      ranges.emplace_back(pos, pos + v.size());
    }
  }
  if (ranges.empty()) return std::string(text);
  std::sort(ranges.begin(), ranges.end());
  std::string out;
  std::size_t cursor = 0;
  std::size_t i = 0;
  while (i < ranges.size()) {
    auto [start, end] = ranges[i];
    for (++i; i < ranges.size() && ranges[i].first <= end; ++i) end = std::max(end, ranges[i].second);
    out.append(text.substr(cursor, start - cursor));
    out += "***";
    cursor = end;
  }
  out.append(text.substr(cursor));
  return out;
}

bool Masker::contains_secret(std::string_view text) const {
  std::lock_guard lock(mutex_);
  return std::any_of(values_.begin(), values_.end(),
                     [&](const std::string& v) { return text.find(v) != std::string_view::npos; });
}

}  // namespace cpa::engine
