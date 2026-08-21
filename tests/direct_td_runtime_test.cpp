#include "core/td_runtime_test_adapter.hpp"
#include "support/scripted_td_runtime.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

class UnsupportedTextEntityType final : public td_api::TextEntityType {
  public:
    static constexpr std::int32_t ID = 700'000'017;

    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }

    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

class UnsupportedDateTimeFormattingType final : public td_api::DateTimeFormattingType {
  public:
    static constexpr std::int32_t ID = 700'000'018;

    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }

    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

class UnsupportedDateTimePartPrecision final : public td_api::DateTimePartPrecision {
  public:
    static constexpr std::int32_t ID = 700'000'019;

    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }

    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

TdValue convert(TdFunctionKind function, NativeObjectPtr object) {
    return detail::convert_production_direct_response_for_test(function,
                                                               TdValue::from(std::move(object)));
}

TdValue make_scripted(tgcli::test::ScriptedTdRuntime& runtime, const TdDirectRequest& request) {
    return std::visit(
        [&](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, TdEditMessageTextRequest>) {
                return runtime.make_edit_message_text(value);
            } else if constexpr (std::is_same_v<Request, TdDeleteMessagesRequest>) {
                return runtime.make_delete_messages(value);
            } else if constexpr (std::is_same_v<Request, TdMessageReactionRequest>) {
                return runtime.make_message_reaction(value);
            } else if constexpr (std::is_same_v<Request, TdPinMessageRequest>) {
                return runtime.make_pin_message(value);
            } else if constexpr (std::is_same_v<Request, TdViewMessagesRequest>) {
                return runtime.make_view_messages(value);
            } else if constexpr (std::is_same_v<Request, TdSetChatNotificationSettingsRequest>) {
                return runtime.make_set_chat_notification_settings(value);
            } else if constexpr (std::is_same_v<Request, TdToggleChatIsPinnedRequest>) {
                return runtime.make_toggle_chat_is_pinned(value);
            } else if constexpr (std::is_same_v<Request, TdAddChatToListRequest>) {
                return runtime.make_add_chat_to_list(value);
            } else if constexpr (std::is_same_v<Request, TdJoinChatRequest>) {
                return runtime.make_join_chat(value);
            } else {
                static_assert(std::is_same_v<Request, TdLeaveChatRequest>);
                return runtime.make_leave_chat(value);
            }
        },
        request);
}

std::vector<TdDirectRequest> direct_requests() {
    const TdChatNotificationSettings settings{
        .use_default_mute_for = true,
        .mute_for = 17,
        .use_default_sound = true,
        .sound_id = std::numeric_limits<std::int64_t>::min(),
        .use_default_show_preview = true,
        .show_preview = true,
        .use_default_mute_stories = true,
        .mute_stories = true,
        .use_default_story_sound = true,
        .story_sound_id = std::numeric_limits<std::int64_t>::max(),
        .use_default_show_story_poster = true,
        .show_story_poster = true,
        .use_default_disable_pinned_message_notifications = true,
        .disable_pinned_message_notifications = true,
        .use_default_disable_mention_notifications = true,
        .disable_mention_notifications = true,
    };
    return {
        TdEditMessageTextRequest{.chat_id = -1001, .message_id = 10, .text = "edit 🧪"},
        TdDeleteMessagesRequest{.chat_id = -1001, .message_ids = {1, 2}, .revoke = true},
        TdDeleteMessagesRequest{.chat_id = -1001, .message_ids = {-2, -1, 1}, .revoke = false},
        TdMessageReactionRequest{
            .chat_id = -1001, .message_id = 10, .reaction = "👍🏽", .remove = false, .big = true},
        TdMessageReactionRequest{
            .chat_id = -1001, .message_id = 10, .reaction = "👍🏽", .remove = true, .big = false},
        TdPinMessageRequest{.chat_id = -1001, .message_id = 10, .pinned = true},
        TdPinMessageRequest{.chat_id = -1001, .message_id = 10, .pinned = false},
        TdViewMessagesRequest{.chat_id = -1001, .message_ids = {10}},
        TdSetChatNotificationSettingsRequest{.chat_id = -1001, .settings = settings},
        TdToggleChatIsPinnedRequest{
            .chat_id = -1001, .list = TdDirectChatList::Main, .pinned = true},
        TdToggleChatIsPinnedRequest{
            .chat_id = -1001, .list = TdDirectChatList::Archive, .pinned = false},
        TdAddChatToListRequest{.chat_id = -1001, .list = TdDirectChatList::Archive},
        TdAddChatToListRequest{.chat_id = -1001, .list = TdDirectChatList::Main},
        TdJoinChatRequest{.chat_id = -1001, .invite_link = std::nullopt},
        TdJoinChatRequest{.chat_id = std::nullopt, .invite_link = "https://t.me/+private-token"},
        TdLeaveChatRequest{.chat_id = -1001},
    };
}

