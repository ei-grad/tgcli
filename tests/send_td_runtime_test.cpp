#include "core/td_client.hpp"
#include "core/td_runtime_test_adapter.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace std::chrono_literals;
using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

class UnsupportedSendingState final : public td_api::MessageSendingState {
  public:
    static constexpr std::int32_t ID = 700'000'101;

    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }

    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

TdSendMessageRequest plain_request(TdSendSchedule schedule = {}) {
    return {.chat_id = -1001,
            .topic = TdTopic{.kind = TdTopicKind::Forum, .id = 9, .tdlib_type_id = 0},
            .reply_to_message_id = 77,
            .options = {.disable_notification = true, .schedule = schedule, .sending_id = 12345},
            .content = {.formatted_text = {.text = "send 🧪", .entities = {}, .capability = {}},
                        .parsed = false}};
}

const TdFieldValue* function_field(const TdFunctionData& function, std::string_view name) {
    const auto found =
        std::ranges::find_if(function.fields(), [&](const TdFunctionField& candidate) {
            return candidate.has_name(name);
        });
    return found == function.fields().end() ? nullptr : &found->value();
}

td_api::object_ptr<td_api::message>
native_message(std::int64_t id,
               td_api::object_ptr<td_api::MessageSendingState> sending_state = nullptr,
               td_api::object_ptr<td_api::MessageSchedulingState> scheduling_state = nullptr) {
    auto value = td_api::make_object<td_api::message>();
    value->id_ = id;
    value->chat_id_ = -1001;
    value->date_ = 1'785'924'000;
    value->sender_id_ = td_api::make_object<td_api::messageSenderUser>(42);
    value->sending_state_ = std::move(sending_state);
    value->scheduling_state_ = std::move(scheduling_state);
    value->is_outgoing_ = true;
    value->topic_id_ = td_api::make_object<td_api::messageTopicForum>(9);
    value->content_ = td_api::make_object<td_api::messageText>(
        td_api::make_object<td_api::formattedText>(
            "send 🧪", std::vector<td_api::object_ptr<td_api::textEntity>>{}),
        nullptr, nullptr);
    return value;
}

template <typename Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

} // namespace

static_assert(!std::is_copy_constructible_v<TdFormattedTextCapability>);
static_assert(!std::is_copy_assignable_v<TdFormattedTextCapability>);
static_assert(std::is_move_constructible_v<TdFormattedTextCapability>);

template <typename Options>
concept HasProtectContent = requires(Options options) { options.protect_content; };

template <typename Options>
concept HasStickerSetUpdateOrder =
    requires(Options options) { options.update_order_of_installed_sticker_sets; };

static_assert(!HasProtectContent<TdMessageSendOptions>);
static_assert(!HasStickerSetUpdateOrder<TdMessageSendOptions>);

