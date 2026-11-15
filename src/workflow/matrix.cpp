#include "workflow/matrix.hpp"

#include <set>

namespace cpa::workflow {

using expr::Kind;
using expr::Value;

namespace {

bool deep_equal(const Value& a, const Value& b) {
  if (a.kind() != b.kind()) return false;
  switch (a.kind()) {
    case Kind::Null:
      return true;
    case Kind::Boolean:
      return a.as_bool() == b.as_bool();
    case Kind::Number:
      return a.as_number() == b.as_number();
    case Kind::String:
      return a.as_string() == b.as_string();
    case Kind::Array: {
      const auto& x = a.items();
      const auto& y = b.items();
      if (x.size() != y.size()) return false;
      for (std::size_t i = 0; i < x.size(); ++i) {
        if (!deep_equal(x[i], y[i])) return false;
      }
      return true;
    }
    case Kind::Object: {
      const auto& x = a.object_data();
      const auto& y = b.object_data();
      if (x.size() != y.size()) return false;
      for (const auto& [k, v] : x.entries()) {
        const Value* other = y.find(k);
        if (other == nullptr || !deep_equal(v, *other)) return false;
      }
      return true;
    }
  }
  return false;
}

// `pattern` matches `value` when every key of an object pattern matches
// (recursively); other values must be equal.
bool partial_match(const Value& value, const Value& pattern) {
  if (pattern.is_object() && value.is_object()) {
    for (const auto& [k, v] : pattern.object_data().entries()) {
      const Value* actual = value.object_data().find(k);
      if (actual == nullptr || !partial_match(*actual, v)) return false;
    }
    return true;
  }
  return deep_equal(value, pattern);
}

std::string display_value(const Value& v) {
  if (v.is_primitive()) return v.to_string();
  return v.to_json_string(false);
}

}  // namespace

std::vector<Value> expand_matrix(const Value& matrix) {
  if (matrix.is_null()) return {Value::object()};
  if (!matrix.is_object()) {
    throw MatrixError(std::string("Unexpected value of type '") + expr::kind_name(matrix.kind()) +
                      "' for 'matrix'; a mapping is expected");
  }
  std::vector<std::string> keys;
  Value include;
  Value exclude;
  for (const auto& [k, v] : matrix.object_data().entries()) {
    if (k == "include") {
      include = v;
    } else if (k == "exclude") {
      exclude = v;
    } else {
      if (!v.is_array()) {
        throw MatrixError("Matrix vector '" + k + "' must be a sequence, got '" + expr::kind_name(v.kind()) + "'");
      }
      if (v.items().empty()) throw MatrixError("Matrix vector '" + k + "' does not contain any values");
      keys.push_back(k);
    }
  }
  if (!include.is_null() && !include.is_array()) throw MatrixError("Matrix 'include' must be a sequence");
  if (!exclude.is_null() && !exclude.is_array()) throw MatrixError("Matrix 'exclude' must be a sequence");

  std::vector<Value> combos;
  if (!keys.empty()) {
    combos.push_back(Value::object());
    for (const auto& key : keys) {
      const auto& values = matrix.object_data().find(key)->items();
      std::vector<Value> next;
      next.reserve(combos.size() * values.size());
      for (const auto& c : combos) {
        for (const auto& v : values) {
          Value copy = Value::object();
          for (const auto& [ck, cv] : c.object_data().entries()) copy.set(ck, cv);
          copy.set(key, v);
          next.push_back(std::move(copy));
        }
      }
      combos = std::move(next);
      if (combos.size() > 100000) throw MatrixError("The matrix is too large");
    }
  }

  const std::set<std::string> original(keys.begin(), keys.end());
  if (exclude.is_array()) {
    for (const auto& e : exclude.items()) {
      if (!e.is_object()) throw MatrixError("Matrix 'exclude' entries must be mappings");
      for (const auto& [k, v] : e.object_data().entries()) {
        (void)v;
        if (original.count(k) == 0) {
          throw MatrixError("Matrix exclude key '" + k + "' does not match any key within the matrix");
        }
      }
    }
    std::vector<Value> kept;
    for (auto& c : combos) {
      bool excluded = false;
      for (const auto& e : exclude.items()) {
        if (partial_match(c, e)) {
          excluded = true;
          break;
        }
      }
      if (!excluded) kept.push_back(std::move(c));
    }
    combos = std::move(kept);
  }

  if (include.is_array()) {
    const std::size_t original_count = combos.size();
    for (const auto& e : include.items()) {
      if (!e.is_object()) throw MatrixError("Matrix 'include' entries must be mappings");
      bool merged = false;
      for (std::size_t i = 0; i < original_count; ++i) {
        auto& c = combos[i];
        bool compatible = true;
        for (const auto& [k, v] : e.object_data().entries()) {
          if (original.count(k) == 0) continue;
          const Value* actual = c.object_data().find(k);
          if (actual == nullptr || !deep_equal(*actual, v)) {
            compatible = false;
            break;
          }
        }
        if (!compatible) continue;
        for (const auto& [k, v] : e.object_data().entries()) c.set(k, v);
        merged = true;
      }
      if (!merged) combos.push_back(e.deep_copy());
    }
  }

  if (combos.empty()) throw MatrixError("The matrix does not produce any job (all combinations were excluded)");
  if (combos.size() > kMaxMatrixJobs) {
    throw MatrixError("The matrix produces " + std::to_string(combos.size()) + " jobs; at most " +
                      std::to_string(kMaxMatrixJobs) + " are allowed");
  }
  return combos;
}

std::string matrix_display_suffix(const Value& combination) {
  if (!combination.is_object() || combination.object_data().empty()) return {};
  std::string s = "(";
  bool first = true;
  for (const auto& [k, v] : combination.object_data().entries()) {
    (void)k;
    if (!first) s += ", ";
    s += display_value(v);
    first = false;
  }
  return s + ")";
}

}  // namespace cpa::workflow
