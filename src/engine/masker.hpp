#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::engine {

// Replaces registered secret values with "***" (thread-safe).
class Masker {
 public:
  // Registers a value, its lines, its JSON-escaped form and shifted Base64 forms.
  void add(std::string_view value);
  std::string mask(std::string_view text) const;
  bool contains_secret(std::string_view text) const;
  bool empty() const;

 private:
  void add_one(std::string value);
  void add_value(std::string_view value);

  mutable std::mutex mutex_;
  std::vector<std::string> values_;
};

}  // namespace cpa::engine
