#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::util {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path);
std::optional<std::string> try_read_file(const fs::path& path);
void write_file(const fs::path& path, std::string_view content);
void append_file(const fs::path& path, std::string_view content);
void make_executable(const fs::path& path);

// realpath(3); falls back to a lexically normalized absolute path when the
// path does not exist.
fs::path real_path(const fs::path& path);

// Copies a single file (regular file or symlink) preserving the mode. Uses
// clonefile(2) on macOS when possible.
void copy_entry(const fs::path& from, const fs::path& to);
// Recursively copies a directory tree (symlinks are copied as symlinks).
void copy_tree(const fs::path& from, const fs::path& to);
// Removes a tree, making read-only directories writable first when needed.
void remove_tree(const fs::path& path);

std::optional<fs::path> find_executable(std::string_view name, std::string_view path_env);

fs::path home_dir();
fs::path temp_dir();
fs::path user_cache_dir();

bool is_subpath(const fs::path& path, const fs::path& base);

}  // namespace cpa::util
