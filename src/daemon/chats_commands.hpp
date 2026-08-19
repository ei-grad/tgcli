#pragma once

#include "core/td_client.hpp"
#include "daemon/dispatch.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tgcli::daemon {

inline constexpr std::int32_t kDefaultChatsLimit = 20;
inline constexpr std::int32_t kMaximumChatsLimit = 100;

struct ChatsCursor {
    std::int32_t version = 1;
    std::string operation = "chats";
    std::string account;
    std::int64_t user_id = 0;
    std::int32_t limit = kDefaultChatsLimit;
    core::TdChatListKind list = core::TdChatListKind::Main;
    std::optional<std::int32_t> folder_id;
    bool unread = false;
    std::int64_t order = 0;
    std::int64_t chat_id = 0;

    bool operator==(const ChatsCursor&) const = default;
};

std::string encode_chats_cursor(const ChatsCursor& cursor);
std::optional<ChatsCursor> decode_chats_cursor(std::string_view token);

class ChatsCoordinator {
  public:
    ChatsCoordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

    void chats(const proto::Request& request, RequestSession& session);
    void unread(const proto::Request& request, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
};

void register_chats_command(Dispatcher& dispatcher, ChatsCoordinator& coordinator);
void register_unread_command(Dispatcher& dispatcher, ChatsCoordinator& coordinator);

} // namespace tgcli::daemon
