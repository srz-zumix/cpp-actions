#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "expr/value.hpp"

namespace cpa::yaml {

// 1-based source position (0 = unknown).
struct Mark {
  int line = 0;
  int column = 0;
};

enum class NodeKind { Null, Boolean, Number, String, Sequence, Mapping };

struct Node;

struct Entry {
  const Node* key;
  const Node* value;
};

// Immutable YAML node. Scalars are resolved with the YAML 1.2 core schema
// (plain scalars only), like the Actions workflow parser.
struct Node {
  NodeKind kind = NodeKind::Null;
  Mark mark;
  bool boolean = false;
  double number = 0.0;
  std::string text;  // scalar source text (the string value for strings)
  bool quoted = false;
  std::vector<const Node*> items;
  std::vector<Entry> entries;

  bool is_null() const { return kind == NodeKind::Null; }
  bool is_scalar() const { return kind != NodeKind::Sequence && kind != NodeKind::Mapping; }
  bool is_string() const { return kind == NodeKind::String; }
  bool is_sequence() const { return kind == NodeKind::Sequence; }
  bool is_mapping() const { return kind == NodeKind::Mapping; }

  // Mapping lookup by (case-sensitive) key.
  const Node* get(std::string_view key) const;
  // Scalar converted to a string like the runner does for "loose" string
  // values: numbers use the G15 format, booleans "true"/"false", null "".
  std::string scalar_string() const;
  const char* kind_name() const;
};

class Document {
 public:
  std::string file;
  const Node* root = nullptr;

  Node* new_node() { return &nodes_.emplace_back(); }

 private:
  std::deque<Node> nodes_;
};

class LoadError : public std::runtime_error {
 public:
  LoadError(std::string file, Mark mark, std::string message);
  const std::string& file() const { return file_; }
  const Mark& mark() const { return mark_; }
  const std::string& message() const { return message_; }

 private:
  std::string file_;
  Mark mark_;
  std::string message_;
};

std::shared_ptr<Document> load(std::string_view text, std::string file);
std::shared_ptr<Document> load_file(const std::filesystem::path& path, std::string display_name = {});

// Converts a node to an expression value without evaluating expressions.
expr::Value to_value(const Node& node);

}  // namespace cpa::yaml
