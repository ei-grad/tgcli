#include "daemon/stream_model.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

template <typename... T> struct Overloaded : T... {
    using T::operator()...;
};

template <typename... T> Overloaded(T...) -> Overloaded<T...>;

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_nonnegative_int53(std::int64_t value) {
    return value >= 0 && value <= kMaximumInt53;
}

bool valid_sender(const MessageSenderRef& sender) {
    switch (sender.kind) {
    case MessageSenderKind::User:
        return sender.id > 0 && sender.id <= kMaximumInt53;
    case MessageSenderKind::Chat:
        return valid_int53(sender.id);
    }
    return false;
}

bool valid_topic(const TopicRef& topic) {
    switch (topic.kind) {
    case TopicKind::Forum:
        return topic.id > 0 && topic.id <= std::numeric_limits<std::int32_t>::max();
    case TopicKind::Thread:
    case TopicKind::Direct:
    case TopicKind::Saved:
        return topic.id > 0 && topic.id <= kMaximumInt53;
    }
    return false;
}

bool valid_content(const MessageContent& content) {
    switch (content.type) {
    case MessageContentKind::Text:
    case MessageContentKind::Photo:
    case MessageContentKind::Video:
    case MessageContentKind::Document:
    case MessageContentKind::Voice:
    case MessageContentKind::Other:
        return common::valid_utf8(content.text);
    }
    return false;
}

std::string_view content_type_name(MessageContentKind type) {
    switch (type) {
    case MessageContentKind::Text:
        return "text";
    case MessageContentKind::Photo:
        return "photo";
    case MessageContentKind::Video:
        return "video";
    case MessageContentKind::Document:
        return "doc";
    case MessageContentKind::Voice:
        return "voice";
    case MessageContentKind::Other:
        return "other";
    }
    return "other";
}

bool valid_reaction(const ReactionRef& reaction) {
    switch (reaction.kind) {
    case ReactionKind::Emoji:
        return !reaction.emoji.empty() && common::valid_utf8(reaction.emoji) &&
               reaction.custom_emoji_id == 0;
    case ReactionKind::CustomEmoji:
        return reaction.emoji.empty() && reaction.custom_emoji_id > 0;
    case ReactionKind::Paid:
        return reaction.emoji.empty() && reaction.custom_emoji_id == 0;
    }
    return false;
}

nlohmann::json sender_json(const MessageSenderRef& sender) {
    return {{"type", sender.kind == MessageSenderKind::User ? "user" : "chat"}, {"id", sender.id}};
}

nlohmann::json reaction_json(const ReactionRef& reaction) {
    switch (reaction.kind) {
    case ReactionKind::Emoji:
        return {{"type", "emoji"}, {"emoji", reaction.emoji}};
    case ReactionKind::CustomEmoji:
        return {{"type", "custom"}, {"custom_emoji_id", std::to_string(reaction.custom_emoji_id)}};
    case ReactionKind::Paid:
        return {{"type", "paid"}};
    }
    return nlohmann::json::object();
}

std::optional<nlohmann::json> reactions_json(const std::vector<ReactionRef>& reactions) {
    auto result = nlohmann::json::array();
    for (const auto& reaction : reactions) {
        if (!valid_reaction(reaction)) {
            return std::nullopt;
        }
        result.push_back(reaction_json(reaction));
    }
    return result;
}

std::optional<nlohmann::json> snapshot_json(const ReactionSnapshot& snapshot) {
    auto items = nlohmann::json::array();
    for (const auto& item : snapshot.items) {
        if (!valid_reaction(item.reaction) || item.total_count < 0 ||
            (item.used_sender && !valid_sender(*item.used_sender)) ||
            !std::ranges::all_of(item.recent_senders, valid_sender)) {
            return std::nullopt;
        }
        auto recent_senders = nlohmann::json::array();
        for (const auto& sender : item.recent_senders) {
            recent_senders.push_back(sender_json(sender));
        }
        items.push_back({{"reaction", reaction_json(item.reaction)},
                         {"total_count", item.total_count},
                         {"is_chosen", item.is_chosen},
                         {"used_sender", item.used_sender ? sender_json(*item.used_sender)
                                                          : nlohmann::json(nullptr)},
                         {"recent_senders", std::move(recent_senders)}});
    }
    return nlohmann::json{{"items", std::move(items)},
                          {"are_tags", snapshot.are_tags},
                          {"can_get_added_reactions", snapshot.can_get_added_reactions}};
}

