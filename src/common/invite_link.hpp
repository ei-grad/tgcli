#pragma once

#include "common/secure_wipe.hpp"

#include <algorithm>
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

inline std::vector<secure::SensitiveString>
exact_telegram_invite_aliases(std::string_view link,
                              const secure::WipeObserver& wipe_observer = {}) {
    const auto token = telegram_invite_token(link);
    if (!token) {
        return {};
    }
    std::vector<secure::SensitiveString> candidates;
    candidates.reserve(5);
    candidates.emplace_back(std::string(link), wipe_observer, "invite_alias");
    for (const auto prefix :
         {std::string_view{"https://t.me/+"}, std::string_view{"t.me/+"},
          std::string_view{"https://t.me/joinchat/"}, std::string_view{"t.me/joinchat/"}}) {
        std::string candidate(prefix);
        candidate.append(*token);
        candidates.emplace_back(std::move(candidate), wipe_observer, "invite_alias");
    }

    std::vector<secure::SensitiveString> aliases;
    aliases.reserve(candidates.size());
    for (auto& candidate : candidates) {
        if (std::ranges::none_of(aliases, [&](const auto& retained) {
                return retained.view() == candidate.view();
            })) {
            aliases.push_back(std::move(candidate));
        }
    }
    return aliases;
}

} // namespace tgcli::common
