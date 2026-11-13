#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::sandbox {

namespace fs = std::filesystem;

inline constexpr char kHostedPythonToolCache[] = "/Users/runner/hostedtoolcache";

std::string hosted_python_provisioning_hint(std::string_view version = {}, std::string_view architecture = {});
void require_disjoint_from_writable(const fs::path& path, const std::vector<fs::path>& writable,
                                    std::string_view description);
void validate_hosted_python_cache_path(const fs::path& cache);
bool ensure_hosted_python_cache(const fs::path& cache, const std::vector<fs::path>& writable);
void validate_hosted_python_cache(const fs::path& cache, const std::vector<fs::path>& writable);
std::optional<fs::path> hosted_python_installation(const fs::path& cache, const fs::path& reported);

}  // namespace cpa::sandbox