td_api::object_ptr<td_api::message> message(bool scheduled) {
    auto value = td_api::make_object<td_api::message>();
    value->id_ = 77;
    value->chat_id_ = -1001;
    value->date_ = 1'785'924'000;
    value->sender_id_ = td_api::make_object<td_api::messageSenderUser>(42);
    value->is_outgoing_ = true;
    value->topic_id_ = td_api::make_object<td_api::messageTopicForum>(9);
    value->content_ = td_api::make_object<td_api::messageText>(
        td_api::make_object<td_api::formattedText>(
            "text 🧪", std::vector<td_api::object_ptr<td_api::textEntity>>{}),
        nullptr, nullptr);
    if (scheduled) {
        value->scheduling_state_ =
            td_api::make_object<td_api::messageSchedulingStateSendWhenOnline>();
    }
    value->reply_markup_ = td_api::make_object<td_api::replyMarkupForceReply>(true, "field");
    return value;
}

td_api::object_ptr<td_api::messageProperties> all_message_properties() {
    auto value = td_api::make_object<td_api::messageProperties>();
    value->can_add_offer_ = true;
    value->can_add_tasks_ = true;
    value->can_be_approved_ = true;
    value->can_be_copied_ = true;
    value->can_be_copied_to_secret_chat_ = true;
    value->can_be_declined_ = true;
    value->can_be_deleted_only_for_self_ = true;
    value->can_be_deleted_for_all_users_ = true;
    value->can_be_edited_ = true;
    value->can_be_forwarded_ = true;
    value->can_be_paid_ = true;
    value->can_be_pinned_ = true;
    value->can_be_replied_ = true;
    value->can_be_replied_in_another_chat_ = true;
    value->can_be_saved_ = true;
    value->can_be_shared_in_story_ = true;
    value->can_delete_reactions_ = true;
    value->can_edit_media_ = true;
    value->can_edit_scheduling_state_ = true;
    value->can_edit_suggested_post_info_ = true;
    value->can_get_author_ = true;
    value->can_get_embedding_code_ = true;
    value->can_get_link_ = true;
    value->can_get_media_timestamp_links_ = true;
    value->can_get_message_thread_ = true;
    value->can_get_poll_vote_statistics_ = true;
    value->can_get_read_date_ = true;
    value->can_get_statistics_ = true;
    value->can_get_video_advertisements_ = true;
    value->can_get_viewers_ = true;
    value->can_mark_tasks_as_done_ = true;
    value->can_recognize_speech_ = true;
    value->can_report_chat_ = true;
    value->can_report_reactions_ = true;
    value->can_report_supergroup_spam_ = true;
    value->can_set_fact_check_ = true;
    value->has_protected_content_by_current_user_ = true;
    value->has_protected_content_by_other_user_ = true;
    value->need_show_statistics_ = true;
    return value;
}

} // namespace

TEST_CASE("direct native factories match the scripted descriptor boundary exactly",
          "[core][tdlib][direct][factory]") {
    tgcli::test::ScriptedTdRuntime scripted;
    const auto requests = direct_requests();
    REQUIRE(requests.size() == 16);
    for (const auto& request : requests) {
        auto native = detail::make_production_direct_request_for_test(request);
        auto fake = make_scripted(scripted, request);
        REQUIRE(native.function_data());
        REQUIRE(fake.function_data());
        CHECK(*native.function_data() == *fake.function_data());
        CHECK(detail::production_direct_request_matches_for_test(native, request));
    }
}

