#include "common/sha256.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace tgcli::common {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

std::string hex(const std::array<unsigned char, 32>& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output(digest.size() * 2, '0');
    std::size_t offset = 0;
    for (const auto byte : digest) {
        output.at(offset++) = digits.at(byte >> 4U);
        output.at(offset++) = digits.at(byte & 0x0fU);
    }
    return output;
}

} // namespace

void Sha256::update(std::span<const unsigned char> bytes) {
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - total_size_) {
        throw std::length_error("SHA-256 input is too large");
    }
    total_size_ += static_cast<std::uint64_t>(bytes.size());
    while (!bytes.empty()) {
        const auto copied = std::min(bytes.size(), block_.size() - block_size_);
        std::copy_n(bytes.begin(), copied,
                    block_.begin() + static_cast<std::ptrdiff_t>(block_size_));
        block_size_ += copied;
        bytes = bytes.subspan(copied);
        if (block_size_ == block_.size()) {
            transform(block_);
            block_size_ = 0;
        }
    }
}

void Sha256::update(std::string_view bytes) {
    update(std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

std::array<unsigned char, 32> Sha256::finish() const {
    Sha256 final = *this;
    const auto bit_length = final.total_size_ * 8U;
    final.block_.at(final.block_size_++) = 0x80U;
    if (final.block_size_ > 56) {
        std::fill(final.block_.begin() + static_cast<std::ptrdiff_t>(final.block_size_),
                  final.block_.end(), 0);
        final.transform(final.block_);
        final.block_size_ = 0;
    }
    std::fill(final.block_.begin() + static_cast<std::ptrdiff_t>(final.block_size_),
              final.block_.begin() + 56, 0);
    for (std::size_t index = 0; index < 8; ++index) {
        final.block_.at(56 + index) =
            static_cast<unsigned char>(bit_length >> static_cast<unsigned>((7 - index) * 8));
    }
    final.transform(final.block_);

    std::array<unsigned char, 32> output{};
    std::size_t offset = 0;
    for (const auto word : final.digest_) {
        output.at(offset++) = static_cast<unsigned char>(word >> 24U);
        output.at(offset++) = static_cast<unsigned char>(word >> 16U);
        output.at(offset++) = static_cast<unsigned char>(word >> 8U);
        output.at(offset++) = static_cast<unsigned char>(word);
    }
    return output;
}

std::string Sha256::finish_hex() const {
    return hex(finish());
}

void Sha256::transform(const std::array<unsigned char, 64>& block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4;
        words.at(index) = (static_cast<std::uint32_t>(block.at(offset)) << 24U) |
                          (static_cast<std::uint32_t>(block.at(offset + 1)) << 16U) |
                          (static_cast<std::uint32_t>(block.at(offset + 2)) << 8U) |
                          static_cast<std::uint32_t>(block.at(offset + 3));
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const auto first = words.at(index - 15);
        const auto second = words.at(index - 2);
        const auto sigma0 = std::rotr(first, 7) ^ std::rotr(first, 18) ^ (first >> 3U);
        const auto sigma1 = std::rotr(second, 17) ^ std::rotr(second, 19) ^ (second >> 10U);
        words.at(index) = words.at(index - 16) + sigma0 + words.at(index - 7) + sigma1;
    }
    auto [a, b, c, d, e, f, g, h] = digest_;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto temporary1 = h + sum1 + choose + kRoundConstants.at(index) + words.at(index);
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    digest_[0] += a;
    digest_[1] += b;
    digest_[2] += c;
    digest_[3] += d;
    digest_[4] += e;
    digest_[5] += f;
    digest_[6] += g;
    digest_[7] += h;
}

std::string sha256_hex(std::string_view bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.finish_hex();
}

std::string sha256_string(std::string_view bytes) {
    return "sha256:" + sha256_hex(bytes);
}

std::string domain_separated_sha256(std::string_view domain, std::string_view payload) {
    if (domain.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SHA-256 domain contains its separator");
    }
    Sha256 digest;
    digest.update(domain);
    constexpr std::array<unsigned char, 1> separator{0};
    digest.update(separator);
    digest.update(payload);
    return "sha256:" + digest.finish_hex();
}

} // namespace tgcli::common
