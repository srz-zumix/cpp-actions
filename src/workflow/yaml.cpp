#include "workflow/yaml.hpp"

#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/mark.h>
#include <yaml-cpp/parser.h>

#include "util/fs.hpp"
#include "util/strings.hpp"

namespace cpa::yaml {

LoadError::LoadError(std::string file, Mark mark, std::string message)
    : std::runtime_error(file + ":" + std::to_string(mark.line) + ":" + std::to_string(mark.column) + ": " + message),
      file_(std::move(file)),
      mark_(mark),
      message_(std::move(message)) {}

const Node* Node::get(std::string_view key) const {
  if (kind != NodeKind::Mapping) return nullptr;
  for (const auto& e : entries) {
    if (e.key->is_scalar() && e.key->text == key) return e.value;
  }
  return nullptr;
}

std::string Node::scalar_string() const {
  switch (kind) {
    case NodeKind::Null: return std::string();
    case NodeKind::Boolean: return boolean ? "true" : "false";
    case NodeKind::Number: return util::format_number(number);
    case NodeKind::String: return text;
    default: return std::string();
  }
}

const char* Node::kind_name() const {
  switch (kind) {
    case NodeKind::Null: return "null";
    case NodeKind::Boolean: return "boolean";
    case NodeKind::Number: return "number";
    case NodeKind::String: return "string";
    case NodeKind::Sequence: return "sequence";
    case NodeKind::Mapping: return "mapping";
  }
  return "unknown";
}

namespace {

bool all_of(std::string_view s, bool (*pred)(char)) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!pred(c)) return false;
  }
  return true;
}

bool is_dec(char c) { return c >= '0' && c <= '9'; }
bool is_oct(char c) { return c >= '0' && c <= '7'; }
bool is_hex(char c) { return is_dec(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// [-+]? ( \. [0-9]+ | [0-9]+ ( \. [0-9]* )? ) ( [eE] [-+]? [0-9]+ )?
bool is_core_float(std::string_view s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  std::size_t int_digits = 0;
  while (i < s.size() && is_dec(s[i])) ++i, ++int_digits;
  std::size_t frac_digits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && is_dec(s[i])) ++i, ++frac_digits;
    if (int_digits == 0 && frac_digits == 0) return false;
  } else if (int_digits == 0) {
    return false;
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    std::size_t exp_digits = 0;
    while (i < s.size() && is_dec(s[i])) ++i, ++exp_digits;
    if (exp_digits == 0) return false;
  }
  return i == s.size();
}

void resolve_plain(Node& n, const std::string& text) {
  n.text = text;
  if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL") {
    n.kind = NodeKind::Null;
    return;
  }
  if (text == "true" || text == "True" || text == "TRUE") {
    n.kind = NodeKind::Boolean;
    n.boolean = true;
    return;
  }
  if (text == "false" || text == "False" || text == "FALSE") {
    n.kind = NodeKind::Boolean;
    n.boolean = false;
    return;
  }
  std::string_view s(text);
  if (s.size() > 2 && s[0] == '0' && s[1] == 'x' && all_of(s.substr(2), is_hex)) {
    n.kind = NodeKind::Number;
    n.number = static_cast<double>(std::strtoull(text.c_str() + 2, nullptr, 16));
    return;
  }
  if (s.size() > 2 && s[0] == '0' && s[1] == 'o' && all_of(s.substr(2), is_oct)) {
    n.kind = NodeKind::Number;
    n.number = static_cast<double>(std::strtoull(text.c_str() + 2, nullptr, 8));
    return;
  }
  if (is_core_float(s)) {
    n.kind = NodeKind::Number;
    n.number = std::strtod(text.c_str(), nullptr);
    return;
  }
  auto body = s;
  bool negative = false;
  if (!body.empty() && (body[0] == '-' || body[0] == '+')) {
    negative = body[0] == '-';
    body.remove_prefix(1);
  }
  if (body == ".inf" || body == ".Inf" || body == ".INF") {
    n.kind = NodeKind::Number;
    n.number = negative ? -HUGE_VAL : HUGE_VAL;
    return;
  }
  if (s == ".nan" || s == ".NaN" || s == ".NAN") {
    n.kind = NodeKind::Number;
    n.number = std::nan("");
    return;
  }
  n.kind = NodeKind::String;
}

Mark to_mark(const YAML::Mark& m) {
  if (m.is_null()) return Mark{};
  return Mark{m.line + 1, m.column + 1};
}

class Builder : public YAML::EventHandler {
 public:
  explicit Builder(Document& doc) : doc_(doc) {}

  void OnDocumentStart(const YAML::Mark&) override {}
  void OnDocumentEnd() override {}

  void OnNull(const YAML::Mark& mark, YAML::anchor_t anchor) override {
    Node* n = doc_.new_node();
    n->kind = NodeKind::Null;
    n->mark = to_mark(mark);
    remember(anchor, n);
    add(n);
  }

