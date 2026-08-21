#include "daemon/stream_model.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

namespace {

using namespace tgcli::daemon;

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

MessageSummary message(std::int64_t chat_id = -1001) {
    return {.id = 123,
            .chat_id = chat_id,
            .date = "2026-08-05T10:00:00Z",
            .sender = {.kind = MessageSenderKind::User, .id = 42},
            .is_outgoing = false,
            .topic = TopicRef{.kind = TopicKind::Forum, .id = 7},
            .type = MessageContentKind::Text,
            .text = "experiment result"};
}

ChatIdentity identity(std::string title = "Project") {
    return {.id = -1001,
            .title = std::move(title),
            .type = "supergroup",
            .is_bot = false,
            .usernames = {"project"}};
}

ChatSummary summary() {
    return {.identity = identity(),
            .is_archived = true,
            .folder_ids = {2, 3},
            .is_marked_unread = true,
            .unread_count = 3,
            .unread_mention_count = 1,
            .unread_reaction_count = 2,
            .unread_poll_vote_count = 4,
            .last_message = message()};
}

nlohmann::json checked_event(const StreamEvent& event) {
    const auto value = stream_event_json(event);
    REQUIRE(value);
    CHECK_THAT(*value, tgcli::test::matches_json_schema("listen.item.schema.json"));
    const auto line = stream_event_line(event);
    REQUIRE(line);
    CHECK(*line == value->dump() + '\n');
    return *value;
}

} // namespace

TEST_CASE("stream event model emits every closed schema branch", "[stream][model][schema]") {
    std::vector<StreamEvent> events;
    events.emplace_back(MessageEvent{message()});
    events.emplace_back(
        EditContentEvent{.chat_id = -1001,
                         .message_id = 123,
                         .content = {.type = MessageContentKind::Photo, .text = "replacement"}});
    events.emplace_back(EditMetadataEvent{
        .chat_id = -1001, .message_id = 123, .edit_date = std::nullopt, .has_reply_markup = false});
    events.emplace_back(ReactionSnapshotEvent{
        .chat_id = -1001,
        .message_id = 123,
        .reactions = ReactionSnapshot{
            .items = {{.reaction = {.kind = ReactionKind::Emoji, .emoji = "🧪"},
                       .total_count = 3,
                       .is_chosen = true,
                       .used_sender = MessageSenderRef{.kind = MessageSenderKind::User, .id = 42},
                       .recent_senders = {{.kind = MessageSenderKind::Chat, .id = -1002}}},
                      {.reaction = {.kind = ReactionKind::CustomEmoji,
                                    .emoji = "",
                                    .custom_emoji_id = std::numeric_limits<std::int64_t>::max()},
                       .total_count = 2,
                       .is_chosen = false,
                       .used_sender = std::nullopt,
                       .recent_senders = {}},
                      {.reaction = {.kind = ReactionKind::Paid, .emoji = "", .custom_emoji_id = 0},
                       .total_count = 1,
                       .is_chosen = false,
                       .used_sender = std::nullopt,
                       .recent_senders = {}}},
            .are_tags = false,
            .can_get_added_reactions = true}});
    events.emplace_back(BotReactionChangeEvent{
        .chat_id = -1001,
        .message_id = 123,
        .actor = {.kind = MessageSenderKind::Chat, .id = -1002},
        .date = "2026-08-05T10:00:00Z",
        .old_reactions = {{.kind = ReactionKind::Emoji, .emoji = "👍"},
                          {.kind = ReactionKind::Paid, .emoji = "", .custom_emoji_id = 0}},
        .new_reactions = {
            {.kind = ReactionKind::CustomEmoji, .emoji = "", .custom_emoji_id = 123456789}}});
    events.emplace_back(BotReactionSnapshotEvent{
        .chat_id = -1001,
        .message_id = 123,
        .date = "2026-08-05T10:00:00Z",
        .reactions = {
            {.reaction = {.kind = ReactionKind::Emoji, .emoji = "🧪"}, .total_count = 3}}});
    events.emplace_back(DeleteBatchEvent{
        .chat_id = -1001, .message_ids = {123, 124}, .is_permanent = true, .from_cache = false});
    events.emplace_back(ChatChangeEvent{NewChatChange{summary()}});
    events.emplace_back(ChatChangeEvent{IdentityChatChange{identity("Renamed")}});
    events.emplace_back(ChatChangeEvent{TitleChatChange{.chat_id = -1001, .title = "New title"}});
    events.emplace_back(
        ChatChangeEvent{LastMessageChatChange{.chat_id = -1001, .last_message = std::nullopt}});
    events.emplace_back(ChatChangeEvent{ListAddedChatChange{
        .chat_id = -1001, .list = {.kind = ChatListKind::Main, .folder_id = 0}}});
    events.emplace_back(ChatChangeEvent{ListRemovedChatChange{
        .chat_id = -1001, .list = {.kind = ChatListKind::Archive, .folder_id = 0}}});
    events.emplace_back(ChatChangeEvent{ListAddedChatChange{
        .chat_id = -1001, .list = {.kind = ChatListKind::Folder, .folder_id = 2}}});
    events.emplace_back(ChatChangeEvent{ReadInboxChatChange{
        .chat_id = -1001, .last_read_inbox_message_id = 123, .unread_count = 2}});
    events.emplace_back(
        ChatChangeEvent{UnreadMentionChatChange{.chat_id = -1001, .unread_mention_count = 1}});
    events.emplace_back(
        ChatChangeEvent{UnreadReactionChatChange{.chat_id = -1001, .unread_reaction_count = 1}});
    events.emplace_back(
        ChatChangeEvent{UnreadPollVoteChatChange{.chat_id = -1001, .unread_poll_vote_count = 1}});
    events.emplace_back(
        ChatChangeEvent{MarkedUnreadChatChange{.chat_id = -1001, .is_marked_unread = true}});

    for (const auto& event : events) {
        checked_event(event);
    }
    const auto wait_result = checked_event(events.front()).at("message");
    CHECK_THAT(wait_result, tgcli::test::matches_json_schema("wait-for.result.schema.json"));

    const auto reaction_snapshot = checked_event(events[3]);
    CHECK((reaction_snapshot.at("reactions").at("items").at(1).at("reaction") ==
           nlohmann::json{{"type", "custom"}, {"custom_emoji_id", "9223372036854775807"}}));
    CHECK((reaction_snapshot.at("reactions").at("items").at(2).at("reaction") ==
           nlohmann::json{{"type", "paid"}}));
}

