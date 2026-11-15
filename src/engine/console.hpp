#pragma once

#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <mutex>
#include <string>
#include <string_view>

#include "engine/masker.hpp"

namespace cpa::engine {

enum class Color { None, Red, Green, Yellow, Blue, Magenta, Cyan, Gray, Bold };

// Thread-safe line output with optional ANSI colors.
class Console {
 public:
  Console(std::ostream& out, bool color) : out_(out), color_(color) {}
  bool color() const { return color_; }
  std::string paint(std::string_view text, Color color) const;
  void line(std::string_view text);

 private:
  std::mutex mutex_;
  std::ostream& out_;
  bool color_;
};

// Log of one job: lines are masked, prefixed with the job name on the console
// and written without colors to the job log file.
class JobLog {
 public:
  JobLog(Console& console, const Masker& masker, std::string prefix, const std::filesystem::path& file, bool debug);

  void info(std::string_view text, Color color = Color::None);
  void output(std::string_view text);
  void debug(std::string_view text);
  void warning(std::string_view text);
  void error(std::string_view text);
  void notice(std::string_view text);
  void group(std::string_view title);
  void endgroup();
  bool debug_enabled() const { return debug_; }

 private:
  void write(std::string_view text, Color color, std::string_view marker);

  Console& console_;
  const Masker& masker_;
  std::string prefix_;
  std::ofstream file_;
  std::mutex mutex_;
  bool debug_;
  int depth_ = 0;
};

}  // namespace cpa::engine
