#include "daemon/chat_summary.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_list(const core::TdChatList& list) {
    switch (list.kind) {
    case core::TdChatListKind::Main:
    case core::TdChatListKind::Archive:
        return list.folder_id == 0;
    case core::TdChatListKind::Folder:
        return list.folder_id > 0;
    case core::TdChatListKind::Unknown:
        return false;
    }
    return false;
}

std::optional<std::string_view> chat_type_name(core::TdChatKind kind) {
    switch (kind) {
    case core::TdChatKind::Private:
        return "private";
    case core::TdChatKind::BasicGroup:
        return "basic_group";
    case core::TdChatKind::Supergroup:
        return "supergroup";
    case core::TdChatKind::Channel:
        return "channel";
    case core::TdChatKind::Secret:
    case core::TdChatKind::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

bool valid_identity(const core::TdChat& chat, const ChatIdentity& identity,
                    std::string_view expected_type) {
    if (!valid_int53(chat.id) || !valid_int53(identity.id) || identity.id != chat.id ||
        identity.title != chat.title || identity.type != expected_type ||
        !common::valid_utf8(identity.title) || (identity.type != "private" && identity.is_bot)) {
        return false;
    }
    return std::ranges::all_of(identity.usernames, [](const std::string& username) {
        return !username.empty() && common::valid_utf8(username);
    });
}

} // namespace

std::optional<ChatSummary> materialize_chat_summary(const core::TdChat& chat,
                                                    ChatIdentity identity) {
    const auto type = chat_type_name(chat.kind);
    if (!type || !valid_identity(chat, identity, *type) || chat.unread_count < 0 ||
        chat.unread_mention_count < 0 || chat.unread_reaction_count < 0 ||
        chat.unread_poll_vote_count < 0) {
        return std::nullopt;
    }

    bool is_archived = false;
    std::vector<std::int32_t> folder_ids;
    for (const auto& list : chat.chat_lists) {
        if (!valid_list(list)) {
            return std::nullopt;
        }
        is_archived = is_archived || list.kind == core::TdChatListKind::Archive;
        if (list.kind == core::TdChatListKind::Folder) {
            folder_ids.push_back(list.folder_id);
        }
    }
    std::ranges::sort(folder_ids);
    folder_ids.erase(std::unique(folder_ids.begin(), folder_ids.end()), folder_ids.end());

    std::optional<MessageSummary> last_message;
    if (chat.last_message) {
        last_message = materialize_message_summary(*chat.last_message);
        if (!last_message || last_message->chat_id != chat.id) {
            return std::nullopt;
        }
    }

    return ChatSummary{.identity = std::move(identity),
                       .is_archived = is_archived,
                       .folder_ids = std::move(folder_ids),
                       .is_marked_unread = chat.is_marked_unread,
                       .unread_count = chat.unread_count,
                       .unread_mention_count = chat.unread_mention_count,
                       .unread_reaction_count = chat.unread_reaction_count,
                       .unread_poll_vote_count = chat.unread_poll_vote_count,
                       .last_message = std::move(last_message)};
}

nlohmann::json chat_summary_json(const ChatSummary& summary) {
    auto result = chat_identity_json(summary.identity);
    result.update(
        {{"is_archived", summary.is_archived},
         {"folder_ids", summary.folder_ids},
         {"is_marked_unread", summary.is_marked_unread},
         {"unread_count", summary.unread_count},
         {"unread_mention_count", summary.unread_mention_count},
         {"unread_reaction_count", summary.unread_reaction_count},
         {"unread_poll_vote_count", summary.unread_poll_vote_count},
         {"last_message", summary.last_message ? message_summary_json(*summary.last_message)
                                               : nlohmann::json(nullptr)}});
    return result;
}

} // namespace tgcli::daemon