TEST_CASE("direct planning factories pin identifiers option and parse-mode defaults",
          "[core][tdlib][direct][factory]") {
    tgcli::test::ScriptedTdRuntime scripted;
    auto get_message = detail::make_production_get_message_for_test(-1001, 77);
    CHECK(detail::production_get_message_matches_for_test(get_message, -1001, 77));
    CHECK(*get_message.function_data() == *scripted.make_get_message(-1001, 77).function_data());
    auto properties = detail::make_production_get_message_properties_for_test(-1001, 77);
    CHECK(detail::production_get_message_properties_matches_for_test(properties, -1001, 77));
    CHECK(*properties.function_data() ==
          *scripted.make_get_message_properties(-1001, 77).function_data());
    auto reactions = detail::make_production_get_message_available_reactions_for_test(-1001, 77);
    CHECK(
        detail::production_get_message_available_reactions_matches_for_test(reactions, -1001, 77));
    CHECK(*reactions.function_data() ==
          *scripted.make_get_message_available_reactions(-1001, 77).function_data());
    auto unix_time = detail::make_production_get_unix_time_for_test();
    CHECK(detail::production_get_unix_time_matches_for_test(unix_time));
    CHECK(*unix_time.function_data() == *scripted.make_get_unix_time().function_data());
    auto markdown = detail::make_production_parse_text_entities_for_test(
        "**bold**", TdTextParseMode::MarkdownV2);
    CHECK(detail::production_parse_text_entities_matches_for_test(markdown, "**bold**",
                                                                  TdTextParseMode::MarkdownV2));
    CHECK(*markdown.function_data() ==
          *scripted.make_parse_text_entities("**bold**", TdTextParseMode::MarkdownV2)
               .function_data());
    auto html =
        detail::make_production_parse_text_entities_for_test("<b>bold</b>", TdTextParseMode::Html);
    CHECK(detail::production_parse_text_entities_matches_for_test(html, "<b>bold</b>",
                                                                  TdTextParseMode::Html));

    for (const auto invalid : {std::int64_t{0}, kTdInt53Max + 1}) {
        CHECK_THROWS_AS(detail::make_production_get_message_for_test(invalid, 1),
                        std::invalid_argument);
        CHECK_THROWS_AS(detail::make_production_get_message_for_test(-1, invalid),
                        std::invalid_argument);
    }
}

