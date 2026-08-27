#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace tgcli::frame_budget {

inline constexpr std::size_t kMaximumSerializedFrameBytes = 16'842'751;

constexpr std::size_t decimal_digits(std::uint64_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// Compact Result is {"type":"result","id":<id>,"data":<payload>}.
// The fixed syntax excluding decimal id digits and payload is exactly 31 bytes.
constexpr std::size_t maximum_result_payload_bytes(std::uint64_t request_id) noexcept {
    constexpr std::size_t fixed_result_bytes = 31;
    return kMaximumSerializedFrameBytes - fixed_result_bytes - decimal_digits(request_id);
}

inline constexpr std::size_t kMinimumResultPayloadBytes =
    maximum_result_payload_bytes(std::numeric_limits<std::uint64_t>::max());

} // namespace tgcli::frame_budget
