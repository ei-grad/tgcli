#pragma once

#include "core/td_client.hpp"
#include "daemon/ready_read.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

struct ChatIdentity {
    std::int64_t id = 0;
    std::string title;
    std::string type;
    bool is_bot = false;
    std::vector<std::string> usernames;

    bool operator==(const ChatIdentity&) const = default;
};

enum class ChatIdentityStatus { Success, Secret, Invalid, TdError, ReadStopped };

struct ChatIdentityResult {
    ChatIdentityStatus status = ChatIdentityStatus::Invalid;
    std::optional<ChatIdentity> identity;
    std::optional<core::TdError> error;
    std::optional<std::int64_t> private_user_id;
    std::optional<core::TdUserPresence> private_user_presence;
};

using ChatIdentityRead = std::function<std::optional<ReadyReadResult>(const ReadyReadStart& start)>;

ChatIdentityResult materialize_chat_identity(core::TdClient& client, const core::TdChat& chat,
                                             const ChatIdentityRead& read);
nlohmann::json chat_identity_json(const ChatIdentity& identity);
bool persistable_chat_identity(const ChatIdentity& identity);

} // namespace tgcli::daemon