TEST_CASE("direct message and property conversion uses separate persistable DTOs",
          "[core][tdlib][direct][conversion]") {
    auto planning = convert(TdFunctionKind::GetMessage, message(true));
    const auto* planned = planning.get_if<TdPlanningMessage>();
    REQUIRE(planned != nullptr);
    CHECK(planned->id == 77);
    CHECK(planned->chat_id == -1001);
    CHECK(planned->has_scheduling_state);
    CHECK(planned->has_reply_markup);
    CHECK(planned->content_kind == TdMessageContentKind::Text);
    CHECK(planned->text == "text 🧪");

    auto scheduled = convert(TdFunctionKind::EditMessageText, message(true));
    const auto* scheduled_result = scheduled.get_if<TdMessageWriteResult>();
    REQUIRE(scheduled_result != nullptr);
    CHECK(scheduled_result->scheduled);
    CHECK_FALSE(scheduled_result->date.has_value());

    auto immediate = convert(TdFunctionKind::EditMessageText, message(false));
    const auto* immediate_result = immediate.get_if<TdMessageWriteResult>();
    REQUIRE(immediate_result != nullptr);
    CHECK_FALSE(immediate_result->scheduled);
    CHECK(immediate_result->date == 1'785'924'000);

    for (const auto size : {std::size_t{4'097}, std::size_t{16'385}}) {
        auto oversized = message(false);
        auto& content = static_cast<td_api::messageText&>(*oversized->content_);
        content.text_->text_ = std::string(size, 'x');
        auto rejected = convert(TdFunctionKind::EditMessageText, std::move(oversized));
        REQUIRE(rejected.get_if<TdDirectConversionError>() != nullptr);
        CHECK(rejected.get_if<TdDirectConversionError>()->tdlib_type_id == td_api::message::ID);
    }

    auto converted_properties =
        convert(TdFunctionKind::GetMessageProperties, all_message_properties());
    const auto* properties = converted_properties.get_if<TdMessageProperties>();
    REQUIRE(properties != nullptr);
    constexpr std::array<bool TdMessageProperties::*, 39> fields{
        &TdMessageProperties::can_add_offer,
        &TdMessageProperties::can_add_tasks,
        &TdMessageProperties::can_be_approved,
        &TdMessageProperties::can_be_copied,
        &TdMessageProperties::can_be_copied_to_secret_chat,
        &TdMessageProperties::can_be_declined,
        &TdMessageProperties::can_be_deleted_only_for_self,
        &TdMessageProperties::can_be_deleted_for_all_users,
        &TdMessageProperties::can_be_edited,
        &TdMessageProperties::can_be_forwarded,
        &TdMessageProperties::can_be_paid,
        &TdMessageProperties::can_be_pinned,
        &TdMessageProperties::can_be_replied,
        &TdMessageProperties::can_be_replied_in_another_chat,
        &TdMessageProperties::can_be_saved,
        &TdMessageProperties::can_be_shared_in_story,
        &TdMessageProperties::can_delete_reactions,
        &TdMessageProperties::can_edit_media,
        &TdMessageProperties::can_edit_scheduling_state,
        &TdMessageProperties::can_edit_suggested_post_info,
        &TdMessageProperties::can_get_author,
        &TdMessageProperties::can_get_embedding_code,
        &TdMessageProperties::can_get_link,
        &TdMessageProperties::can_get_media_timestamp_links,
        &TdMessageProperties::can_get_message_thread,
        &TdMessageProperties::can_get_poll_vote_statistics,
        &TdMessageProperties::can_get_read_date,
        &TdMessageProperties::can_get_statistics,
        &TdMessageProperties::can_get_video_advertisements,
        &TdMessageProperties::can_get_viewers,
        &TdMessageProperties::can_mark_tasks_as_done,
        &TdMessageProperties::can_recognize_speech,
        &TdMessageProperties::can_report_chat,
        &TdMessageProperties::can_report_reactions,
        &TdMessageProperties::can_report_supergroup_spam,
        &TdMessageProperties::can_set_fact_check,
        &TdMessageProperties::has_protected_content_by_current_user,
        &TdMessageProperties::has_protected_content_by_other_user,
        &TdMessageProperties::need_show_statistics,
    };
    for (const auto field : fields) {
        CHECK(properties->*field);
    }
}

TEST_CASE("chat conversion preserves all sixteen notification settings fields",
          "[core][tdlib][direct][conversion]") {
    const TdChatNotificationSettings expected{
        .use_default_mute_for = true,
        .mute_for = 17,
        .use_default_sound = true,
        .sound_id = std::numeric_limits<std::int64_t>::min(),
        .use_default_show_preview = true,
        .show_preview = true,
        .use_default_mute_stories = true,
        .mute_stories = true,
        .use_default_story_sound = true,
        .story_sound_id = std::numeric_limits<std::int64_t>::max(),
        .use_default_show_story_poster = true,
        .show_story_poster = true,
        .use_default_disable_pinned_message_notifications = true,
        .disable_pinned_message_notifications = true,
        .use_default_disable_mention_notifications = true,
        .disable_mention_notifications = true,
    };
    auto chat = td_api::make_object<td_api::chat>();
    chat->id_ = -1001;
    chat->type_ = td_api::make_object<td_api::chatTypeSupergroup>(42, false);
    chat->notification_settings_ = td_api::make_object<td_api::chatNotificationSettings>(
        expected.use_default_mute_for, expected.mute_for, expected.use_default_sound,
        expected.sound_id, expected.use_default_show_preview, expected.show_preview,
        expected.use_default_mute_stories, expected.mute_stories, expected.use_default_story_sound,
        expected.story_sound_id, expected.use_default_show_story_poster, expected.show_story_poster,
        expected.use_default_disable_pinned_message_notifications,
        expected.disable_pinned_message_notifications,
        expected.use_default_disable_mention_notifications, expected.disable_mention_notifications);
    auto converted = detail::convert_production_response_for_test(
        TdValue::from(NativeObjectPtr{std::move(chat)}));
    const auto* neutral = converted.get_if<TdChat>();
    REQUIRE(neutral != nullptr);
    REQUIRE(neutral->notification_settings.has_value());
    CHECK(*neutral->notification_settings == expected);
}

TEST_CASE("formatted text conversion covers every pinned entity type",
          "[core][tdlib][direct][conversion]") {
    std::vector<td_api::object_ptr<td_api::TextEntityType>> types;
    types.emplace_back(td_api::make_object<td_api::textEntityTypeMention>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeHashtag>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeCashtag>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeBotCommand>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeUrl>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeEmailAddress>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypePhoneNumber>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeBankCardNumber>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeBold>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeItalic>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeUnderline>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeStrikethrough>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeSpoiler>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeCode>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypePre>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypePreCode>("cpp"));
    types.emplace_back(td_api::make_object<td_api::textEntityTypeBlockQuote>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeExpandableBlockQuote>());
    types.emplace_back(td_api::make_object<td_api::textEntityTypeTextUrl>("https://example.test"));
    types.emplace_back(td_api::make_object<td_api::textEntityTypeMentionName>(42));
    types.emplace_back(td_api::make_object<td_api::textEntityTypeCustomEmoji>(73));
    types.emplace_back(td_api::make_object<td_api::textEntityTypeMediaTimestamp>(91));
    types.emplace_back(td_api::make_object<td_api::textEntityTypeDateTime>(
        123, td_api::make_object<td_api::dateTimeFormattingTypeRelative>()));
    constexpr std::array expected{
        TdTextEntityKind::Mention,     TdTextEntityKind::Hashtag,
        TdTextEntityKind::Cashtag,     TdTextEntityKind::BotCommand,
        TdTextEntityKind::Url,         TdTextEntityKind::EmailAddress,
        TdTextEntityKind::PhoneNumber, TdTextEntityKind::BankCardNumber,
        TdTextEntityKind::Bold,        TdTextEntityKind::Italic,
        TdTextEntityKind::Underline,   TdTextEntityKind::Strikethrough,
        TdTextEntityKind::Spoiler,     TdTextEntityKind::Code,
        TdTextEntityKind::Pre,         TdTextEntityKind::PreCode,
        TdTextEntityKind::BlockQuote,  TdTextEntityKind::ExpandableBlockQuote,
        TdTextEntityKind::TextUrl,     TdTextEntityKind::MentionName,
        TdTextEntityKind::CustomEmoji, TdTextEntityKind::MediaTimestamp,
        TdTextEntityKind::DateTime,
    };
    std::vector<td_api::object_ptr<td_api::textEntity>> entities;
    for (std::size_t index = 0; index < types.size(); ++index) {
        entities.push_back(td_api::make_object<td_api::textEntity>(static_cast<std::int32_t>(index),
                                                                   1, std::move(types[index])));
    }
    auto converted = convert(
        TdFunctionKind::ParseTextEntities,
        td_api::make_object<td_api::formattedText>("abcdefghijklmnopqrstuvw", std::move(entities)));
    const auto* text = converted.get_if<TdFormattedText>();
    REQUIRE(text != nullptr);
    REQUIRE(text->entities.size() == expected.size());
    auto entity = text->entities.begin();
    for (const auto kind : expected) {
        REQUIRE(entity != text->entities.end());
        CHECK(entity->kind == kind);
        ++entity;
    }
    CHECK(text->entities[15].value == "cpp");
    CHECK(text->entities[18].value == "https://example.test");
    CHECK(text->entities[19].numeric_value == 42);
    CHECK(text->entities[20].numeric_value == 73);
    CHECK(text->entities[21].numeric_value == 91);
    CHECK(text->entities[22].numeric_value == 123);
    REQUIRE(text->entities[22].date_time_formatting.has_value());
    CHECK(std::holds_alternative<TdDateTimeFormattingRelative>(
        *text->entities[22].date_time_formatting));
}

TEST_CASE("formatted DateTime conversion preserves every absolute formatting field",
          "[core][tdlib][direct][conversion]") {
    std::vector<td_api::object_ptr<td_api::textEntity>> entities;
    entities.emplace_back(td_api::make_object<td_api::textEntity>(
        0, 1,
        td_api::make_object<td_api::textEntityTypeDateTime>(
            123, td_api::make_object<td_api::dateTimeFormattingTypeAbsolute>(
                     td_api::make_object<td_api::dateTimePartPrecisionNone>(),
                     td_api::make_object<td_api::dateTimePartPrecisionShort>(), true))));
    entities.emplace_back(td_api::make_object<td_api::textEntity>(
        1, 1,
        td_api::make_object<td_api::textEntityTypeDateTime>(
            456, td_api::make_object<td_api::dateTimeFormattingTypeAbsolute>(
                     td_api::make_object<td_api::dateTimePartPrecisionLong>(),
                     td_api::make_object<td_api::dateTimePartPrecisionNone>(), false))));
    auto converted = convert(TdFunctionKind::ParseTextEntities,
                             td_api::make_object<td_api::formattedText>("xy", std::move(entities)));
    const auto* text = converted.get_if<TdFormattedText>();
    REQUIRE(text != nullptr);
    REQUIRE(text->entities.size() == 2);
    REQUIRE(text->entities[0].date_time_formatting.has_value());
    REQUIRE(text->entities[1].date_time_formatting.has_value());
    const auto* first =
        std::get_if<TdDateTimeFormattingAbsolute>(&*text->entities[0].date_time_formatting);
    const auto* second =
        std::get_if<TdDateTimeFormattingAbsolute>(&*text->entities[1].date_time_formatting);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->time_precision == TdDateTimePartPrecision::None);
    CHECK(first->date_precision == TdDateTimePartPrecision::Short);
    CHECK(first->show_day_of_week);
    CHECK(second->time_precision == TdDateTimePartPrecision::Long);
    CHECK(second->date_precision == TdDateTimePartPrecision::None);
    CHECK_FALSE(second->show_day_of_week);
}

