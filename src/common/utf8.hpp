#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tgcli::common {

inline bool valid_utf8(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7F) {
            length = 1;
            codepoint = first;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return false;
        }
        index += length;
    }
    return true;
}

} // namespace tgcli::common
