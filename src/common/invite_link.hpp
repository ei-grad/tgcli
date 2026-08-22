#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tgcli::common {

inline bool valid_telegram_invite_token(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

inline std::optional<std::string_view> telegram_invite_token(std::string_view link) noexcept {
    std::optional<std::string_view> remainder;
    for (const auto prefix : {std::string_view{"https://t.me/"}, std::string_view{"t.me/"}}) {
        if (link.starts_with(prefix)) {
            remainder = link.substr(prefix.size());
            break;
        }
    }
    if (!remainder) {
        return std::nullopt;
    }
    if (remainder->starts_with('+')) {
        const auto token = remainder->substr(1);
        return valid_telegram_invite_token(token) ? std::optional{token} : std::nullopt;
    }
    constexpr std::string_view joinchat = "joinchat/";
    if (!remainder->starts_with(joinchat)) {
        return std::nullopt;
    }
    const auto token = remainder->substr(joinchat.size());
    return valid_telegram_invite_token(token) ? std::optional{token} : std::nullopt;
}

inline bool is_exact_telegram_invite_link(std::string_view link) noexcept {
    return telegram_invite_token(link).has_value();
}

inline std::vector<std::string> exact_telegram_invite_aliases(std::string_view link) {
    const auto token = telegram_invite_token(link);
    if (!token) {
        return {};
    }
    const std::array candidates{
        std::string(link), "https://t.me/+" + std::string(*token), "t.me/+" + std::string(*token),
        "https://t.me/joinchat/" + std::string(*token), "t.me/joinchat/" + std::string(*token)};
    std::vector<std::string> aliases;
    aliases.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (std::ranges::find(aliases, candidate) == aliases.end()) {
            aliases.push_back(candidate);
        }
    }
    return aliases;
}

} // namespace tgcli::common