TEST_CASE("formatted text conversion rejects unknown and malformed nested entity types",
          "[core][tdlib][direct][conversion]") {
    SECTION("unknown entity") {
        std::vector<td_api::object_ptr<td_api::textEntity>> entities;
        entities.emplace_back(td_api::make_object<td_api::textEntity>(
            0, 1, td_api::make_object<UnsupportedTextEntityType>()));
        auto converted =
            convert(TdFunctionKind::ParseTextEntities,
                    td_api::make_object<td_api::formattedText>("x", std::move(entities)));
        CHECK(converted.get_if<TdDirectConversionError>() != nullptr);
    }

    SECTION("DateTime without formatting") {
        std::vector<td_api::object_ptr<td_api::textEntity>> entities;
        entities.emplace_back(td_api::make_object<td_api::textEntity>(
            0, 1, td_api::make_object<td_api::textEntityTypeDateTime>(123, nullptr)));
        auto converted =
            convert(TdFunctionKind::ParseTextEntities,
                    td_api::make_object<td_api::formattedText>("x", std::move(entities)));
        CHECK(converted.get_if<TdDirectConversionError>() != nullptr);
    }

    SECTION("DateTime with unknown formatting") {
        std::vector<td_api::object_ptr<td_api::textEntity>> entities;
        entities.emplace_back(td_api::make_object<td_api::textEntity>(
            0, 1,
            td_api::make_object<td_api::textEntityTypeDateTime>(
                123, td_api::make_object<UnsupportedDateTimeFormattingType>())));
        auto converted =
            convert(TdFunctionKind::ParseTextEntities,
                    td_api::make_object<td_api::formattedText>("x", std::move(entities)));
        CHECK(converted.get_if<TdDirectConversionError>() != nullptr);
    }

    SECTION("absolute DateTime with malformed precision") {
        std::vector<td_api::object_ptr<td_api::textEntity>> entities;
        entities.emplace_back(td_api::make_object<td_api::textEntity>(
            0, 1,
            td_api::make_object<td_api::textEntityTypeDateTime>(
                123, td_api::make_object<td_api::dateTimeFormattingTypeAbsolute>(
                         td_api::make_object<UnsupportedDateTimePartPrecision>(),
                         td_api::make_object<td_api::dateTimePartPrecisionLong>(), false))));
        auto converted =
            convert(TdFunctionKind::ParseTextEntities,
                    td_api::make_object<td_api::formattedText>("x", std::move(entities)));
        CHECK(converted.get_if<TdDirectConversionError>() != nullptr);
    }

    SECTION("absolute DateTime with missing precision") {
        std::vector<td_api::object_ptr<td_api::textEntity>> entities;
        entities.emplace_back(td_api::make_object<td_api::textEntity>(
            0, 1,
            td_api::make_object<td_api::textEntityTypeDateTime>(
                123,
                td_api::make_object<td_api::dateTimeFormattingTypeAbsolute>(
                    nullptr, td_api::make_object<td_api::dateTimePartPrecisionLong>(), false))));
        auto converted =
            convert(TdFunctionKind::ParseTextEntities,
                    td_api::make_object<td_api::formattedText>("x", std::move(entities)));
        CHECK(converted.get_if<TdDirectConversionError>() != nullptr);
    }
}

