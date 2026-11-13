#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace cpa::util {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t size);
  void update(std::string_view s) { update(s.data(), s.size()); }
  std::array<std::uint8_t, 32> digest();

  static std::string hex(const std::array<std::uint8_t, 32>& d);

 private:
  void transform(const std::uint8_t* block);
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t length_ = 0;
  std::size_t buffered_ = 0;
};

std::string sha256_hex(std::string_view data);
std::array<std::uint8_t, 32> sha256_file(const std::filesystem::path& path);

}  // namespace cpa::util
