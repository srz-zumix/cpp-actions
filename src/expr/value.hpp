#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace cpa::expr {

struct ArrayData;
class ObjectData;

enum class Kind { Null, Boolean, Number, String, Array, Object };

const char* kind_name(Kind kind);

// Value model of the Actions expression language. Arrays and objects have
// reference semantics (copies share the same instance), like the runner's
// PipelineContextData.
class Value {
 public:
  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : v_(b) {}
  Value(double d) : v_(d) {}
  Value(int i) : v_(static_cast<double>(i)) {}
  Value(long i) : v_(static_cast<double>(i)) {}
  Value(long long i) : v_(static_cast<double>(i)) {}
  Value(unsigned i) : v_(static_cast<double>(i)) {}
  Value(unsigned long i) : v_(static_cast<double>(i)) {}
  Value(unsigned long long i) : v_(static_cast<double>(i)) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(std::string_view s) : v_(std::string(s)) {}
  Value(const char* s) : v_(std::string(s)) {}
  Value(std::shared_ptr<ArrayData> a) : v_(std::move(a)) {}
  Value(std::shared_ptr<ObjectData> o) : v_(std::move(o)) {}

  static Value array();
  static Value array(std::vector<Value> items);
  static Value object(bool case_sensitive = false);

  Kind kind() const { return static_cast<Kind>(v_.index()); }
  bool is_null() const { return kind() == Kind::Null; }
  bool is_bool() const { return kind() == Kind::Boolean; }
  bool is_number() const { return kind() == Kind::Number; }
  bool is_string() const { return kind() == Kind::String; }
  bool is_array() const { return kind() == Kind::Array; }
  bool is_object() const { return kind() == Kind::Object; }
  bool is_primitive() const { return kind() != Kind::Array && kind() != Kind::Object; }

  bool as_bool() const { return std::get<bool>(v_); }
  double as_number() const { return std::get<double>(v_); }
  const std::string& as_string() const { return std::get<std::string>(v_); }
  ArrayData& array_data() const { return *std::get<std::shared_ptr<ArrayData>>(v_); }
  ObjectData& object_data() const { return *std::get<std::shared_ptr<ObjectData>>(v_); }
  const std::vector<Value>& items() const;

  // Actions coercion rules.
  bool truthy() const;
  double to_number() const;
  std::string to_string() const;

  // Object helpers (no-ops / null for other kinds).
  Value get(std::string_view key) const;
  bool has(std::string_view key) const;
  Value& set(std::string key, Value value);
  // Array helper.
  void push(Value value);

  bool same_instance(const Value& other) const;
  Value deep_copy() const;

  // JSON conversion. Integral numbers are written without a fraction.
  nlohmann::ordered_json to_json() const;
  static Value from_json(const nlohmann::ordered_json& json);
  static Value from_json(const nlohmann::json& json);
  std::string to_json_string(bool pretty = true) const;
  // Throws std::runtime_error on invalid JSON.
  static Value parse_json(std::string_view text);

 private:
  std::variant<std::monostate, bool, double, std::string, std::shared_ptr<ArrayData>, std::shared_ptr<ObjectData>> v_;
};

struct ArrayData {
  std::vector<Value> items;
  // True for the result of a wildcard (`.*` / `[*]`) index operation.
  bool filtered = false;
};

// Insertion-ordered dictionary. Keys are case-insensitive unless created as
// case-sensitive (the `env` context on macOS / Linux).
class ObjectData {
 public:
  explicit ObjectData(bool case_sensitive = false) : case_sensitive_(case_sensitive) {}

  const Value* find(std::string_view key) const;
  Value* find(std::string_view key);
  void set(std::string key, Value value);
  bool erase(std::string_view key);

  std::size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }
  bool case_sensitive() const { return case_sensitive_; }
  const std::vector<std::pair<std::string, Value>>& entries() const { return entries_; }

 private:
  std::string index_key(std::string_view key) const;
  void reindex();

  bool case_sensitive_;
  std::vector<std::pair<std::string, Value>> entries_;
  std::unordered_map<std::string, std::size_t> index_;
};

inline const std::vector<Value>& Value::items() const { return array_data().items; }

// Comparison operators of the expression language (abstract equality with
// OrdinalIgnoreCase string comparison, reference equality for containers).
bool loose_equals(const Value& a, const Value& b);
bool loose_greater(const Value& a, const Value& b);
bool loose_less(const Value& a, const Value& b);

}  // namespace cpa::expr
