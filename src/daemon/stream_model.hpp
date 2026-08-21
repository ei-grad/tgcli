#pragma once

#include "daemon/chat_summary.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

enum class ReactionKind { Emoji, CustomEmoji, Paid };

struct ReactionRef {
    ReactionKind kind = ReactionKind::Emoji;
    std::string emoji;
    std::int64_t custom_emoji_id = 0;

    bool operator==(const ReactionRef&) const = default;
};

struct ReactionSnapshotItem {
    ReactionRef reaction;
    std::int32_t total_count = 0;
    bool is_chosen = false;
    std::optional<MessageSenderRef> used_sender;
    std::vector<MessageSenderRef> recent_senders;

    bool operator==(const ReactionSnapshotItem&) const = default;
};

struct ReactionSnapshot {
    std::vector<ReactionSnapshotItem> items;
    bool are_tags = false;
    bool can_get_added_reactions = false;

    bool operator==(const ReactionSnapshot&) const = default;
};

struct AnonymousReaction {
    ReactionRef reaction;
    std::int32_t total_count = 0;

    bool operator==(const AnonymousReaction&) const = default;
};

enum class ChatListKind { Main, Archive, Folder };

struct ChatListRef {
    ChatListKind kind = ChatListKind::Main;
    std::int32_t folder_id = 0;

    bool operator==(const ChatListRef&) const = default;
};

struct MessageContent {
    MessageContentKind type = MessageContentKind::Other;
    std::string text;

    bool operator==(const MessageContent&) const = default;
};

struct MessageEvent {
    MessageSummary message;
};

struct EditContentEvent {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    MessageContent content;
};

struct EditMetadataEvent {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::optional<std::string> edit_date;
    bool has_reply_markup = false;
};

struct ReactionSnapshotEvent {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::optional<ReactionSnapshot> reactions;
};

struct BotReactionChangeEvent {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    MessageSenderRef actor;
    std::string date;
    std::vector<ReactionRef> old_reactions;
    std::vector<ReactionRef> new_reactions;
};

struct BotReactionSnapshotEvent {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::string date;
    std::vector<AnonymousReaction> reactions;
};

struct DeleteBatchEvent {
    std::int64_t chat_id = 0;
    std::vector<std::int64_t> message_ids;
    bool is_permanent = false;
    bool from_cache = false;
};

struct NewChatChange {
    ChatSummary chat;
};

struct IdentityChatChange {
    ChatIdentity chat;
};

struct TitleChatChange {
    std::int64_t chat_id = 0;
    std::string title;
};

struct LastMessageChatChange {
    std::int64_t chat_id = 0;
    std::optional<MessageSummary> last_message;
};

struct ListAddedChatChange {
    std::int64_t chat_id = 0;
    ChatListRef list;
};

struct ListRemovedChatChange {
    std::int64_t chat_id = 0;
    ChatListRef list;
};

struct ReadInboxChatChange {
    std::int64_t chat_id = 0;
    std::int64_t last_read_inbox_message_id = 0;
    std::int32_t unread_count = 0;
};

struct UnreadMentionChatChange {
    std::int64_t chat_id = 0;
    std::int32_t unread_mention_count = 0;
};

struct UnreadReactionChatChange {
    std::int64_t chat_id = 0;
    std::int32_t unread_reaction_count = 0;
};

struct UnreadPollVoteChatChange {
    std::int64_t chat_id = 0;
    std::int32_t unread_poll_vote_count = 0;
};

struct MarkedUnreadChatChange {
    std::int64_t chat_id = 0;
    bool is_marked_unread = false;
};

using ChatChange =
    std::variant<NewChatChange, IdentityChatChange, TitleChatChange, LastMessageChatChange,
                 ListAddedChatChange, ListRemovedChatChange, ReadInboxChatChange,
                 UnreadMentionChatChange, UnreadReactionChatChange, UnreadPollVoteChatChange,
                 MarkedUnreadChatChange>;

struct ChatChangeEvent {
    ChatChange change;
};

using StreamEvent = std::variant<MessageEvent, EditContentEvent, EditMetadataEvent,
                                 ReactionSnapshotEvent, BotReactionChangeEvent,
                                 BotReactionSnapshotEvent, DeleteBatchEvent, ChatChangeEvent>;

bool valid_stream_timestamp(const std::string& timestamp);
bool valid_stream_message(const MessageSummary& message);
bool valid_chat_identity(const ChatIdentity& identity);
bool valid_chat_summary(const ChatSummary& summary);
bool valid_chat_list(const ChatListRef& list);
std::optional<nlohmann::json> stream_event_json(const StreamEvent& event);
std::optional<std::string> stream_event_line(const StreamEvent& event);

} // namespace tgcli::daemon
