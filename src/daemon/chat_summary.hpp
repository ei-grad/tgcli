#pragma once

#include "core/td_runtime.hpp"
#include "daemon/chat_identity.hpp"
#include "daemon/message_summary.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

struct ChatSummary {
    ChatIdentity identity;
    bool is_archived = false;
    std::vector<std::int32_t> folder_ids;
    bool is_marked_unread = false;
    std::int32_t unread_count = 0;
    std::int32_t unread_mention_count = 0;
    std::int32_t unread_reaction_count = 0;
    std::int32_t unread_poll_vote_count = 0;
    std::optional<MessageSummary> last_message;

    bool operator==(const ChatSummary&) const = default;
};

std::optional<ChatSummary> materialize_chat_summary(const core::TdChat& chat,
                                                    ChatIdentity identity);
nlohmann::json chat_summary_json(const ChatSummary& summary);

} // namespace tgcli::daemon