std::optional<nlohmann::json>
anonymous_reactions_json(const std::vector<AnonymousReaction>& reactions) {
    auto result = nlohmann::json::array();
    for (const auto& reaction : reactions) {
        if (!valid_reaction(reaction.reaction) || reaction.total_count < 0) {
            return std::nullopt;
        }
        result.push_back({{"reaction", reaction_json(reaction.reaction)},
                          {"total_count", reaction.total_count}});
    }
    return result;
}

nlohmann::json chat_list_json(const ChatListRef& list) {
    switch (list.kind) {
    case ChatListKind::Main:
        return {{"type", "main"}};
    case ChatListKind::Archive:
        return {{"type", "archive"}};
    case ChatListKind::Folder:
        return {{"type", "folder"}, {"folder_id", list.folder_id}};
    }
    return nlohmann::json::object();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed chat-change union.
std::optional<nlohmann::json> chat_change_json(const ChatChange& change) {
    return std::visit(
        Overloaded{
            [](const NewChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_chat_summary(value.chat)) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "new"},
                                      {"chat", chat_summary_json(value.chat)}};
            },
            [](const IdentityChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_chat_identity(value.chat)) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "identity"},
                                      {"chat", chat_identity_json(value.chat)}};
            },
            [](const TitleChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) || !common::valid_utf8(value.title)) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "title"},
                                      {"chat_id", value.chat_id},
                                      {"title", value.title}};
            },
            [](const LastMessageChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) ||
                    (value.last_message && (!valid_stream_message(*value.last_message) ||
                                            value.last_message->chat_id != value.chat_id))) {
                    return std::nullopt;
                }
                return nlohmann::json{
                    {"event", "chat_change"},
                    {"change", "last_message"},
                    {"chat_id", value.chat_id},
                    {"last_message", value.last_message ? message_summary_json(*value.last_message)
                                                        : nlohmann::json(nullptr)}};
            },
            [](const ListAddedChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) || !valid_chat_list(value.list)) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "list_added"},
                                      {"chat_id", value.chat_id},
                                      {"list", chat_list_json(value.list)}};
            },
            [](const ListRemovedChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) || !valid_chat_list(value.list)) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "list_removed"},
                                      {"chat_id", value.chat_id},
                                      {"list", chat_list_json(value.list)}};
            },
            [](const ReadInboxChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) ||
                    !valid_nonnegative_int53(value.last_read_inbox_message_id) ||
                    value.unread_count < 0) {
                    return std::nullopt;
                }
                return nlohmann::json{
                    {"event", "chat_change"},
                    {"change", "read_inbox"},
                    {"chat_id", value.chat_id},
                    {"last_read_inbox_message_id", value.last_read_inbox_message_id},
                    {"unread_count", value.unread_count}};
            },
            [](const UnreadMentionChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) || value.unread_mention_count < 0) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "unread_mention_count"},
                                      {"chat_id", value.chat_id},
                                      {"unread_mention_count", value.unread_mention_count}};
            },
            [](const UnreadReactionChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) || value.unread_reaction_count < 0) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "unread_reaction_count"},
                                      {"chat_id", value.chat_id},
                                      {"unread_reaction_count", value.unread_reaction_count}};
            },
            [](const UnreadPollVoteChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id) || value.unread_poll_vote_count < 0) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "unread_poll_vote_count"},
                                      {"chat_id", value.chat_id},
                                      {"unread_poll_vote_count", value.unread_poll_vote_count}};
            },
            [](const MarkedUnreadChatChange& value) -> std::optional<nlohmann::json> {
                if (!valid_int53(value.chat_id)) {
                    return std::nullopt;
                }
                return nlohmann::json{{"event", "chat_change"},
                                      {"change", "marked_unread"},
                                      {"chat_id", value.chat_id},
                                      {"is_marked_unread", value.is_marked_unread}};
            }},
        change);
}

