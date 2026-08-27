#include "daemon/chat_summary.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

tgcli::core::TdChatList main_list() {
    return {.kind = tgcli::core::TdChatListKind::Main, .folder_id = 0, .tdlib_type_id = 1};
}

tgcli::core::TdChatList archive_list() {
    return {.kind = tgcli::core::TdChatListKind::Archive, .folder_id = 0, .tdlib_type_id = 2};
}

tgcli::core::TdChatList folder_list(std::int32_t folder_id) {
    return {
        .kind = tgcli::core::TdChatListKind::Folder, .folder_id = folder_id, .tdlib_type_id = 3};
}

tgcli::core::TdChat project_chat() {
    return {
        .id = -1001,
        .title = "Project",
        .kind = tgcli::core::TdChatKind::Supergroup,
        .related_id = 55,
        .tdlib_type_id = 4,
        .positions = {},
        .chat_lists = {folder_list(3), archive_list(), main_list(), folder_list(2), folder_list(3)},
        .is_marked_unread = true,
        .unread_count = 3,
        .unread_mention_count = 1,
        .unread_reaction_count = 2,
        .unread_poll_vote_count = 4,
        .last_message =
            tgcli::core::TdMessageSummary{
                .id = 123,
                .chat_id = -1001,
                .date = 1785924000,
                .sender = {.kind = tgcli::core::TdMessageSenderKind::User,
                           .id = 42,
                           .tdlib_type_id = 5},
                .is_outgoing = false,
                .topic = tgcli::core::TdTopic{.kind = tgcli::core::TdTopicKind::Forum,
                                              .id = 7,
                                              .tdlib_type_id = 6},
                .content_kind = tgcli::core::TdMessageContentKind::Text,
                .text = "experiment result"},
        .permissions = std::nullopt,
        .notification_settings = std::nullopt};
}

tgcli::daemon::ChatIdentity project_identity() {
    return {.id = -1001,
            .title = "Project",
            .type = "supergroup",
            .is_bot = false,
            .usernames = {"project"}};
}

} // namespace

TEST_CASE("shared chat summary preserves chats JSON bytes and semantics", "[chats][dto]") {
    const auto summary =
        tgcli::daemon::materialize_chat_summary(project_chat(), project_identity());
    REQUIRE(summary);
    CHECK(summary->is_archived);
    CHECK((summary->folder_ids == std::vector<std::int32_t>{2, 3}));
    REQUIRE(summary->last_message);
    CHECK((summary->last_message->topic ==
           tgcli::daemon::TopicRef{.kind = tgcli::daemon::TopicKind::Forum, .id = 7}));

    const auto rendered = tgcli::daemon::chat_summary_json(*summary);
    CHECK((rendered == nlohmann::json{{"id", -1001},
                                      {"title", "Project"},
                                      {"type", "supergroup"},
                                      {"is_bot", false},
                                      {"usernames", {"project"}},
                                      {"is_archived", true},
                                      {"folder_ids", {2, 3}},
                                      {"is_marked_unread", true},
                                      {"unread_count", 3},
                                      {"unread_mention_count", 1},
                                      {"unread_reaction_count", 2},
                                      {"unread_poll_vote_count", 4},
                                      {"last_message",
                                       {{"id", 123},
                                        {"chat_id", -1001},
                                        {"date", "2026-08-05T10:00:00Z"},
                                        {"sender", {{"type", "user"}, {"id", 42}}},
                                        {"is_outgoing", false},
                                        {"topic", {{"kind", "forum"}, {"id", 7}}},
                                        {"type", "text"},
                                        {"text", "experiment result"}}}}));
    CHECK(
        rendered.dump() ==
        R"({"folder_ids":[2,3],"id":-1001,"is_archived":true,"is_bot":false,"is_marked_unread":true,"last_message":{"chat_id":-1001,"date":"2026-08-05T10:00:00Z","id":123,"is_outgoing":false,"sender":{"id":42,"type":"user"},"text":"experiment result","topic":{"id":7,"kind":"forum"},"type":"text"},"title":"Project","type":"supergroup","unread_count":3,"unread_mention_count":1,"unread_poll_vote_count":4,"unread_reaction_count":2,"usernames":["project"]})");
}

TEST_CASE("shared chat summary enforces the exact signed int53 id domain", "[chats][dto]") {
    constexpr std::int64_t maximum_int53 = 9'007'199'254'740'991LL;
    auto chat = project_chat();
    auto identity = project_identity();
    chat.last_message.reset();

    for (const auto valid_id : std::array<std::int64_t, 2>{-maximum_int53, maximum_int53}) {
        chat.id = valid_id;
        identity.id = valid_id;
        const auto summary = tgcli::daemon::materialize_chat_summary(chat, identity);
        REQUIRE(summary);
        CHECK(summary->identity.id == valid_id);
    }

    for (const auto invalid_id :
         std::array<std::int64_t, 3>{-maximum_int53 - 1, 0, maximum_int53 + 1}) {
        chat.id = invalid_id;
        identity.id = invalid_id;
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
}

TEST_CASE("shared chat summary rejects malformed TD projections", "[chats][dto]") {
    auto chat = project_chat();
    auto identity = project_identity();

    SECTION("identity does not match the chat") {
        identity.id = -1002;
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
    SECTION("non-private bot identity is impossible") {
        identity.is_bot = true;
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
    SECTION("counter is negative") {
        chat.unread_poll_vote_count = -1;
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
    SECTION("chat list is malformed") {
        chat.chat_lists.push_back(
            {.kind = tgcli::core::TdChatListKind::Folder, .folder_id = 0, .tdlib_type_id = 3});
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
    SECTION("last message belongs to another chat") {
        chat.last_message->chat_id = -1002;
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
    SECTION("chat type is not public") {
        chat.kind = tgcli::core::TdChatKind::Secret;
        identity.type = "secret";
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
    SECTION("copied text is not UTF-8") {
        chat.last_message->text = std::string(1, static_cast<char>(0xC3));
        CHECK_FALSE(tgcli::daemon::materialize_chat_summary(chat, identity));
    }
}
