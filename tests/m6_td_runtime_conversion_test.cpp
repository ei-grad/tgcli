// Exercises the pinned generated-TDLib side of the M6 type-erased boundary.
// Tagged [tdlib] because this translation unit includes generated TDLib types.

#include "core/td_runtime_test_adapter.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

namespace {

using namespace tgcli::core;
namespace td_api = td::td_api;

using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

TdM6ChatFolder folder() {
    return {.name = {.text = "Work", .animate_custom_emoji = false, .custom_emoji_entities = {}},
            .icon = TdM6FolderIcon::Work,
            .color_id = 2,
            .is_shareable = false,
            .pinned_chat_ids = {},
            .included_chat_ids = {7},
            .excluded_chat_ids = {},
            .exclude_muted = false,
            .exclude_read = false,
            .exclude_archived = false,
            .include_contacts = false,
            .include_non_contacts = false,
            .include_bots = false,
            .include_groups = false,
            .include_channels = false};
}

std::vector<TdM6Request> requests() {
    auto administrator = TdM6MemberStatus{};
    administrator.kind = TdM6MemberStatusKind::Administrator;
    administrator.can_be_edited = true;
    administrator.rights.can_manage_chat = true;
    administrator.rights.can_change_info = true;
    return {
        TdM6GetContactsRequest{},
        TdM6SearchContactsRequest{.query = "alice", .limit = 100},
        TdM6AddContactRequest{.user_id = 7,
                              .phone_number = "+12025550123",
                              .first_name = "Alice",
                              .last_name = "Example",
                              .share_phone_number = false},
        TdM6RemoveContactsRequest{.user_ids = {7}},
        TdM6SetBlockRequest{.user_id = 7, .blocked = true},
        TdM6GetChatFolderRequest{.folder_id = 3},
        TdM6CreateChatFolderRequest{.folder = folder()},
        TdM6EditChatFolderRequest{.folder_id = 3, .folder = folder()},
        TdM6DeleteChatFolderRequest{.folder_id = 3, .leave_chat_ids = {}},
        TdM6GetForumTopicsRequest{.chat_id = -1001,
                                  .query = "",
                                  .offset_date = 0,
                                  .offset_message_id = 0,
                                  .offset_forum_topic_id = 0,
                                  .limit = 100},
        TdM6GetForumTopicRequest{.chat_id = -1001, .topic_id = 4},
        TdM6CreateForumTopicRequest{.chat_id = -1001,
                                    .name = "Topic",
                                    .icon = {.color = TdM6TopicColor::Blue, .custom_emoji_id = "0"},
                                    .is_name_implicit = false},
        TdM6EditForumTopicRequest{.chat_id = -1001,
                                  .topic_id = 4,
                                  .name = "Renamed",
                                  .edit_icon_custom_emoji = false,
                                  .icon_custom_emoji_id = 0},
        TdM6ToggleForumTopicRequest{.chat_id = -1001, .topic_id = 4, .is_closed = true},
        TdM6GetChatMemberRequest{.chat_id = -1001, .user_id = 7},
        TdM6SetChatTitleRequest{.chat_id = -1001, .title = "Title"},
        TdM6SetChatPhotoRequest{.chat_id = -1001, .local_path = std::nullopt},
        TdM6SetChatDescriptionRequest{.chat_id = -1001, .description = "Description"},
        TdM6CreateChatInviteLinkRequest{.chat_id = -1001},
        TdM6RevokeChatInviteLinkRequest{.chat_id = -1001, .invite_link = "https://t.me/+secret"},
        TdM6SetChatMemberStatusRequest{.chat_id = -1001, .user_id = 7, .status = administrator},
        TdM6SetChatPermissionsRequest{.chat_id = -1001, .permissions = {}},
        TdM6GetStorageStatisticsRequest{.chat_limit = 100},
        TdM6OptimizeStorageRequest{},
    };
}

TEST_CASE("production M6 factories and native matchers cover every request variant",
          "[m6][tdlib][td-runtime]") {
    const auto all = requests();
    REQUIRE(all.size() == 24);
    for (const auto& request : all) {
        CAPTURE(td_function_name(td_m6_request_kind(request)));
        auto native = detail::make_production_m6_function_for_test(request);
        REQUIRE(native.function_data().has_value());
        CHECK(native.function_data()->kind() == td_m6_request_kind(request));
        CHECK(detail::production_function_matches_for_test(native, td_m6_request_kind(request)));
    }
}

TEST_CASE("production M6 converters retain strict folder topic member invite and storage trees",
          "[m6][tdlib][td-runtime]") {
    SECTION("contacts") {
        auto users = td_api::make_object<td_api::users>();
        users->total_count_ = 2;
        users->user_ids_ = {7, 8};
        auto value = detail::convert_production_m6_response_for_test(
            TdFunctionKind::SearchContacts, TdValue::from(NativeObjectPtr{std::move(users)}));
        const auto* response = value.get_if<TdM6Response>();
        REQUIRE(response != nullptr);
        REQUIRE(std::get_if<TdM6Users>(response) != nullptr);
        CHECK(std::get<TdM6Users>(*response).user_ids == std::vector<std::int64_t>{7, 8});
    }

    SECTION("top-level folder and topic absence stays typed") {
        auto folder_value = detail::convert_production_m6_response_for_test(
            TdFunctionKind::GetChatFolder, TdValue::from(NativeObjectPtr{}));
        REQUIRE(folder_value.get_if<TdM6Response>() != nullptr);
        CHECK_FALSE(
            std::get<TdM6MaybeChatFolder>(*folder_value.get_if<TdM6Response>()).folder.has_value());

        auto topic_value = detail::convert_production_m6_response_for_test(
            TdFunctionKind::GetForumTopic, TdValue::from(NativeObjectPtr{}));
        REQUIRE(topic_value.get_if<TdM6Response>() != nullptr);
        CHECK_FALSE(
            std::get<TdM6MaybeForumTopic>(*topic_value.get_if<TdM6Response>()).topic.has_value());
    }

    SECTION("storage") {
        auto file = td_api::make_object<td_api::storageStatisticsByFileType>();
        file->file_type_ = td_api::make_object<td_api::fileTypePhoto>();
        file->size_ = 10;
        file->count_ = 1;
        auto chat = td_api::make_object<td_api::storageStatisticsByChat>();
        chat->chat_id_ = 0;
        chat->size_ = 10;
        chat->count_ = 1;
        chat->by_file_type_.push_back(std::move(file));
        auto statistics = td_api::make_object<td_api::storageStatistics>();
        statistics->size_ = 10;
        statistics->count_ = 1;
        statistics->by_chat_.push_back(std::move(chat));
        auto value = detail::convert_production_m6_response_for_test(
            TdFunctionKind::GetStorageStatistics,
            TdValue::from(NativeObjectPtr{std::move(statistics)}));
        const auto* response = value.get_if<TdM6Response>();
        REQUIRE(response != nullptr);
        const auto* converted = std::get_if<TdM6StorageStatistics>(response);
        REQUIRE(converted != nullptr);
        REQUIRE(converted->by_chat.size() == 1);
        REQUIRE(converted->by_chat.front().by_file_type.size() == 1);
        CHECK(converted->by_chat.front().by_file_type.front().file_type ==
              TdM6StorageFileType::Photo);
    }
}

} // namespace