bool read_decimal(std::string_view value, std::size_t offset, std::size_t length, int& result) {
    if (offset + length > value.size()) {
        return false;
    }
    const auto* const first = value.data() + offset;
    const auto* const last = first + length;
    if (!std::all_of(first, last,
                     [](char character) { return character >= '0' && character <= '9'; })) {
        return false;
    }
    const auto parsed = std::from_chars(first, last, result);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

} // namespace

bool valid_stream_timestamp(const std::string& timestamp) {
    if (timestamp.size() != 20 || timestamp[4] != '-' || timestamp[7] != '-' ||
        timestamp[10] != 'T' || timestamp[13] != ':' || timestamp[16] != ':' ||
        timestamp[19] != 'Z') {
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!read_decimal(timestamp, 0, 4, year) || !read_decimal(timestamp, 5, 2, month) ||
        !read_decimal(timestamp, 8, 2, day) || !read_decimal(timestamp, 11, 2, hour) ||
        !read_decimal(timestamp, 14, 2, minute) || !read_decimal(timestamp, 17, 2, second) ||
        year < 1970 || year > 2038 || month < 1 || month > 12 || hour > 23 || minute > 59 ||
        second > 59) {
        return false;
    }
    constexpr std::array<int, 12> days_per_month{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int maximum_day = days_per_month.at(static_cast<std::size_t>(month - 1)) +
                            (month == 2 && leap_year(year) ? 1 : 0);
    if (day < 1 || day > maximum_day || timestamp == "1970-01-01T00:00:00Z") {
        return false;
    }
    return timestamp <= "2038-01-19T03:14:07Z";
}

bool valid_stream_message(const MessageSummary& message) {
    return valid_int53(message.id) && valid_int53(message.chat_id) &&
           (!message.date || valid_stream_timestamp(*message.date)) &&
           valid_sender(message.sender) && (!message.topic || valid_topic(*message.topic)) &&
           valid_content({.type = message.type, .text = message.text});
}

bool valid_chat_identity(const ChatIdentity& identity) {
    const bool valid_type = identity.type == "private" || identity.type == "basic_group" ||
                            identity.type == "supergroup" || identity.type == "channel";
    return valid_int53(identity.id) && common::valid_utf8(identity.title) && valid_type &&
           (identity.type == "private" || !identity.is_bot) &&
           std::ranges::all_of(identity.usernames, [](const std::string& username) {
               return !username.empty() && common::valid_utf8(username);
           });
}

bool valid_chat_summary(const ChatSummary& summary) {
    const auto& folders = summary.folder_ids;
    return valid_chat_identity(summary.identity) && summary.unread_count >= 0 &&
           summary.unread_mention_count >= 0 && summary.unread_reaction_count >= 0 &&
           summary.unread_poll_vote_count >= 0 &&
           std::ranges::all_of(folders, [](std::int32_t id) { return id > 0; }) &&
           std::adjacent_find(folders.begin(), folders.end(),
                              [](std::int32_t left, std::int32_t right) {
                                  return left >= right;
                              }) == folders.end() &&
           (!summary.last_message || (valid_stream_message(*summary.last_message) &&
                                      summary.last_message->chat_id == summary.identity.id));
}

bool valid_chat_list(const ChatListRef& list) {
    switch (list.kind) {
    case ChatListKind::Folder:
        return list.folder_id > 0;
    case ChatListKind::Main:
    case ChatListKind::Archive:
        return list.folder_id == 0;
    }
    return false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed public event union.
std::optional<nlohmann::json> stream_event_json(const StreamEvent& event) {
    return std::visit(
        Overloaded{[](const MessageEvent& value) -> std::optional<nlohmann::json> {
                       if (!valid_stream_message(value.message)) {
                           return std::nullopt;
                       }
                       return nlohmann::json{{"event", "message"},
                                             {"message", message_summary_json(value.message)}};
                   },
                   [](const EditContentEvent& value) -> std::optional<nlohmann::json> {
                       if (!valid_int53(value.chat_id) || !valid_int53(value.message_id) ||
                           !valid_content(value.content)) {
                           return std::nullopt;
                       }
                       return nlohmann::json{{"event", "edit_content"},
                                             {"chat_id", value.chat_id},
                                             {"message_id", value.message_id},
                                             {"content",
                                              {{"type", content_type_name(value.content.type)},
                                               {"text", value.content.text}}}};
                   },
                   [](const EditMetadataEvent& value) -> std::optional<nlohmann::json> {
                       if (!valid_int53(value.chat_id) || !valid_int53(value.message_id) ||
                           (value.edit_date && !valid_stream_timestamp(*value.edit_date))) {
                           return std::nullopt;
                       }
                       return nlohmann::json{{"event", "edit_metadata"},
                                             {"chat_id", value.chat_id},
                                             {"message_id", value.message_id},
                                             {"edit_date", value.edit_date
                                                               ? nlohmann::json(*value.edit_date)
                                                               : nlohmann::json(nullptr)},
                                             {"has_reply_markup", value.has_reply_markup}};
                   },
                   [](const ReactionSnapshotEvent& value) -> std::optional<nlohmann::json> {
                       if (!valid_int53(value.chat_id) || !valid_int53(value.message_id)) {
                           return std::nullopt;
                       }
                       std::optional<nlohmann::json> reactions = nlohmann::json(nullptr);
                       if (value.reactions) {
                           reactions = snapshot_json(*value.reactions);
                           if (!reactions) {
                               return std::nullopt;
                           }
                       }
                       return nlohmann::json{{"event", "reaction_snapshot"},
                                             {"chat_id", value.chat_id},
                                             {"message_id", value.message_id},
                                             {"reactions", std::move(*reactions)}};
                   },
                   [](const BotReactionChangeEvent& value) -> std::optional<nlohmann::json> {
                       const auto old_reactions = reactions_json(value.old_reactions);
                       const auto new_reactions = reactions_json(value.new_reactions);
                       if (!valid_int53(value.chat_id) || !valid_int53(value.message_id) ||
                           !valid_sender(value.actor) || !valid_stream_timestamp(value.date) ||
                           !old_reactions || !new_reactions) {
                           return std::nullopt;
                       }
                       return nlohmann::json{{"event", "bot_reaction_change"},
                                             {"chat_id", value.chat_id},
                                             {"message_id", value.message_id},
                                             {"actor", sender_json(value.actor)},
                                             {"date", value.date},
                                             {"old_reactions", *old_reactions},
                                             {"new_reactions", *new_reactions}};
                   },
                   [](const BotReactionSnapshotEvent& value) -> std::optional<nlohmann::json> {
                       const auto reactions = anonymous_reactions_json(value.reactions);
                       if (!valid_int53(value.chat_id) || !valid_int53(value.message_id) ||
                           !valid_stream_timestamp(value.date) || !reactions) {
                           return std::nullopt;
                       }
                       return nlohmann::json{{"event", "bot_reaction_snapshot"},
                                             {"chat_id", value.chat_id},
                                             {"message_id", value.message_id},
                                             {"date", value.date},
                                             {"reactions", *reactions}};
                   },
                   [](const DeleteBatchEvent& value) -> std::optional<nlohmann::json> {
                       if (!valid_int53(value.chat_id) ||
                           !std::ranges::all_of(value.message_ids, valid_int53)) {
                           return std::nullopt;
                       }
                       return nlohmann::json{{"event", "delete_batch"},
                                             {"chat_id", value.chat_id},
                                             {"message_ids", value.message_ids},
                                             {"is_permanent", value.is_permanent},
                                             {"from_cache", value.from_cache}};
                   },
                   [](const ChatChangeEvent& value) { return chat_change_json(value.change); }},
        event);
}

std::optional<std::string> stream_event_line(const StreamEvent& event) {
    const auto value = stream_event_json(event);
    if (!value) {
        return std::nullopt;
    }
    return value->dump() + '\n';
}

} // namespace tgcli::daemon