  void OnAlias(const YAML::Mark& mark, YAML::anchor_t anchor) override {
    const auto it = anchors_.find(anchor);
    if (it == anchors_.end()) throw LoadError(doc_.file, to_mark(mark), "unknown alias");
    add(it->second);
  }

  void OnScalar(const YAML::Mark& mark, const std::string& tag, YAML::anchor_t anchor,
                const std::string& value) override {
    Node* n = doc_.new_node();
    n->mark = to_mark(mark);
    if (tag == "?") {
      resolve_plain(*n, value);
    } else if (tag == "!" || tag == "tag:yaml.org,2002:str") {
      n->kind = NodeKind::String;
      n->text = value;
      n->quoted = tag == "!";
    } else if (tag == "tag:yaml.org,2002:null") {
      n->kind = NodeKind::Null;
      n->text = value;
    } else if (tag == "tag:yaml.org,2002:bool" || tag == "tag:yaml.org,2002:int" ||
               tag == "tag:yaml.org,2002:float") {
      resolve_plain(*n, value);
      const bool ok = (tag == "tag:yaml.org,2002:bool" && n->kind == NodeKind::Boolean) ||
                      (tag != "tag:yaml.org,2002:bool" && n->kind == NodeKind::Number);
      if (!ok) throw LoadError(doc_.file, n->mark, "The value '" + value + "' is not valid for tag '" + tag + "'");
    } else {
      throw LoadError(doc_.file, n->mark, "Unexpected tag '" + tag + "'");
    }
    remember(anchor, n);
    add(n);
  }

  void OnSequenceStart(const YAML::Mark& mark, const std::string&, YAML::anchor_t anchor,
                       YAML::EmitterStyle::value) override {
    Node* n = doc_.new_node();
    n->kind = NodeKind::Sequence;
    n->mark = to_mark(mark);
    remember(anchor, n);
    add(n);
    stack_.push_back({n, nullptr});
  }

  void OnSequenceEnd() override { stack_.pop_back(); }

  void OnMapStart(const YAML::Mark& mark, const std::string&, YAML::anchor_t anchor,
                  YAML::EmitterStyle::value) override {
    Node* n = doc_.new_node();
    n->kind = NodeKind::Mapping;
    n->mark = to_mark(mark);
    remember(anchor, n);
    add(n);
    stack_.push_back({n, nullptr});
  }

  void OnMapEnd() override { stack_.pop_back(); }

 private:
  struct Frame {
    Node* node;
    const Node* pending_key;
  };

  void remember(YAML::anchor_t anchor, const Node* n) {
    if (anchor != YAML::NullAnchor) anchors_[anchor] = n;
  }

  void add(const Node* n) {
    if (stack_.empty()) {
      if (!doc_.root) doc_.root = n;
      return;
    }
    Frame& top = stack_.back();
    if (top.node->kind == NodeKind::Sequence) {
      top.node->items.push_back(n);
      return;
    }
    if (!top.pending_key) {
      if (!n->is_scalar()) throw LoadError(doc_.file, n->mark, "A mapping key must be a scalar");
      for (const auto& e : top.node->entries) {
        if (e.key->text == n->text) {
          throw LoadError(doc_.file, n->mark, "'" + n->text + "' is already defined");
        }
      }
      top.pending_key = n;
      return;
    }
    top.node->entries.push_back({top.pending_key, n});
    top.pending_key = nullptr;
  }

  Document& doc_;
  std::vector<Frame> stack_;
  std::map<YAML::anchor_t, const Node*> anchors_;
};

}  // namespace

std::shared_ptr<Document> load(std::string_view text, std::string file) {
  auto doc = std::make_shared<Document>();
  doc->file = std::move(file);
  std::istringstream in{std::string(text)};
  try {
    YAML::Parser parser(in);
    Builder builder(*doc);
    parser.HandleNextDocument(builder);
  } catch (const YAML::Exception& e) {
    throw LoadError(doc->file, to_mark(e.mark), e.msg);
  }
  if (!doc->root) {
    Node* n = doc->new_node();
    n->kind = NodeKind::Null;
    n->mark = Mark{1, 1};
    doc->root = n;
  }
  return doc;
}

std::shared_ptr<Document> load_file(const std::filesystem::path& path, std::string display_name) {
  const auto text = util::read_file(path);
  return load(text, display_name.empty() ? path.string() : std::move(display_name));
}

expr::Value to_value(const Node& node) {
  switch (node.kind) {
    case NodeKind::Null: return expr::Value();
    case NodeKind::Boolean: return expr::Value(node.boolean);
    case NodeKind::Number: return expr::Value(node.number);
    case NodeKind::String: return expr::Value(node.text);
    case NodeKind::Sequence: {
      auto v = expr::Value::array();
      for (const auto* item : node.items) v.push(to_value(*item));
      return v;
    }
    case NodeKind::Mapping: {
      auto v = expr::Value::object();
      for (const auto& e : node.entries) v.set(e.key->text, to_value(*e.value));
      return v;
    }
  }
  return expr::Value();
}

}  // namespace cpa::yaml
