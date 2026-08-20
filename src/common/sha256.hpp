#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tgcli::common {

class Sha256 final {
  public:
    void update(std::span<const unsigned char> bytes);
    void update(std::string_view bytes);

    [[nodiscard]] std::array<unsigned char, 32> finish() const;
    [[nodiscard]] std::string finish_hex() const;

  private:
    void transform(const std::array<unsigned char, 64>& block);

    std::array<std::uint32_t, 8> digest_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                         0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block_{};
    std::size_t block_size_ = 0;
    std::uint64_t total_size_ = 0;
};

[[nodiscard]] std::string sha256_hex(std::string_view bytes);
[[nodiscard]] std::string sha256_string(std::string_view bytes);

// The domain argument excludes its separator; this function inserts exactly one NUL byte.
[[nodiscard]] std::string domain_separated_sha256(std::string_view domain,
                                                  std::string_view payload);

} // namespace tgcli::common
