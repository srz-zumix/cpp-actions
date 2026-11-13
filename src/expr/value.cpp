#include "expr/value.hpp"

#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "util/strings.hpp"

namespace cpa::expr {

const char* kind_name(Kind kind) {
  switch (kind) {
    case Kind::Null: return "Null";
    case Kind::Boolean: return "Boolean";
    case Kind::Number: return "Number";
    case Kind::String: return "String";
    case Kind::Array: return "Array";
    case Kind::Object: return "Object";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// ObjectData

std::string ObjectData::index_key(std::string_view key) const {
  return case_sensitive_ ? std::string(key) : util::to_upper(key);
}

const Value* ObjectData::find(std::string_view key) const {
  const auto it = index_.find(index_key(key));
  return it == index_.end() ? nullptr : &entries_[it->second].second;
}

Value* ObjectData::find(std::string_view key) {
  const auto it = index_.find(index_key(key));
  return it == index_.end() ? nullptr : &entries_[it->second].second;
}

void ObjectData::set(std::string key, Value value) {
  auto k = index_key(key);
  const auto it = index_.find(k);
  if (it != index_.end()) {
    entries_[it->second].second = std::move(value);
    return;
  }
  index_.emplace(std::move(k), entries_.size());
  entries_.emplace_back(std::move(key), std::move(value));
}

bool ObjectData::erase(std::string_view key) {
  const auto it = index_.find(index_key(key));
  if (it == index_.end()) return false;
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(it->second));
  reindex();
  return true;
}

void ObjectData::reindex() {
  index_.clear();
  for (std::size_t i = 0; i < entries_.size(); ++i) index_.emplace(index_key(entries_[i].first), i);
}

// ---------------------------------------------------------------------------
// Value

Value Value::array() { return Value(std::make_shared<ArrayData>()); }

Value Value::array(std::vector<Value> items) {
  auto a = std::make_shared<ArrayData>();
  a->items = std::move(items);
  return Value(std::move(a));
}

Value Value::object(bool case_sensitive) { return Value(std::make_shared<ObjectData>(case_sensitive)); }

bool Value::truthy() const {
  switch (kind()) {
    case Kind::Null: return false;
    case Kind::Boolean: return as_bool();
    case Kind::Number: {
      const double d = as_number();
      return !(d == 0.0 || std::isnan(d));
    }
    case Kind::String: return !as_string().empty();
    case Kind::Array:
    case Kind::Object: return true;
  }
  return false;
}

double Value::to_number() const {
  switch (kind()) {
    case Kind::Null: return 0.0;
    case Kind::Boolean: return as_bool() ? 1.0 : 0.0;
    case Kind::Number: return as_number();
    case Kind::String: return util::parse_number(as_string());
    case Kind::Array:
    case Kind::Object: return std::nan("");
  }
  return std::nan("");
}

std::string Value::to_string() const {
  switch (kind()) {
    case Kind::Null: return std::string();
    case Kind::Boolean: return as_bool() ? "true" : "false";
    case Kind::Number: return util::format_number(as_number());
    case Kind::String: return as_string();
    case Kind::Array: return "Array";
    case Kind::Object: return "Object";
  }
  return std::string();
}

Value Value::get(std::string_view key) const {
  if (!is_object()) return Value();
  const auto* v = object_data().find(key);
  return v ? *v : Value();
}

bool Value::has(std::string_view key) const { return is_object() && object_data().find(key) != nullptr; }

Value& Value::set(std::string key, Value value) {
  if (!is_object()) throw std::logic_error("Value::set on non-object");
  auto& obj = object_data();
  obj.set(key, std::move(value));
  return *obj.find(key);
}

void Value::push(Value value) {
  if (!is_array()) throw std::logic_error("Value::push on non-array");
  array_data().items.push_back(std::move(value));
}

bool Value::same_instance(const Value& other) const {
  if (is_array() && other.is_array()) return &array_data() == &other.array_data();
  if (is_object() && other.is_object()) return &object_data() == &other.object_data();
  return false;
}

Value Value::deep_copy() const {
  if (is_array()) {
    auto a = std::make_shared<ArrayData>();
    a->filtered = array_data().filtered;
    for (const auto& item : items()) a->items.push_back(item.deep_copy());
    return Value(std::move(a));
  }
  if (is_object()) {
    auto o = std::make_shared<ObjectData>(object_data().case_sensitive());
    for (const auto& [k, v] : object_data().entries()) o->set(k, v.deep_copy());
    return Value(std::move(o));
  }
  return *this;
}

nlohmann::ordered_json Value::to_json() const {
  switch (kind()) {
    case Kind::Null: return nullptr;
    case Kind::Boolean: return as_bool();
    case Kind::Number: {
      const double d = as_number();
      if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.2e18) {
        return static_cast<std::int64_t>(d);
      }
      return d;
    }
    case Kind::String: return as_string();
    case Kind::Array: {
      auto j = nlohmann::ordered_json::array();
      for (const auto& item : items()) j.push_back(item.to_json());
      return j;
    }
    case Kind::Object: {
      auto j = nlohmann::ordered_json::object();
      for (const auto& [k, v] : object_data().entries()) j[k] = v.to_json();
      return j;
    }
  }
  return nullptr;
}

namespace {

template <class Json>
Value from_json_impl(const Json& j) {
  if (j.is_null()) return Value();
  if (j.is_boolean()) return Value(j.template get<bool>());
  if (j.is_number()) return Value(j.template get<double>());
  if (j.is_string()) return Value(j.template get<std::string>());
  if (j.is_array()) {
    auto v = Value::array();
    for (const auto& item : j) v.push(from_json_impl(item));
    return v;
  }
  if (j.is_object()) {
    auto v = Value::object();
    for (auto it = j.begin(); it != j.end(); ++it) v.set(it.key(), from_json_impl(it.value()));
    return v;
  }
  return Value();
}

}  // namespace

Value Value::from_json(const nlohmann::ordered_json& json) { return from_json_impl(json); }
Value Value::from_json(const nlohmann::json& json) { return from_json_impl(json); }

std::string Value::to_json_string(bool pretty) const {
  return to_json().dump(pretty ? 2 : -1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

Value Value::parse_json(std::string_view text) {
  try {
    return from_json(nlohmann::ordered_json::parse(text.begin(), text.end()));
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error(std::string("Error parsing fromJson: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Comparisons

namespace {

enum class Op { Equal, Greater, Less };

bool compare_same_kind(const Value& a, const Value& b, Op op) {
  switch (a.kind()) {
    case Kind::Null: return op == Op::Equal;
    case Kind::Number: {
      const double x = a.as_number();
      const double y = b.as_number();
      if (std::isnan(x) || std::isnan(y)) return false;
      return op == Op::Equal ? x == y : (op == Op::Greater ? x > y : x < y);
    }
    case Kind::String: {
      const int c = util::icompare(a.as_string(), b.as_string());
      return op == Op::Equal ? c == 0 : (op == Op::Greater ? c > 0 : c < 0);
    }
    case Kind::Boolean: {
      const bool x = a.as_bool();
      const bool y = b.as_bool();
      return op == Op::Equal ? x == y : (op == Op::Greater ? (x && !y) : (!x && y));
    }
    case Kind::Array:
    case Kind::Object: return op == Op::Equal && a.same_instance(b);
  }
  return false;
}

bool compare(const Value& a, const Value& b, Op op) {
  const Kind ka = a.kind();
  const Kind kb = b.kind();
  if (ka == kb) return compare_same_kind(a, b, op);
  if (ka == Kind::Number && kb == Kind::String) return compare_same_kind(a, Value(b.to_number()), op);
  if (ka == Kind::String && kb == Kind::Number) return compare_same_kind(Value(a.to_number()), b, op);
  if (ka == Kind::Boolean || ka == Kind::Null) return compare(Value(a.to_number()), b, op);
  if (kb == Kind::Boolean || kb == Kind::Null) return compare(a, Value(b.to_number()), op);
  return false;
}

}  // namespace

bool loose_equals(const Value& a, const Value& b) { return compare(a, b, Op::Equal); }
bool loose_greater(const Value& a, const Value& b) { return compare(a, b, Op::Greater); }
bool loose_less(const Value& a, const Value& b) { return compare(a, b, Op::Less); }

}  // namespace cpa::expr