TEST_CASE("production and scripted direct factories reject the same invalid requests",
          "[core][tdlib][direct][fake-boundary]") {
    const std::vector<TdDirectRequest> invalid{
        TdEditMessageTextRequest{.chat_id = 0, .message_id = 10, .text = "edit"},
        TdEditMessageTextRequest{.chat_id = kTdInt53Max + 1, .message_id = 10, .text = "edit"},
        TdEditMessageTextRequest{.chat_id = -1001, .message_id = kTdInt53Max + 1, .text = "edit"},
        TdDeleteMessagesRequest{.chat_id = -1001, .message_ids = {}, .revoke = false},
        TdDeleteMessagesRequest{.chat_id = -1001, .message_ids = {2, 1}, .revoke = false},
        TdDeleteMessagesRequest{
            .chat_id = -1001, .message_ids = std::vector<std::int64_t>(101, 1), .revoke = false},
        TdMessageReactionRequest{
            .chat_id = -1001, .message_id = 10, .reaction = {}, .remove = false, .big = false},
        TdMessageReactionRequest{.chat_id = -1001,
                                 .message_id = 10,
                                 .reaction = std::string(65, 'x'),
                                 .remove = false,
                                 .big = false},
        TdMessageReactionRequest{.chat_id = -1001,
                                 .message_id = 10,
                                 .reaction = std::string("\xC3", 1),
                                 .remove = false,
                                 .big = false},
        TdMessageReactionRequest{
            .chat_id = -1001, .message_id = 10, .reaction = "x", .remove = true, .big = true},
        TdViewMessagesRequest{.chat_id = -1001, .message_ids = {}},
        TdViewMessagesRequest{.chat_id = -1001, .message_ids = {10, 11}},
        TdSetChatNotificationSettingsRequest{
            .chat_id = -1001, .settings = {.use_default_mute_for = false, .mute_for = -1}},
        TdJoinChatRequest{.chat_id = -1001, .invite_link = "https://t.me/+token"},
        TdJoinChatRequest{.chat_id = std::nullopt, .invite_link = std::nullopt},
        TdJoinChatRequest{.chat_id = std::nullopt, .invite_link = std::string{}},
        TdLeaveChatRequest{.chat_id = 0},
    };
    tgcli::test::ScriptedTdRuntime runtime;
    for (const auto& request : invalid) {
        INFO(request.index());
        CHECK_THROWS_AS(detail::make_production_direct_request_for_test(request),
                        std::invalid_argument);
        CHECK_THROWS_AS(make_scripted(runtime, request), std::invalid_argument);
    }
    CHECK(runtime.sent_functions().empty());

    CHECK_THROWS_AS(detail::make_production_get_message_for_test(0, 10), std::invalid_argument);
    CHECK_THROWS_AS(runtime.make_get_message(0, 10), std::invalid_argument);
    CHECK_THROWS_AS(detail::make_production_get_message_properties_for_test(-1001, 0),
                    std::invalid_argument);
    CHECK_THROWS_AS(runtime.make_get_message_properties(-1001, 0), std::invalid_argument);
    CHECK_THROWS_AS(
        detail::make_production_get_message_available_reactions_for_test(kTdInt53Max + 1, 10),
        std::invalid_argument);
    CHECK_THROWS_AS(runtime.make_get_message_available_reactions(kTdInt53Max + 1, 10),
                    std::invalid_argument);
}