TEST_CASE("stream event model covers nullable and tagged DTO alternatives",
          "[stream][model][schema]") {
    constexpr std::array<std::pair<MessageContentKind, std::string_view>, 6> content_types{{
        {MessageContentKind::Text, "text"},
        {MessageContentKind::Photo, "photo"},
        {MessageContentKind::Video, "video"},
        {MessageContentKind::Document, "doc"},
        {MessageContentKind::Voice, "voice"},
        {MessageContentKind::Other, "other"},
    }};
    for (const auto& [kind, name] : content_types) {
        const auto value = checked_event(StreamEvent{EditContentEvent{
            .chat_id = -1001, .message_id = 123, .content = {.type = kind, .text = "value"}}});
        CHECK(value.at("content").at("type") == name);
    }

    auto value = checked_event(StreamEvent{EditMetadataEvent{.chat_id = -1001,
                                                             .message_id = 123,
                                                             .edit_date = "2026-08-05T10:00:00Z",
                                                             .has_reply_markup = true}});
    CHECK(value.at("edit_date") == "2026-08-05T10:00:00Z");
    value = checked_event(StreamEvent{
        ReactionSnapshotEvent{.chat_id = -1001, .message_id = 123, .reactions = std::nullopt}});
    CHECK(value.at("reactions").is_null());
    value = checked_event(StreamEvent{
        ChatChangeEvent{LastMessageChatChange{.chat_id = -1001, .last_message = message()}}});
    CHECK(value.at("last_message").at("id") == 123);

    auto private_summary = summary();
    private_summary.identity.type = "private";
    private_summary.identity.is_bot = true;
    value = checked_event(StreamEvent{ChatChangeEvent{NewChatChange{std::move(private_summary)}}});
    CHECK(value.at("chat").at("is_bot") == true);

    constexpr std::array<TopicKind, 4> topic_kinds{TopicKind::Forum, TopicKind::Thread,
                                                   TopicKind::Direct, TopicKind::Saved};
    for (const auto kind : topic_kinds) {
        auto value_message = message();
        value_message.topic = TopicRef{.kind = kind, .id = 7};
        checked_event(StreamEvent{MessageEvent{std::move(value_message)}});
    }
}