TEST_CASE("sendMessage native and scripted factories share every pinned default",
          "[core][tdlib][send][factory]") {
    tgcli::test::ScriptedTdRuntime scripted;
    for (const auto schedule :
         {TdSendSchedule{},
          TdSendSchedule{.kind = TdSendScheduleKind::AtDate, .send_date = 1'800'000'000},
          TdSendSchedule{.kind = TdSendScheduleKind::WhenOnline, .send_date = 0}}) {
        auto expected = plain_request(schedule);
        auto native = detail::make_production_send_message_for_test(plain_request(schedule), 7);
        auto fake = scripted.make_send_message(plain_request(schedule), 7);
        REQUIRE(native.function_data());
        REQUIRE(fake.function_data());
        CHECK(*native.function_data() == *fake.function_data());
        const auto* native_protect = function_field(*native.function_data(), "protect_content");
        const auto* native_update_order =
            function_field(*native.function_data(), "update_order_of_installed_sticker_sets");
        REQUIRE(native_protect != nullptr);
        REQUIRE(native_update_order != nullptr);
        CHECK_FALSE(std::get<bool>(*native_protect));
        CHECK_FALSE(std::get<bool>(*native_update_order));
        CHECK(detail::production_send_message_matches_for_test(native, expected));
        CHECK(detail::production_function_matches_for_test(native, TdFunctionKind::SendMessage));
    }

    auto saved = plain_request();
    saved.topic = TdTopic{.kind = TdTopicKind::Saved, .id = 91, .tdlib_type_id = 0};
    saved.reply_to_message_id.reset();
    saved.options.disable_notification = false;
    auto expected = plain_request();
    expected.topic = TdTopic{.kind = TdTopicKind::Saved, .id = 91, .tdlib_type_id = 0};
    expected.reply_to_message_id.reset();
    expected.options.disable_notification = false;
    auto native = detail::make_production_send_message_for_test(std::move(saved), 7);
    CHECK(detail::production_send_message_matches_for_test(native, expected));
}

TEST_CASE("parsed formattedText capability is exact generation-bound and one-shot",
          "[core][tdlib][send][capability]") {
    auto capability = TdFormattedTextCapability::from(
        TdScriptedFormattedTextCapability{.text = "bound", .entities = {}}, 7);
    CHECK(capability.valid_for(7));
    CHECK_FALSE(capability.valid_for(8));
    CHECK_FALSE(capability.consume<TdScriptedFormattedTextCapability>(8));
    CHECK(capability.consume<TdScriptedFormattedTextCapability>(7));
    CHECK_FALSE(capability.has_value());
    CHECK_FALSE(capability.consume<TdScriptedFormattedTextCapability>(7));

    std::vector<td_api::object_ptr<td_api::textEntity>> entities;
    entities.push_back(td_api::make_object<td_api::textEntity>(
        0, 4, td_api::make_object<td_api::textEntityTypeBold>()));
    auto converted = detail::convert_production_direct_response_for_test(
        TdFunctionKind::ParseTextEntities,
        TdValue::from(NativeObjectPtr{
            td_api::make_object<td_api::formattedText>("bold", std::move(entities))}));
    auto* formatted = converted.get_if<TdFormattedText>();
    REQUIRE(formatted != nullptr);
    REQUIRE(formatted->capability.valid_for(1));
    TdSendMessageRequest parsed{
        .chat_id = -1001,
        .topic = std::nullopt,
        .reply_to_message_id = std::nullopt,
        .options = {.disable_notification = false, .schedule = {}, .sending_id = 9},
        .content = {.formatted_text = std::move(*formatted), .parsed = true}};
    const TdSendMessageRequest expected{
        .chat_id = -1001,
        .topic = std::nullopt,
        .reply_to_message_id = std::nullopt,
        .options = {.disable_notification = false, .schedule = {}, .sending_id = 9},
        .content = {
            .formatted_text = {.text = "bold",
                               .entities = {{.offset = 0,
                                             .length = 4,
                                             .kind = TdTextEntityKind::Bold,
                                             .value = {},
                                             .numeric_value = 0,
                                             .tdlib_type_id = td_api::textEntityTypeBold::ID,
                                             .date_time_formatting = std::nullopt}},
                               .capability = {}},
            .parsed = true}};
    auto native = detail::make_production_send_message_for_test(std::move(parsed), 1);
    CHECK(detail::production_send_message_matches_for_test(native, expected));

    SECTION("scripted capability rejects changed text facts") {
        tgcli::test::ScriptedTdRuntime scripted;
        auto request = plain_request();
        request.content.parsed = true;
        request.content.formatted_text = tgcli::test::ScriptedTdRuntime::parsed_formatted_text(
            {.client_id = 1, .client_generation = 7}, "bold",
            {{.offset = 0,
              .length = 4,
              .kind = TdTextEntityKind::Bold,
              .value = {},
              .numeric_value = 0,
              .tdlib_type_id = 1,
              .date_time_formatting = std::nullopt}});
        request.content.formatted_text.text = "changed";
        CHECK_THROWS_AS(scripted.make_send_message(std::move(request), 7), std::invalid_argument);
    }

    SECTION("scripted capability rejects changed entity facts") {
        tgcli::test::ScriptedTdRuntime scripted;
        auto request = plain_request();
        request.content.parsed = true;
        request.content.formatted_text = tgcli::test::ScriptedTdRuntime::parsed_formatted_text(
            {.client_id = 1, .client_generation = 7}, "bold",
            {{.offset = 0,
              .length = 4,
              .kind = TdTextEntityKind::Bold,
              .value = {},
              .numeric_value = 0,
              .tdlib_type_id = 1,
              .date_time_formatting = std::nullopt}});
        request.content.formatted_text.entities.front().length = 2;
        CHECK_THROWS_AS(scripted.make_send_message(std::move(request), 7), std::invalid_argument);
    }

    SECTION("production and scripted facts reject an out-of-range UTF-16 entity") {
        std::vector<td_api::object_ptr<td_api::textEntity>> invalid_entities;
        invalid_entities.push_back(td_api::make_object<td_api::textEntity>(
            4, 1, td_api::make_object<td_api::textEntityTypeBold>()));
        auto invalid_production = detail::convert_production_direct_response_for_test(
            TdFunctionKind::ParseTextEntities,
            TdValue::from(NativeObjectPtr{
                td_api::make_object<td_api::formattedText>("bold", std::move(invalid_entities))}));
        CHECK(invalid_production.get_if<TdDirectConversionError>() != nullptr);

        tgcli::test::ScriptedTdRuntime scripted;
        auto request = plain_request();
        request.content.parsed = true;
        request.content.formatted_text = tgcli::test::ScriptedTdRuntime::parsed_formatted_text(
            {.client_id = 1, .client_generation = 7}, "bold",
            {{.offset = 4,
              .length = 1,
              .kind = TdTextEntityKind::Bold,
              .value = {},
              .numeric_value = 0,
              .tdlib_type_id = 1,
              .date_time_formatting = std::nullopt}});
        CHECK_FALSE(valid_td_send_message_request(request));
        CHECK_THROWS_AS(scripted.make_send_message(std::move(request), 7), std::invalid_argument);
    }

    SECTION("production and scripted facts reject a split UTF-16 surrogate pair") {
        std::vector<td_api::object_ptr<td_api::textEntity>> invalid_entities;
        invalid_entities.push_back(td_api::make_object<td_api::textEntity>(
            1, 1, td_api::make_object<td_api::textEntityTypeBold>()));
        auto invalid_production = detail::convert_production_direct_response_for_test(
            TdFunctionKind::ParseTextEntities,
            TdValue::from(NativeObjectPtr{
                td_api::make_object<td_api::formattedText>("🧪", std::move(invalid_entities))}));
        CHECK(invalid_production.get_if<TdDirectConversionError>() != nullptr);

        tgcli::test::ScriptedTdRuntime scripted;
        auto request = plain_request();
        request.content.parsed = true;
        request.content.formatted_text = tgcli::test::ScriptedTdRuntime::parsed_formatted_text(
            {.client_id = 1, .client_generation = 7}, "🧪",
            {{.offset = 1,
              .length = 1,
              .kind = TdTextEntityKind::Bold,
              .value = {},
              .numeric_value = 0,
              .tdlib_type_id = 1,
              .date_time_formatting = std::nullopt}});
        CHECK_FALSE(valid_td_send_message_request(request));
        CHECK_THROWS_AS(scripted.make_send_message(std::move(request), 7), std::invalid_argument);
    }
}

TEST_CASE("sendMessage conversion separates stable message facts from sending state",
          "[core][tdlib][send][conversion]") {
    auto pending = detail::convert_production_send_response_for_test(
        TdValue::from(NativeObjectPtr{
            native_message(-77, td_api::make_object<td_api::messageSendingStatePending>(12345),
                           td_api::make_object<td_api::messageSchedulingStateSendWhenOnline>())}),
        7);
    const auto* pending_message = pending.get_if<TdWriteMessage>();
    REQUIRE(pending_message != nullptr);
    CHECK(pending_message->message.id == -77);
    CHECK(pending_message->sending_state.kind == TdMessageSendingStateKind::Pending);
    CHECK(pending_message->sending_state.sending_id == 12345);
    CHECK(pending_message->scheduling_state.kind == TdMessageSchedulingStateKind::SendWhenOnline);
    CHECK_FALSE(pending_message->has_reply_markup);

    auto failed_state = td_api::make_object<td_api::messageSendingStateFailed>(
        td_api::make_object<td_api::error>(429, "retry after 2"), true, true, true, true, 17, 1.25);
    auto failed = detail::convert_production_send_response_for_test(
        TdValue::from(NativeObjectPtr{native_message(-77, std::move(failed_state))}), 7);
    const auto* failed_message = failed.get_if<TdWriteMessage>();
    REQUIRE(failed_message != nullptr);
    CHECK(failed_message->sending_state.kind == TdMessageSendingStateKind::Failed);
    REQUIRE(failed_message->sending_state.error);
    CHECK(failed_message->sending_state.error->code == 429);
    CHECK(failed_message->sending_state.can_retry);
    CHECK(failed_message->sending_state.need_another_sender);
    CHECK(failed_message->sending_state.need_another_reply_quote);
    CHECK(failed_message->sending_state.need_drop_reply);
    CHECK(failed_message->sending_state.required_paid_message_star_count == 17);
    CHECK(failed_message->sending_state.retry_after == 1.25);

    auto unknown = detail::convert_production_send_response_for_test(
        TdValue::from(
            NativeObjectPtr{native_message(-77, td_api::make_object<UnsupportedSendingState>())}),
        7);
    REQUIRE(unknown.get_if<TdWriteMessage>() != nullptr);
    CHECK(unknown.get_if<TdWriteMessage>()->sending_state.kind ==
          TdMessageSendingStateKind::Unknown);
    CHECK(unknown.get_if<TdWriteMessage>()->sending_state.unsupported_tdlib_type_id ==
          UnsupportedSendingState::ID);
}

TEST_CASE("send lifecycle updates convert nulls and generation correlation without native leaks",
          "[core][tdlib][send][update]") {
    auto success = detail::convert_production_update_for_test(
        TdValue::from(NativeObjectPtr{
            td_api::make_object<td_api::updateMessageSendSucceeded>(native_message(101), -77)}),
        9);
    const auto* succeeded = success.get_if<TdUpdateMessageSendSucceeded>();
    REQUIRE(succeeded != nullptr);
    CHECK(succeeded->client_generation == 9);
    CHECK(succeeded->old_message_id == -77);
    REQUIRE(succeeded->message);
    CHECK(succeeded->message->message.id == 101);

    auto null_success = detail::convert_production_update_for_test(
        TdValue::from(
            NativeObjectPtr{td_api::make_object<td_api::updateMessageSendSucceeded>(nullptr, -77)}),
        9);
    REQUIRE(null_success.get_if<TdUpdateMessageSendSucceeded>() != nullptr);
    CHECK_FALSE(null_success.get_if<TdUpdateMessageSendSucceeded>()->message);

    auto failed = detail::convert_production_update_for_test(
        TdValue::from(NativeObjectPtr{td_api::make_object<td_api::updateMessageSendFailed>(
            native_message(-77, td_api::make_object<td_api::messageSendingStateFailed>(
                                    td_api::make_object<td_api::error>(400, "failed"), false, false,
                                    false, false, 0, 0.0)),
            -77, td_api::make_object<td_api::error>(400, "failed"))}),
        9);
    const auto* failed_update = failed.get_if<TdUpdateMessageSendFailed>();
    REQUIRE(failed_update != nullptr);
    REQUIRE(failed_update->message);
    REQUIRE(failed_update->error);
    CHECK(failed_update->error->code == 400);

    auto deleted = detail::convert_production_update_for_test(
        TdValue::from(NativeObjectPtr{td_api::make_object<td_api::updateDeleteMessages>(
            -1001, std::vector<std::int64_t>{-77, 88}, true, false)}),
        9);
    const auto* deletion = deleted.get_if<TdUpdateDeleteMessages>();
    REQUIRE(deletion != nullptr);
    CHECK(deletion->client_generation == 9);
    CHECK(deletion->message_ids == std::vector<std::int64_t>{-77, 88});
    CHECK(deletion->is_permanent);
    CHECK_FALSE(deletion->from_cache);
}

TEST_CASE("typed sendMessage rejects validation stale auth and expired capability before fake send",
          "[core][send][auth][fake-boundary]") {
    auto runtime_owner = std::make_unique<tgcli::test::ScriptedTdRuntime>();
    auto* runtime = runtime_owner.get();
    TdClient client(std::move(runtime_owner));
    REQUIRE(runtime->wait_for_sent(1));
    const auto first = runtime->clients().front();
    runtime->push_response(first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->auth_sequence == 1; }));
    const auto ready = client.auth_state();

    const auto reject = [&](TdSendMessageRequest request) {
        auto response = client.send_message(ready, std::move(request));
        CHECK_THROWS(response.get());
        CHECK(runtime->sent_functions().size() == 1);
    };

    auto invalid = plain_request();
    invalid.options.sending_id = 0;
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.chat_id = 0;
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.topic = TdTopic{.kind = TdTopicKind::Thread, .id = 9, .tdlib_type_id = 0};
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.reply_to_message_id = 0;
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.options.schedule = {.kind = TdSendScheduleKind::AtDate, .send_date = 0};
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.options.schedule = {.kind = TdSendScheduleKind::Immediate, .send_date = 1};
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.formatted_text.text.clear();
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.formatted_text.text = std::string(1, static_cast<char>(0xC0));
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.formatted_text.text = std::string(4'097, 'x');
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.parsed = true;
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.formatted_text.capability = TdFormattedTextCapability::from(
        TdScriptedFormattedTextCapability{.text = invalid.content.formatted_text.text,
                                          .entities = {}},
        first.client_generation);
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.parsed = true;
    invalid.content.formatted_text =
        tgcli::test::ScriptedTdRuntime::parsed_formatted_text(first, "parsed");
    invalid.content.formatted_text.text = "changed";
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.parsed = true;
    invalid.content.formatted_text = tgcli::test::ScriptedTdRuntime::parsed_formatted_text(
        first, "parsed",
        {{.offset = 0,
          .length = 6,
          .kind = TdTextEntityKind::Bold,
          .value = {},
          .numeric_value = 0,
          .tdlib_type_id = 1,
          .date_time_formatting = std::nullopt}});
    invalid.content.formatted_text.entities.front().length = 3;
    reject(std::move(invalid));
    invalid = plain_request();
    invalid.content.parsed = true;
    invalid.content.formatted_text = tgcli::test::ScriptedTdRuntime::parsed_formatted_text(
        first, "parsed",
        {{.offset = 6,
          .length = 1,
          .kind = TdTextEntityKind::Bold,
          .value = {},
          .numeric_value = 0,
          .tdlib_type_id = 1,
          .date_time_formatting = std::nullopt}});
    reject(std::move(invalid));

    auto expired = plain_request();
    expired.content.parsed = true;
    expired.content.formatted_text =
        tgcli::test::ScriptedTdRuntime::parsed_formatted_text(first, "parsed");
    expired.content.formatted_text.capability = TdFormattedTextCapability::from(
        TdScriptedFormattedTextCapability{.text = expired.content.formatted_text.text,
                                          .entities = expired.content.formatted_text.entities},
        first.client_generation + 1);
    auto expired_response = client.send_message(ready, std::move(expired));
    CHECK_THROWS(expired_response.get());
    CHECK(runtime->sent_functions().size() == 1);

    runtime->push_update(first, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return client.auth_state()->auth_sequence == 2; }));
    auto stale_response = client.send_message(ready, plain_request());
    CHECK_THROWS_AS(stale_response.get(), TdAuthorizationError);
    CHECK(runtime->sent_functions().size() == 1);

    client.close();
}