TEST_CASE("available reactions option formatted text and join variants convert neutrally",
          "[core][tdlib][direct][conversion]") {
    std::vector<td_api::object_ptr<td_api::availableReaction>> top;
    top.push_back(td_api::make_object<td_api::availableReaction>(
        td_api::make_object<td_api::reactionTypeEmoji>("👍🏽"), true));
    std::vector<td_api::object_ptr<td_api::availableReaction>> recent;
    recent.push_back(td_api::make_object<td_api::availableReaction>(
        td_api::make_object<td_api::reactionTypeCustomEmoji>(99), false));
    std::vector<td_api::object_ptr<td_api::availableReaction>> popular;
    popular.push_back(td_api::make_object<td_api::availableReaction>(
        td_api::make_object<td_api::reactionTypePaid>(), false));
    auto available = td_api::make_object<td_api::availableReactions>(
        std::move(top), std::move(recent), std::move(popular), true, false,
        td_api::make_object<td_api::reactionUnavailabilityReasonGuest>());
    auto converted = convert(TdFunctionKind::GetMessageAvailableReactions, std::move(available));
    const auto* reactions = converted.get_if<TdMessageAvailableReactions>();
    REQUIRE(reactions != nullptr);
    REQUIRE(reactions->top.size() == 1);
    CHECK(reactions->top[0].type.emoji == "👍🏽");
    CHECK(reactions->top[0].needs_premium);
    CHECK(reactions->recent[0].type.custom_emoji_id == 99);
    CHECK(reactions->popular[0].type.kind == TdReactionKind::Paid);
    CHECK(reactions->allow_custom_emoji);
    CHECK(reactions->unavailability_reason == TdReactionUnavailabilityReason::Guest);

    auto option = convert(TdFunctionKind::GetOption,
                          td_api::make_object<td_api::optionValueInteger>(1'785'924'000));
    REQUIRE(option.get_if<TdOptionInteger>() != nullptr);
    CHECK(option.get_if<TdOptionInteger>()->value == 1'785'924'000);

    std::vector<td_api::object_ptr<td_api::textEntity>> entities;
    entities.push_back(td_api::make_object<td_api::textEntity>(
        0, 4, td_api::make_object<td_api::textEntityTypeBold>()));
    entities.push_back(td_api::make_object<td_api::textEntity>(
        5, 4, td_api::make_object<td_api::textEntityTypeTextUrl>("https://example.test")));
    entities.push_back(td_api::make_object<td_api::textEntity>(
        10, 2, td_api::make_object<td_api::textEntityTypeCustomEmoji>(123)));
    auto formatted =
        convert(TdFunctionKind::ParseTextEntities,
                td_api::make_object<td_api::formattedText>("bold link xx", std::move(entities)));
    const auto* text = formatted.get_if<TdFormattedText>();
    REQUIRE(text != nullptr);
    REQUIRE(text->entities.size() == 3);
    CHECK(text->entities[0].kind == TdTextEntityKind::Bold);
    CHECK(text->entities[1].kind == TdTextEntityKind::TextUrl);
    CHECK(text->entities[1].value == "https://example.test");
    CHECK(text->entities[2].numeric_value == 123);

    auto joined = convert(TdFunctionKind::JoinChat,
                          td_api::make_object<td_api::chatJoinResultSuccess>(-1001));
    REQUIRE(joined.get_if<TdChatJoinResult>() != nullptr);
    CHECK(joined.get_if<TdChatJoinResult>()->kind == TdChatJoinResultKind::Success);
    CHECK(joined.get_if<TdChatJoinResult>()->chat_id == -1001);
    auto requested = convert(TdFunctionKind::JoinChatByInviteLink,
                             td_api::make_object<td_api::chatJoinResultRequestSent>());
    CHECK(requested.get_if<TdChatJoinResult>()->kind == TdChatJoinResultKind::RequestSent);
    auto guarded = convert(TdFunctionKind::JoinChatByInviteLink,
                           td_api::make_object<td_api::chatJoinResultGuardBotApprovalRequired>(
                               42, td_api::make_object<td_api::webAppUrl>(), 73));
    CHECK(guarded.get_if<TdChatJoinResult>()->kind ==
          TdChatJoinResultKind::GuardBotApprovalRequired);
    CHECK(guarded.get_if<TdChatJoinResult>()->guard_bot_user_id == 42);
    CHECK(guarded.get_if<TdChatJoinResult>()->guard_query_id == 73);
    auto declined =
        convert(TdFunctionKind::JoinChat, td_api::make_object<td_api::chatJoinResultDeclined>());
    CHECK(declined.get_if<TdChatJoinResult>()->kind == TdChatJoinResultKind::Declined);

    auto wrong = convert(TdFunctionKind::GetOption, td_api::make_object<td_api::ok>());
    REQUIRE(wrong.get_if<TdDirectConversionError>() != nullptr);
    CHECK(wrong.get_if<TdDirectConversionError>()->tdlib_type_id == td_api::ok::ID);
}
