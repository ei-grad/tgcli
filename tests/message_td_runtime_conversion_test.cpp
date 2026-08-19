#include "core/td_runtime_test_adapter.hpp"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

TEST_CASE("production messages conversion retains exact null positions and message fields",
          "[msg][core][tdlib][td-runtime-converter]") {
    auto message = td_api::make_object<td_api::message>();
    message->id_ = 123;
    message->chat_id_ = -1001;
    message->date_ = 1785924000;
    message->sender_id_ = td_api::make_object<td_api::messageSenderUser>(42);
    message->topic_id_ = td_api::make_object<td_api::messageTopicForum>(7);
    message->content_ = td_api::make_object<td_api::messageAnimatedEmoji>(nullptr, "🧪");

    std::vector<td_api::object_ptr<td_api::message>> items;
    items.push_back(std::move(message));
    items.emplace_back(nullptr);
    td_api::object_ptr<td_api::Object> native =
        td_api::make_object<td_api::messages>(2, std::move(items));
    auto converted = detail::convert_production_response_for_test(TdValue::from(std::move(native)));
    const auto* messages = converted.get_if<TdMessages>();
    REQUIRE(messages != nullptr);
    CHECK(messages->total_count == 2);
    REQUIRE(messages->messages.size() == 2);
    REQUIRE(messages->messages[0]);
    CHECK(messages->messages[0]->id == 123);
    CHECK(messages->messages[0]->chat_id == -1001);
    CHECK(messages->messages[0]->content_kind == TdMessageContentKind::Text);
    CHECK(messages->messages[0]->text == "🧪");
    CHECK_FALSE(messages->messages[1]);
}

TEST_CASE("production messages conversion retains total count boundary values",
          "[msg][core][tdlib][td-runtime-converter]") {
    for (const auto count : {-1, std::numeric_limits<std::int32_t>::max()}) {
        DYNAMIC_SECTION(count) {
            td_api::object_ptr<td_api::Object> native = td_api::make_object<td_api::messages>(
                count, std::vector<td_api::object_ptr<td_api::message>>{});
            auto converted =
                detail::convert_production_response_for_test(TdValue::from(std::move(native)));
            const auto* messages = converted.get_if<TdMessages>();
            REQUIRE(messages != nullptr);
            CHECK(messages->total_count == count);
            CHECK(messages->messages.empty());
        }
    }
}

TEST_CASE("production messageLink conversion preserves opaque link and visibility",
          "[msg][core][tdlib][td-runtime-converter]") {
    td_api::object_ptr<td_api::Object> native =
        td_api::make_object<td_api::messageLink>("urn:telegram:message", false);
    auto converted = detail::convert_production_response_for_test(TdValue::from(std::move(native)));
    const auto* link = converted.get_if<TdMessageLink>();
    REQUIRE(link != nullptr);
    CHECK(link->link == "urn:telegram:message");
    CHECK_FALSE(link->is_public);
}

TEST_CASE("production message-read builders retain all exact pinned TDLib arguments",
          "[msg][core][tdlib][td-runtime-factory]") {
    auto messages = detail::make_production_get_messages_for_test(-1001, {123, 123, 124});
    const std::vector<std::int64_t> expected_ids{123, 123, 124};
    CHECK(detail::production_function_matches_for_test(messages, TdFunctionKind::GetMessages));
    CHECK(detail::production_get_messages_matches_for_test(messages, -1001, expected_ids));

    auto link =
        detail::make_production_get_message_link_for_test(-1001, 123, 0, 0, "", false, false);
    CHECK(detail::production_function_matches_for_test(link, TdFunctionKind::GetMessageLink));
    CHECK(detail::production_get_message_link_matches_for_test(link, -1001, 123, 0, 0, "", false,
                                                               false));
}
