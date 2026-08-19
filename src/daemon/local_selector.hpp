#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tgcli::daemon {

enum class LocalSelectorKind {
    Numeric,
    Username,
    PublicChatLink,
    BotStartLink,
    MessageLink,
    ChatInviteLink,
    DirectMessagesChatLink,
    InvalidLink,
    UnsupportedLink,
    Title,
};

struct LocalSelector {
    LocalSelectorKind kind = LocalSelectorKind::Title;
    std::int64_t chat_id = 0;
    std::string value;

    bool operator==(const LocalSelector&) const = default;
};

std::optional<LocalSelector> classify_local_selector(std::string_view selector);

} // namespace tgcli::daemon
