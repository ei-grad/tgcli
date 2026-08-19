#include "core/td_runtime_test_adapter.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

td_api::object_ptr<td_api::formattedText> text(const std::string& value) {
    return td_api::make_object<td_api::formattedText>(
        value, std::vector<td_api::object_ptr<td_api::textEntity>>{});
}

TdMessageSummary convert_message(td_api::object_ptr<td_api::MessageContent> content,
                                 td_api::object_ptr<td_api::MessageSender> sender,
                                 td_api::object_ptr<td_api::MessageTopic> topic = nullptr) {
    auto message = td_api::make_object<td_api::message>();
    message->id_ = 123;
    message->chat_id_ = -1001;
    message->date_ = 1785924000;
    message->sender_id_ = std::move(sender);
    message->is_outgoing_ = true;
    message->topic_id_ = std::move(topic);
    message->content_ = std::move(content);

    auto chat = td_api::make_object<td_api::chat>();
    chat->id_ = -1001;
    chat->title_ = "Project";
    chat->type_ = td_api::make_object<td_api::chatTypeBasicGroup>(7);
    chat->last_message_ = std::move(message);
    td_api::object_ptr<td_api::Object> native = std::move(chat);
    auto converted = detail::convert_production_response_for_test(TdValue::from(std::move(native)));
    const auto* value = converted.get_if<TdChat>();
    REQUIRE(value != nullptr);
    REQUIRE(value->last_message.has_value());
    return *value->last_message;
}

td_api::object_ptr<td_api::MessageSender> user_sender() {
    return td_api::make_object<td_api::messageSenderUser>(42);
}

} // namespace

TEST_CASE("production chat conversion preserves lists positions counters and last message",
          "[chats][core][tdlib][td-runtime-converter]") {
    auto message = td_api::make_object<td_api::message>();
    message->id_ = 123;
    message->chat_id_ = -1001;
    message->date_ = 1785924000;
    message->sender_id_ = td_api::make_object<td_api::messageSenderChat>(-1002);
    message->topic_id_ = td_api::make_object<td_api::messageTopicDirectMessages>(77);
    message->content_ =
        td_api::make_object<td_api::messageText>(text("experiment result"), nullptr, nullptr);

    auto chat = td_api::make_object<td_api::chat>();
    chat->id_ = -1001;
    chat->title_ = "Project";
    chat->type_ = td_api::make_object<td_api::chatTypeSupergroup>(55, false);
    chat->positions_.push_back(td_api::make_object<td_api::chatPosition>(
        td_api::make_object<td_api::chatListMain>(), 900, false, nullptr));
    chat->positions_.push_back(td_api::make_object<td_api::chatPosition>(
        td_api::make_object<td_api::chatListFolder>(2), 800, true, nullptr));
    chat->chat_lists_.emplace_back(td_api::make_object<td_api::chatListArchive>());
    chat->chat_lists_.emplace_back(td_api::make_object<td_api::chatListFolder>(2));
    chat->is_marked_as_unread_ = true;
    chat->unread_count_ = 3;
    chat->unread_mention_count_ = 1;
    chat->unread_reaction_count_ = 2;
    chat->unread_poll_vote_count_ = 4;
    chat->last_message_ = std::move(message);

    td_api::object_ptr<td_api::Object> native = std::move(chat);
    auto converted = detail::convert_production_response_for_test(TdValue::from(std::move(native)));
    const auto* value = converted.get_if<TdChat>();
    REQUIRE(value != nullptr);
    CHECK(value->positions ==
          std::vector<TdChatPosition>{{.list = {.kind = TdChatListKind::Main,
                                                .folder_id = 0,
                                                .tdlib_type_id = td_api::chatListMain::ID},
                                       .order = 900},
                                      {.list = {.kind = TdChatListKind::Folder,
                                                .folder_id = 2,
                                                .tdlib_type_id = td_api::chatListFolder::ID},
                                       .order = 800}});
    CHECK(value->chat_lists ==
          std::vector<TdChatList>{{.kind = TdChatListKind::Archive,
                                   .folder_id = 0,
                                   .tdlib_type_id = td_api::chatListArchive::ID},
                                  {.kind = TdChatListKind::Folder,
                                   .folder_id = 2,
                                   .tdlib_type_id = td_api::chatListFolder::ID}});
    CHECK(value->is_marked_unread);
    CHECK(value->unread_count == 3);
    CHECK(value->unread_mention_count == 1);
    CHECK(value->unread_reaction_count == 2);
    CHECK(value->unread_poll_vote_count == 4);
    REQUIRE(value->last_message);
    CHECK(value->last_message->sender.kind == TdMessageSenderKind::Chat);
    CHECK(value->last_message->sender.id == -1002);
    CHECK(value->last_message->topic ==
          TdTopic{.kind = TdTopicKind::Direct,
                  .id = 77,
                  .tdlib_type_id = td_api::messageTopicDirectMessages::ID});
    CHECK(value->last_message->content_kind == TdMessageContentKind::Text);
    CHECK(value->last_message->text == "experiment result");
}

