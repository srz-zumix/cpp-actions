#include "engine/commands.hpp"

#include <array>

#include "util/strings.hpp"

namespace cpa::engine {

namespace {

constexpr std::array<std::string_view, 15> kCommands = {
    "set-env", "set-output", "save-state",    "add-path",  "add-mask",    "debug",
    "warning", "error",      "notice",        "group",     "endgroup",    "echo",
    "add-matcher", "remove-matcher", "stop-commands",
};

bool known_command(std::string_view name) {
  for (auto c : kCommands) {
    if (c == name) return true;
  }
  return false;
}

std::string replace_sequence(std::string text, const std::vector<std::pair<std::string_view, std::string_view>>& map) {
  for (const auto& [from, to] : map) text = util::replace_all(text, from, to);
  return text;
}

// Reads a line terminated by "\n" or "\r\n". Returns nullopt at the end.
std::optional<std::string_view> read_line(std::string_view text, std::size_t& index, std::string_view* newline) {
  if (index >= text.size()) {
    if (newline != nullptr) *newline = {};
    return std::nullopt;
  }
  const auto lf = text.find('\n', index);
  if (lf == std::string_view::npos) {
    auto line = text.substr(index);
    index = text.size();
    if (newline != nullptr) *newline = {};
    return line;
  }
  std::size_t end = lf;
  std::size_t nl_len = 1;
  if (end > index && text[end - 1] == '\r') {
    --end;
    nl_len = 2;
  }
  auto line = text.substr(index, end - index);
  if (newline != nullptr) *newline = text.substr(end, nl_len);
  index = lf + 1;
  return line;
}

}  // namespace

std::string unescape_command_data(std::string_view text) {
  return replace_sequence(std::string(text), {{"%0D", "\r"}, {"%0A", "\n"}, {"%25", "%"}});
}

std::string unescape_command_property(std::string_view text) {
  return replace_sequence(std::string(text),
                          {{"%0D", "\r"}, {"%0A", "\n"}, {"%3A", ":"}, {"%2C", ","}, {"%25", "%"}});
}

std::string escape_command_data(std::string_view text) {
  return replace_sequence(std::string(text), {{"%", "%25"}, {"\r", "%0D"}, {"\n", "%0A"}});
}

std::string escape_command_property(std::string_view text) {
  return replace_sequence(std::string(text),
                          {{"%", "%25"}, {"\r", "%0D"}, {"\n", "%0A"}, {":", "%3A"}, {",", "%2C"}});
}

std::optional<WorkflowCommand> parse_workflow_command(std::string_view line) {
  line = util::trim_left(line);
  if (line.starts_with("::")) {
    const auto end = line.find("::", 2);
    if (end == std::string_view::npos) return std::nullopt;
    const auto info = line.substr(2, end - 2);
    const auto space = info.find(' ');
    const auto name = space == std::string_view::npos ? info : info.substr(0, space);
    if (!known_command(name)) return std::nullopt;
    WorkflowCommand cmd;
    cmd.name = std::string(name);
    if (space != std::string_view::npos) {
      for (const auto& prop : util::split(util::trim(info.substr(space + 1)), ',')) {
        if (prop.empty()) continue;
        const auto eq = prop.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == prop.size()) continue;
        cmd.properties[prop.substr(0, eq)] = unescape_command_property(prop.substr(eq + 1));
      }
    }
    cmd.message = unescape_command_data(line.substr(end + 2));
    return cmd;
  }
  if (line.starts_with("##[")) {
    const auto close = line.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    const auto info = line.substr(3, close - 3);
    const auto space = info.find(' ');
    const auto name = space == std::string_view::npos ? info : info.substr(0, space);
    if (!known_command(name)) return std::nullopt;
    WorkflowCommand cmd;
    cmd.name = std::string(name);
    if (space != std::string_view::npos) {
      for (const auto& prop : util::split(util::trim(info.substr(space + 1)), ';')) {
        const auto eq = prop.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        cmd.properties[std::string(util::trim(prop.substr(0, eq)))] =
            unescape_command_property(util::trim(std::string_view(prop).substr(eq + 1)));
      }
    }
    cmd.message = unescape_command_data(line.substr(close + 1));
    return cmd;
  }
  return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> parse_key_value_file(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> out;
  std::size_t index = 0;
  while (auto line = read_line(text, index, nullptr)) {
    if (line->empty()) continue;
    const auto eq = line->find('=');
    const auto heredoc = line->find("<<");
    if (eq != std::string_view::npos && (heredoc == std::string_view::npos || eq < heredoc)) {
      const auto name = line->substr(0, eq);
      if (name.empty()) throw FileCommandError("Invalid format '" + std::string(*line) + "'. Name must not be empty");
      out.emplace_back(std::string(name), std::string(line->substr(eq + 1)));
    } else if (heredoc != std::string_view::npos && (eq == std::string_view::npos || heredoc < eq)) {
      const auto name = line->substr(0, heredoc);
      const auto delimiter = line->substr(heredoc + 2);
      if (name.empty() || delimiter.empty()) {
        throw FileCommandError("Invalid format '" + std::string(*line) +
                               "'. Name must not be empty and delimiter must not be empty");
      }
      const std::size_t start = index;
      std::size_t end = index;
      std::string_view newline;
      auto value_line = read_line(text, index, &newline);
      while (!value_line || *value_line != delimiter) {
        if (!value_line) {
          throw FileCommandError("Invalid value. Matching delimiter not found '" + std::string(delimiter) + "'");
        }
        if (newline.empty()) throw FileCommandError("Invalid value. EOF marker missing new line.");
        end = index - newline.size();
        value_line = read_line(text, index, &newline);
      }
      out.emplace_back(std::string(name), end > start ? std::string(text.substr(start, end - start)) : std::string());
    } else {
      throw FileCommandError("Invalid format '" + std::string(*line) + "'");
    }
  }
  return out;
}

std::vector<std::string> parse_path_file(std::string_view content) {
  std::vector<std::string> out;
  std::size_t index = 0;
  while (auto line = read_line(content, index, nullptr)) {
    if (util::trim(*line).empty()) continue;
    out.emplace_back(*line);
  }
  return out;
}

}  // namespace cpa::engine