TEST_CASE("stream event model rejects malformed supported values", "[stream][model]") {
    SECTION("identifier is outside int53") {
        auto value = message();
        value.id = kMaximumInt53 + 1;
        CHECK_FALSE(stream_event_json(StreamEvent{MessageEvent{value}}));
    }
    SECTION("timestamp is outside the schema domain") {
        CHECK_FALSE(valid_stream_timestamp("1970-01-01T00:00:00Z"));
        CHECK_FALSE(valid_stream_timestamp("2025-02-29T10:00:00Z"));
        CHECK_FALSE(valid_stream_timestamp("2038-01-19T03:14:08Z"));
        CHECK(valid_stream_timestamp("1970-01-01T00:00:01Z"));
        CHECK(valid_stream_timestamp("2000-02-29T23:59:59Z"));
        CHECK(valid_stream_timestamp("2038-01-19T03:14:07Z"));
    }
    SECTION("text is malformed UTF-8") {
        auto value = message();
        value.text = std::string(1, static_cast<char>(0xC3));
        CHECK_FALSE(stream_event_line(StreamEvent{MessageEvent{value}}));
    }
    SECTION("reaction discriminator payload is inconsistent") {
        const StreamEvent event =
            BotReactionSnapshotEvent{.chat_id = -1001,
                                     .message_id = 123,
                                     .date = "2026-08-05T10:00:00Z",
                                     .reactions = {{.reaction = {.kind = ReactionKind::CustomEmoji,
                                                                 .emoji = "unexpected",
                                                                 .custom_emoji_id = 42},
                                                    .total_count = 1}}};
        CHECK_FALSE(stream_event_json(event));
    }
    SECTION("reaction count is negative") {
        const StreamEvent event = BotReactionSnapshotEvent{
            .chat_id = -1001,
            .message_id = 123,
            .date = "2026-08-05T10:00:00Z",
            .reactions = {
                {.reaction = {.kind = ReactionKind::Paid, .emoji = "", .custom_emoji_id = 0},
                 .total_count = -1}}};
        CHECK_FALSE(stream_event_json(event));
    }
    SECTION("sender variant has the wrong id domain") {
        const StreamEvent event =
            BotReactionChangeEvent{.chat_id = -1001,
                                   .message_id = 123,
                                   .actor = {.kind = MessageSenderKind::User, .id = -1},
                                   .date = "2026-08-05T10:00:00Z",
                                   .old_reactions = {},
                                   .new_reactions = {}};
        CHECK_FALSE(stream_event_json(event));
    }
    SECTION("chat list variant has the wrong folder domain") {
        const StreamEvent event = ChatChangeEvent{ListAddedChatChange{
            .chat_id = -1001, .list = {.kind = ChatListKind::Folder, .folder_id = 0}}};
        CHECK_FALSE(stream_event_json(event));
    }
    SECTION("summary folder ids are not strictly ascending") {
        auto value = summary();
        value.folder_ids = {3, 2};
        CHECK_FALSE(stream_event_json(StreamEvent{ChatChangeEvent{NewChatChange{value}}}));
    }
    SECTION("last message belongs to another chat") {
        auto value = message(-1002);
        const StreamEvent event =
            ChatChangeEvent{LastMessageChatChange{.chat_id = -1001, .last_message = value}};
        CHECK_FALSE(stream_event_json(event));
    }
    SECTION("delete batch contains an invalid id") {
        const StreamEvent event = DeleteBatchEvent{
            .chat_id = -1001, .message_ids = {123, 0}, .is_permanent = true, .from_cache = false};
        CHECK_FALSE(stream_event_json(event));
    }
}