TEST_CASE("production message conversion covers all curated content variants",
          "[chats][core][tdlib][td-runtime-converter]") {
    auto text_content = td_api::make_object<td_api::messageText>(text("text"), nullptr, nullptr);
    auto converted = convert_message(std::move(text_content), user_sender());
    CHECK(converted.content_kind == TdMessageContentKind::Text);
    CHECK(converted.text == "text");

    auto emoji = td_api::make_object<td_api::messageAnimatedEmoji>(nullptr, "🧪");
    converted = convert_message(std::move(emoji), user_sender());
    CHECK(converted.content_kind == TdMessageContentKind::Text);
    CHECK(converted.text == "🧪");

    auto photo = td_api::make_object<td_api::messagePhoto>();
    photo->caption_ = text("photo caption");
    converted = convert_message(std::move(photo), user_sender());
    CHECK(converted.content_kind == TdMessageContentKind::Photo);
    CHECK(converted.text == "photo caption");

    auto video = td_api::make_object<td_api::messageVideo>();
    video->caption_ = text("video caption");
    converted = convert_message(std::move(video), user_sender());
    CHECK(converted.content_kind == TdMessageContentKind::Video);
    CHECK(converted.text == "video caption");

    auto document = td_api::make_object<td_api::messageDocument>();
    document->caption_ = text("document caption");
    converted = convert_message(std::move(document), user_sender());
    CHECK(converted.content_kind == TdMessageContentKind::Document);
    CHECK(converted.text == "document caption");

    auto voice = td_api::make_object<td_api::messageVoiceNote>();
    voice->caption_ = text("voice caption");
    converted = convert_message(std::move(voice), user_sender());
    CHECK(converted.content_kind == TdMessageContentKind::Voice);
    CHECK(converted.text == "voice caption");

    converted = convert_message(td_api::make_object<td_api::messageDice>(), user_sender());
    CHECK(converted.content_kind == TdMessageContentKind::Other);
    CHECK(converted.text.empty());
}

TEST_CASE("production message conversion preserves every tagged topic variant",
          "[chats][core][tdlib][td-runtime-converter]") {
    struct TopicCase {
        td_api::object_ptr<td_api::MessageTopic> native;
        TdTopic expected;
    };
    std::vector<TopicCase> cases;
    cases.push_back(
        {td_api::make_object<td_api::messageTopicForum>(7),
         {.kind = TdTopicKind::Forum, .id = 7, .tdlib_type_id = td_api::messageTopicForum::ID}});
    cases.push_back(
        {td_api::make_object<td_api::messageTopicThread>(8),
         {.kind = TdTopicKind::Thread, .id = 8, .tdlib_type_id = td_api::messageTopicThread::ID}});
    cases.push_back({td_api::make_object<td_api::messageTopicDirectMessages>(9),
                     {.kind = TdTopicKind::Direct,
                      .id = 9,
                      .tdlib_type_id = td_api::messageTopicDirectMessages::ID}});
    cases.push_back({td_api::make_object<td_api::messageTopicSavedMessages>(10),
                     {.kind = TdTopicKind::Saved,
                      .id = 10,
                      .tdlib_type_id = td_api::messageTopicSavedMessages::ID}});
    for (auto& test_case : cases) {
        auto content = td_api::make_object<td_api::messageText>(text("message"), nullptr, nullptr);
        const auto converted =
            convert_message(std::move(content), user_sender(), std::move(test_case.native));
        REQUIRE(converted.topic);
        CHECK(*converted.topic == test_case.expected);
    }
}
