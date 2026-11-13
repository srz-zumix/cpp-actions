#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::util {

// Workflow filter patterns (on.<event>.branches / tags / paths):
//   *  any characters except '/'      ** any characters
//   ?  zero or one of the preceding   +  one or more of the preceding
//   [] character class                \  escape
bool filter_match(std::string_view pattern, std::string_view value);

// Evaluates an ordered list of filter patterns where patterns starting with
// '!' exclude previously matched values. The last matching pattern wins.
bool filter_list_match(const std::vector<std::string>& patterns, std::string_view value);

// minimatch-like glob (as used by @actions/glob for hashFiles):
//   * and ? do not cross '/', ** matches any number of path segments,
//   [] character class, dot files are matched.
bool glob_match(std::string_view pattern, std::string_view path);

// Match one glob segment as a whole string, without treating '/' specially.
bool glob_match_string(std::string_view pattern, std::string_view value);

// Returns regular files below `root` matching `patterns` (patterns starting
// with '!' exclude). Paths are returned sorted.
std::vector<std::filesystem::path> glob_files(const std::filesystem::path& root,
                                              const std::vector<std::string>& patterns);

}  // namespace cpa::util
