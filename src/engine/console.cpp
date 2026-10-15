#include "engine/console.hpp"

#include <ostream>

#include "util/strings.hpp"

namespace cpa::engine {

namespace {

std::string_view code(Color c) {
  switch (c) {
    case Color::Red:
      return "\x1b[31m";
    case Color::Green:
      return "\x1b[32m";
    case Color::Yellow:
      return "\x1b[33m";
    case Color::Blue:
      return "\x1b[34m";
    case Color::Magenta:
      return "\x1b[35m";
    case Color::Cyan:
      return "\x1b[36m";
    case Color::Gray:
      return "\x1b[90m";
    case Color::Bold:
      return "\x1b[1m";
    case Color::None:
      break;
  }
  return {};
}

}  // namespace

std::string Console::paint(std::string_view text, Color color) const {
  if (!color_ || color == Color::None) return std::string(text);
  return std::string(code(color)) + std::string(text) + "\x1b[0m";
}

void Console::line(std::string_view text) {
  std::lock_guard lock(mutex_);
  out_ << text << '\n';
  out_.flush();
}

JobLog::JobLog(Console& console, const Masker& masker, std::string prefix, const std::filesystem::path& file,
               bool debug)
    : console_(console), masker_(masker), prefix_(std::move(prefix)), debug_(debug) {
  if (!file.empty()) {
    std::filesystem::create_directories(file.parent_path());
    file_.open(file, std::ios::out | std::ios::app | std::ios::binary);
  }
}

void JobLog::write(std::string_view text, Color color, std::string_view marker) {
  const auto masked = masker_.mask(text);
  std::lock_guard lock(mutex_);
  const std::string indent(static_cast<std::size_t>(depth_) * 2, ' ');
  auto lines = util::split_lines(masked);
  if (lines.empty()) lines.emplace_back();
  for (const auto& l : lines) {
    std::string body = indent;
    if (!marker.empty()) body += std::string(marker) + " ";
    body += l;
    console_.line(console_.paint("[" + prefix_ + "]", Color::Gray) + " " + console_.paint(body, color));
    if (file_) file_ << body << '\n';
  }
  if (file_) file_.flush();
}

void JobLog::info(std::string_view text, Color color) { write(text, color, {}); }
void JobLog::output(std::string_view text) { write(text, Color::None, "|"); }
void JobLog::debug(std::string_view text) {
  if (debug_) write(text, Color::Magenta, "##[debug]");
}
void JobLog::warning(std::string_view text) { write(text, Color::Yellow, "Warning:"); }
void JobLog::error(std::string_view text) { write(text, Color::Red, "Error:"); }
void JobLog::notice(std::string_view text) { write(text, Color::Cyan, "Notice:"); }

void JobLog::group(std::string_view title) {
  write(title, Color::Bold, ">");
  std::lock_guard lock(mutex_);
  ++depth_;
}

void JobLog::endgroup() {
  std::lock_guard lock(mutex_);
  if (depth_ > 0) --depth_;
}

}  // namespace cpa::engine
