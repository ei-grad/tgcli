#include "daemon/m6_model.hpp"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

namespace core = tgcli::core;
namespace daemon = tgcli::daemon;

core::TdM6FolderInfo folder_info() {
    return {
        .id = 7,
        .name = {.text = "A\xF0\x9F\x98\x80"
                         "B",
                 .animate_custom_emoji = true,
                 .custom_emoji_entities = {{.offset = 1, .length = 2, .custom_emoji_id = "42"}}},
        .icon = core::TdM6FolderIcon::Work,
        .color_id = 3,
        .is_shareable = false,
        .has_my_invite_links = true};
}

TEST_CASE("M6 folder projection retains animation and exact UTF-16 entities", "[m6][model]") {
    const auto projected = daemon::m6_folder_summary_json(folder_info());
    REQUIRE(projected);
    CHECK((*projected)["name"]["animate_custom_emoji"] == true);
    CHECK((*projected)["name"]["custom_emoji_entities"][0]["offset"] == 1);
    CHECK((*projected)["name"]["custom_emoji_entities"][0]["length"] == 2);

    auto split = folder_info();
    split.name.custom_emoji_entities[0].offset = 2;
    split.name.custom_emoji_entities[0].length = 1;
    CHECK_FALSE(daemon::m6_folder_summary_json(split));
}

TEST_CASE("M6 contact materialization is atomic ordered and bounded", "[m6][model]") {
    const core::TdM6Users ids{.total_count = 2, .user_ids = {42, 43}};
    std::vector<core::TdUserSummary> users{{.id = 42,
                                            .first_name = "Ada",
                                            .last_name = "Lovelace",
                                            .usernames = {"ada"},
                                            .phone_number = "12025550123",
                                            .is_bot = false,
                                            .is_premium = false},
                                           {.id = 43,
                                            .first_name = "Grace",
                                            .last_name = "Hopper",
                                            .usernames = {"grace"},
                                            .phone_number = {},
                                            .is_bot = false,
                                            .is_premium = false}};
    const auto result = daemon::m6_contact_list_json(ids, users, false);
    REQUIRE(result);
    CHECK((*result)["items"][0]["id"] == 42);
    CHECK((*result)["items"][1]["display_name"] == "Grace Hopper");
    CHECK((*result)["next"].is_null());

    users[1].id = 42;
    CHECK_FALSE(daemon::m6_contact_list_json(ids, users, false));
    users[1].id = 43;
    auto duplicate = ids;
    duplicate.user_ids[1] = 42;
    CHECK_FALSE(daemon::m6_contact_list_json(duplicate, users, false));
}

TEST_CASE("M6 folder and session list projection retain exact upstream order", "[m6][model]") {
    auto first = folder_info();
    auto second = folder_info();
    second.id = 8;
    second.name.text = "Other";
    second.name.custom_emoji_entities.clear();
    const auto folders = daemon::m6_folder_list_json(
        {.folders = {first, second}, .main_chat_list_position = 1, .are_tags_enabled = true});
    REQUIRE(folders);
    CHECK((*folders)["items"][0]["id"] == 7);
    CHECK((*folders)["items"][1]["id"] == 8);

    core::TdSession session{.id = "-7",
                            .is_current = true,
                            .is_password_pending = false,
                            .is_unconfirmed = false,
                            .can_accept_secret_chats = true,
                            .can_accept_calls = true,
                            .device_type = core::TdSessionDeviceType::Linux,
                            .api_id = 1,
                            .application_name = "tgcli",
                            .application_version = "1",
                            .is_official_application = false,
                            .device_model = "PC",
                            .platform = "Linux",
                            .system_version = "1",
                            .log_in_date = "1970-01-01T00:00:01Z",
                            .last_active_date = std::nullopt,
                            .ip_address = "127.0.0.1",
                            .location = "local"};
    const auto sessions =
        daemon::m6_session_list_json({.items = {session}, .inactive_session_ttl_days = 30});
    REQUIRE(sessions);
    CHECK((*sessions)["items"][0]["id"] == "-7");
    CHECK((*sessions)["items"][0]["last_active_date"].is_null());
}

TEST_CASE("M6 folder snapshot rejects every duplicate and cross-list duplicate", "[m6][model]") {
    const auto info = folder_info();
    core::TdM6ChatFolder folder{.name = info.name,
                                .icon = core::TdM6FolderIcon::Work,
                                .color_id = 3,
                                .pinned_chat_ids = {-1001},
                                .included_chat_ids = {-1002},
                                .excluded_chat_ids = {-1003}};
    REQUIRE(daemon::m6_folder_snapshot_json(7, folder, info));

    for (const auto duplicate : {-1001LL, -1002LL, -1003LL}) {
        auto malformed = folder;
        malformed.excluded_chat_ids.push_back(duplicate);
        CHECK_FALSE(daemon::m6_folder_snapshot_json(7, malformed, info));
    }
}

TEST_CASE("M6 topic projection formats UTC and rejects malformed scalars", "[m6][model]") {
    core::TdM6ForumTopicInfo topic{
        .chat_id = -1001,
        .id = 9,
        .name = "Release",
        .icon = {.color = core::TdM6TopicColor::Purple, .custom_emoji_id = "0"},
        .creation_date = 1,
        .creator = {.kind = core::TdM6SenderKind::User,
                    .id = 42,
                    .unsupported_tdlib_type_id = std::nullopt}};
    const auto projected = daemon::m6_topic_info_json(topic);
    REQUIRE(projected);
    CHECK((*projected)["creation_date"] == "1970-01-01T00:00:01Z");
    CHECK((*projected)["creator"] == nlohmann::json{{"type", "user"}, {"id", 42}});

    topic.creation_date = 0;
    CHECK_FALSE(daemon::m6_topic_info_json(topic));
}

TEST_CASE("M6 storage projection proves every parent and top-level sum", "[m6][model]") {
    core::TdM6StorageStatistics statistics{
        .size = 7,
        .count = 3,
        .by_chat = {{.chat_id = -1001,
                     .size = 7,
                     .count = 3,
                     .by_file_type = {
                         {.file_type = core::TdM6StorageFileType::Photo, .size = 5, .count = 1},
                         {.file_type = core::TdM6StorageFileType::Video, .size = 2, .count = 2}}}}};
    REQUIRE(daemon::m6_storage_statistics_json(statistics));

    statistics.by_chat.front().size = 6;
    CHECK_FALSE(daemon::m6_storage_statistics_json(statistics));
    statistics.by_chat.front().size = 7;
    statistics.count = 2;
    CHECK_FALSE(daemon::m6_storage_statistics_json(statistics));
}

TEST_CASE("M6 invite projection rejects contradictory subscription records", "[m6][model]") {
    core::TdM6ChatInviteLink link{.invite_link = "https://t.me/+abcdef",
                                  .name = {},
                                  .creator_user_id = 42,
                                  .date = 1,
                                  .edit_date = 0,
                                  .expiration_date = 0,
                                  .member_limit = 0,
                                  .member_count = 0,
                                  .expired_member_count = 0,
                                  .pending_join_request_count = 0,
                                  .creates_join_request = false,
                                  .is_primary = false,
                                  .is_revoked = false,
                                  .subscription_pricing = core::TdM6StarSubscriptionPricing{
                                      .period = 2'592'000, .star_count = 100}};
    REQUIRE(daemon::m6_invite_link_json(link));
    link.member_limit = 1;
    CHECK_FALSE(daemon::m6_invite_link_json(link));
}

} // namespace
