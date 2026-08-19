#include "core/td_runtime_test_adapter.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

td_api::object_ptr<td_api::message> native_message(std::int64_t chat_id, std::int64_t id) {
    auto message = td_api::make_object<td_api::message>();
    message->id_ = id;
    message->chat_id_ = chat_id;
    message->date_ = 1785924000;
    message->sender_id_ = td_api::make_object<td_api::messageSenderUser>(42);
    message->content_ = td_api::make_object<td_api::messageText>(
        td_api::make_object<td_api::formattedText>(
            "message", std::vector<td_api::object_ptr<td_api::textEntity>>{}),
        nullptr, nullptr);
    return message;
}

} // namespace

TEST_CASE("production single-message and thread-info conversion retain exact read metadata",
          "[read][core][tdlib][td-runtime-converter]") {
    {
        td_api::object_ptr<td_api::Object> native = native_message(-1002, 123);
        auto converted =
            detail::convert_production_response_for_test(TdValue::from(std::move(native)));
        const auto* message = converted.get_if<TdMessageSummary>();
        REQUIRE(message != nullptr);
        CHECK(message->id == 123);
        CHECK(message->chat_id == -1002);
        CHECK(message->date == 1785924000);
        CHECK(message->text == "message");
    }
    {
        std::vector<td_api::object_ptr<td_api::message>> starting;
        starting.push_back(native_message(-1002, 200));
        starting.emplace_back(nullptr);
        starting.push_back(native_message(-1002, 150));
        auto info = td_api::make_object<td_api::messageThreadInfo>();
        info->chat_id_ = -1002;
        info->message_thread_id_ = 150;
        info->messages_ = std::move(starting);
        td_api::object_ptr<td_api::Object> native = std::move(info);
        auto converted =
            detail::convert_production_response_for_test(TdValue::from(std::move(native)));
        const auto* thread = converted.get_if<TdMessageThreadInfo>();
        REQUIRE(thread != nullptr);
        CHECK(thread->history_chat_id == -1002);
        CHECK(thread->history_thread_id == 150);
        REQUIRE(thread->starting_messages.size() == 3);
        REQUIRE(thread->starting_messages[0]);
        CHECK(thread->starting_messages[0]->id == 200);
        CHECK_FALSE(thread->starting_messages[1]);
        REQUIRE(thread->starting_messages[2]);
        CHECK(thread->starting_messages[2]->id == 150);
    }
}

TEST_CASE("production read-history builders retain every exact pinned TDLib argument",
          "[read][core][tdlib][td-runtime-factory]") {
    auto plain = detail::make_production_get_chat_history_for_test(-1001, 123, 0, 21, true);
    CHECK(detail::production_function_matches_for_test(plain, TdFunctionKind::GetChatHistory));
    CHECK(detail::production_get_chat_history_matches_for_test(plain, -1001, 123, 0, 21, true));

    auto date = detail::make_production_get_chat_message_by_date_for_test(-1002, -17);
    CHECK(detail::production_function_matches_for_test(date, TdFunctionKind::GetChatMessageByDate));
    CHECK(detail::production_get_chat_message_by_date_matches_for_test(date, -1002, -17));

    auto thread = detail::make_production_get_message_thread_for_test(-1001, 500);
    CHECK(detail::production_function_matches_for_test(thread, TdFunctionKind::GetMessageThread));
    CHECK(detail::production_get_message_thread_matches_for_test(thread, -1001, 500));

    auto forum = detail::make_production_get_forum_topic_history_for_test(-1001, 7, 123, 0, 20);
    CHECK(
        detail::production_function_matches_for_test(forum, TdFunctionKind::GetForumTopicHistory));
    CHECK(detail::production_get_forum_topic_history_matches_for_test(forum, -1001, 7, 123, 0, 20));

    auto thread_history =
        detail::make_production_get_message_thread_history_for_test(-1001, 500, 123, 0, 20);
    CHECK(detail::production_function_matches_for_test(thread_history,
                                                       TdFunctionKind::GetMessageThreadHistory));
    CHECK(detail::production_get_message_thread_history_matches_for_test(thread_history, -1001, 500,
                                                                         123, 0, 20));

    auto direct = detail::make_production_get_direct_messages_chat_topic_history_for_test(
        -1001, 600, 123, 0, 20);
    CHECK(detail::production_function_matches_for_test(
        direct, TdFunctionKind::GetDirectMessagesChatTopicHistory));
    CHECK(detail::production_get_direct_messages_chat_topic_history_matches_for_test(
        direct, -1001, 600, 123, 0, 20));

    auto saved = detail::make_production_get_saved_messages_topic_history_for_test(700, 123, 0, 20);
    CHECK(detail::production_function_matches_for_test(
        saved, TdFunctionKind::GetSavedMessagesTopicHistory));
    CHECK(detail::production_get_saved_messages_topic_history_matches_for_test(saved, 700, 123, 0,
                                                                               20));
}
