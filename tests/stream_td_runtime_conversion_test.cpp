#include "core/td_runtime_test_adapter.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

TdValue convert(NativeObjectPtr object, std::uint64_t generation = 7) {
    return detail::convert_production_update_for_test(TdValue::from(std::move(object)), generation);
}

td_api::object_ptr<td_api::message> message(std::int64_t id = 123, std::int64_t chat_id = -1001) {
    auto value = td_api::make_object<td_api::message>();
    value->id_ = id;
    value->chat_id_ = chat_id;
    value->date_ = 1'785'924'000;
    value->sender_id_ = td_api::make_object<td_api::messageSenderUser>(42);
    value->content_ = td_api::make_object<td_api::messageText>(
        td_api::make_object<td_api::formattedText>(
            "experiment", std::vector<td_api::object_ptr<td_api::textEntity>>{}),
        nullptr, nullptr);
    return value;
}

td_api::object_ptr<td_api::user> user(std::int64_t id = 42) {
    auto value = td_api::make_object<td_api::user>();
    value->id_ = id;
    value->first_name_ = "Ada";
    value->last_name_ = "Lovelace";
    value->usernames_ = td_api::make_object<td_api::usernames>(std::vector<std::string>{"ada"},
                                                               std::vector<std::string>{}, "",
                                                               std::vector<std::string>{});
    value->type_ = td_api::make_object<td_api::userTypeRegular>();
    return value;
}

td_api::object_ptr<td_api::chat> chat(std::int64_t id = -1001) {
    auto value = td_api::make_object<td_api::chat>();
    value->id_ = id;
    value->title_ = "Project";
    value->type_ = td_api::make_object<td_api::chatTypeSupergroup>(55, false);
    value->last_message_ = message(123, id);
    return value;
}

const TdMalformedSupportedUpdate* malformed(const TdValue& value, TdSupportedUpdateKind kind) {
    const auto* result = value.get_if<TdMalformedSupportedUpdate>();
    REQUIRE(result != nullptr);
    CHECK(result->kind == kind);
    return result;
}

} // namespace

TEST_CASE("production stream conversion preserves message edit and reaction update variants",
          "[stream][core][tdlib][td-runtime-converter]") {
    auto converted = convert(td_api::make_object<td_api::updateNewMessage>(message()));
    const auto* added = converted.get_if<TdUpdateNewMessage>();
    REQUIRE(added != nullptr);
    CHECK(added->message.id == 123);
    CHECK(added->message.text == "experiment");

    converted = convert(td_api::make_object<td_api::updateMessageContent>(
        -1001, 123,
        td_api::make_object<td_api::messagePhoto>(
            nullptr, nullptr,
            td_api::make_object<td_api::formattedText>(
                "replacement", std::vector<td_api::object_ptr<td_api::textEntity>>{}),
            false, false, false)));
    const auto* content = converted.get_if<TdUpdateMessageContent>();
    REQUIRE(content != nullptr);
    CHECK(content->chat_id == -1001);
    CHECK(content->message_id == 123);
    CHECK(content->content.kind == TdMessageContentKind::Photo);
    CHECK(content->content.text == "replacement");

    converted = convert(
        td_api::make_object<td_api::updateMessageEdited>(-1001, 123, 1'785'924'001, nullptr));
    const auto* edited = converted.get_if<TdUpdateMessageEdited>();
    REQUIRE(edited != nullptr);
    CHECK(edited->edit_date == 1'785'924'001);
    CHECK_FALSE(edited->has_reply_markup);

    auto snapshot = td_api::make_object<td_api::messageReactions>();
    snapshot->are_tags_ = true;
    snapshot->can_get_added_reactions_ = true;
    std::vector<td_api::object_ptr<td_api::MessageSender>> recent_senders;
    recent_senders.emplace_back(td_api::make_object<td_api::messageSenderChat>(-1002));
    snapshot->reactions_.push_back(td_api::make_object<td_api::messageReaction>(
        td_api::make_object<td_api::reactionTypeEmoji>("🧪"), 3, true,
        td_api::make_object<td_api::messageSenderUser>(42), std::move(recent_senders)));
    auto interaction = td_api::make_object<td_api::messageInteractionInfo>();
    interaction->reactions_ = std::move(snapshot);
    converted = convert(td_api::make_object<td_api::updateMessageInteractionInfo>(
        -1001, 123, std::move(interaction)));
    const auto* user_reactions = converted.get_if<TdUpdateMessageInteractionInfo>();
    REQUIRE(user_reactions != nullptr);
    REQUIRE(user_reactions->reactions);
    CHECK(user_reactions->reactions->are_tags);
    CHECK(user_reactions->reactions->can_get_added_reactions);
    REQUIRE(user_reactions->reactions->items.size() == 1);
    CHECK(user_reactions->reactions->items.front().reaction.kind == TdReactionKind::Emoji);
    CHECK(user_reactions->reactions->items.front().total_count == 3);
    REQUIRE(user_reactions->reactions->items.front().used_sender);
    CHECK(user_reactions->reactions->items.front().used_sender->id == 42);

    converted =
        convert(td_api::make_object<td_api::updateMessageInteractionInfo>(-1001, 123, nullptr));
    const auto* null_reactions = converted.get_if<TdUpdateMessageInteractionInfo>();
    REQUIRE(null_reactions != nullptr);
    CHECK_FALSE(null_reactions->reactions);

    std::vector<td_api::object_ptr<td_api::ReactionType>> old_reactions;
    old_reactions.emplace_back(td_api::make_object<td_api::reactionTypePaid>());
    std::vector<td_api::object_ptr<td_api::ReactionType>> new_reactions;
    new_reactions.emplace_back(td_api::make_object<td_api::reactionTypeCustomEmoji>(999));
    converted = convert(td_api::make_object<td_api::updateMessageReaction>(
        -1001, 123, td_api::make_object<td_api::messageSenderUser>(42), 1'785'924'002,
        std::move(old_reactions), std::move(new_reactions)));
    const auto* bot_delta = converted.get_if<TdUpdateMessageReaction>();
    REQUIRE(bot_delta != nullptr);
    CHECK(bot_delta->actor.id == 42);
    CHECK(bot_delta->old_reactions.front().kind == TdReactionKind::Paid);
    CHECK(bot_delta->new_reactions.front().custom_emoji_id == 999);

    std::vector<td_api::object_ptr<td_api::messageReaction>> counts;
    counts.push_back(td_api::make_object<td_api::messageReaction>(
        td_api::make_object<td_api::reactionTypeEmoji>("👍"), 4, false, nullptr,
        std::vector<td_api::object_ptr<td_api::MessageSender>>{}));
    converted = convert(td_api::make_object<td_api::updateMessageReactions>(
        -1001, 123, 1'785'924'003, std::move(counts)));
    const auto* bot_snapshot = converted.get_if<TdUpdateMessageReactions>();
    REQUIRE(bot_snapshot != nullptr);
    REQUIRE(bot_snapshot->reactions.size() == 1);
    CHECK(bot_snapshot->reactions.front().total_count == 4);
}

TEST_CASE("production stream conversion preserves entity chat delete and counter variants",
          "[stream][core][tdlib][td-runtime-converter]") {
    auto converted = convert(td_api::make_object<td_api::updateUser>(user()));
    REQUIRE(converted.get_if<TdUpdateUser>() != nullptr);
    CHECK(converted.get_if<TdUpdateUser>()->user.id == 42);

    auto basic = td_api::make_object<td_api::basicGroup>();
    basic->id_ = 51;
    basic->member_count_ = 8;
    basic->is_active_ = true;
    converted = convert(td_api::make_object<td_api::updateBasicGroup>(std::move(basic)));
    REQUIRE(converted.get_if<TdUpdateBasicGroup>() != nullptr);
    CHECK(converted.get_if<TdUpdateBasicGroup>()->basic_group.member_count == 8);

    auto supergroup = td_api::make_object<td_api::supergroup>();
    supergroup->id_ = 55;
    supergroup->is_channel_ = false;
    supergroup->is_forum_ = true;
    supergroup->usernames_ = td_api::make_object<td_api::usernames>(
        std::vector<std::string>{"project"}, std::vector<std::string>{}, "",
        std::vector<std::string>{});
    converted = convert(td_api::make_object<td_api::updateSupergroup>(std::move(supergroup)));
    REQUIRE(converted.get_if<TdUpdateSupergroup>() != nullptr);
    CHECK(converted.get_if<TdUpdateSupergroup>()->supergroup.is_forum);

    converted = convert(td_api::make_object<td_api::updateNewChat>(chat()));
    REQUIRE(converted.get_if<TdUpdateNewChat>() != nullptr);
    CHECK(converted.get_if<TdUpdateNewChat>()->chat.title == "Project");

    converted = convert(td_api::make_object<td_api::updateChatTitle>(-1001, "Renamed"));
    REQUIRE(converted.get_if<TdUpdateChatTitle>() != nullptr);
    CHECK(converted.get_if<TdUpdateChatTitle>()->title == "Renamed");

    converted = convert(td_api::make_object<td_api::updateChatLastMessage>(
        -1001, message(), std::vector<td_api::object_ptr<td_api::chatPosition>>{}));
    REQUIRE(converted.get_if<TdUpdateChatLastMessage>() != nullptr);
    REQUIRE(converted.get_if<TdUpdateChatLastMessage>()->last_message);

    converted = convert(td_api::make_object<td_api::updateChatAddedToList>(
        -1001, td_api::make_object<td_api::chatListFolder>(2)));
    REQUIRE(converted.get_if<TdUpdateChatAddedToList>() != nullptr);
    CHECK(converted.get_if<TdUpdateChatAddedToList>()->list.folder_id == 2);

    converted = convert(td_api::make_object<td_api::updateChatRemovedFromList>(
        -1001, td_api::make_object<td_api::chatListArchive>()));
    REQUIRE(converted.get_if<TdUpdateChatRemovedFromList>() != nullptr);
    CHECK(converted.get_if<TdUpdateChatRemovedFromList>()->list.kind == TdChatListKind::Archive);

    converted = convert(td_api::make_object<td_api::updateChatReadInbox>(-1001, 123, 7));
    REQUIRE(converted.get_if<TdUpdateChatReadInbox>() != nullptr);
    CHECK(converted.get_if<TdUpdateChatReadInbox>()->unread_count == 7);

    converted = convert(td_api::make_object<td_api::updateMessageMentionRead>(-1001, 123, 6));
    REQUIRE(converted.get_if<TdUpdateMessageMentionRead>() != nullptr);
    converted = convert(td_api::make_object<td_api::updateMessageUnreadReactions>(
        -1001, 123, std::vector<td_api::object_ptr<td_api::unreadReaction>>{}, 5));
    REQUIRE(converted.get_if<TdUpdateMessageUnreadReactions>() != nullptr);
    converted = convert(
        td_api::make_object<td_api::updateMessageContainsUnreadPollVotes>(-1001, 123, true, 4));
    REQUIRE(converted.get_if<TdUpdateMessageContainsUnreadPollVotes>() != nullptr);
    converted = convert(td_api::make_object<td_api::updateChatUnreadMentionCount>(-1001, 3));
    REQUIRE(converted.get_if<TdUpdateChatUnreadMentionCount>() != nullptr);
    converted = convert(td_api::make_object<td_api::updateChatUnreadReactionCount>(-1001, 2));
    REQUIRE(converted.get_if<TdUpdateChatUnreadReactionCount>() != nullptr);
    converted = convert(td_api::make_object<td_api::updateChatUnreadPollVoteCount>(-1001, 1));
    REQUIRE(converted.get_if<TdUpdateChatUnreadPollVoteCount>() != nullptr);
    converted = convert(td_api::make_object<td_api::updateChatIsMarkedAsUnread>(-1001, true));
    REQUIRE(converted.get_if<TdUpdateChatIsMarkedAsUnread>() != nullptr);

    converted = convert(td_api::make_object<td_api::updateDeleteMessages>(
        -1001, std::vector<std::int64_t>{123, 124}, true, false));
    const auto* deleted = converted.get_if<TdUpdateDeleteMessages>();
    REQUIRE(deleted != nullptr);
    CHECK(deleted->client_generation == 7);
    CHECK(deleted->message_ids == std::vector<std::int64_t>{123, 124});
}

TEST_CASE("malformed supported stream updates remain typed while unsupported updates stay ignored",
          "[stream][core][tdlib][td-runtime-converter][safety]") {
    malformed(convert(td_api::make_object<td_api::updateNewMessage>(nullptr)),
              TdSupportedUpdateKind::NewMessage);
    malformed(convert(td_api::make_object<td_api::updateMessageContent>(-1001, 123, nullptr)),
              TdSupportedUpdateKind::MessageContent);
    malformed(convert(td_api::make_object<td_api::updateMessageReaction>(
                  -1001, 123, nullptr, 0, std::vector<td_api::object_ptr<td_api::ReactionType>>{},
                  std::vector<td_api::object_ptr<td_api::ReactionType>>{})),
              TdSupportedUpdateKind::MessageReaction);
    malformed(convert(td_api::make_object<td_api::updateUser>(nullptr)),
              TdSupportedUpdateKind::User);
    malformed(convert(td_api::make_object<td_api::updateBasicGroup>(nullptr)),
              TdSupportedUpdateKind::BasicGroup);
    malformed(convert(td_api::make_object<td_api::updateSupergroup>(nullptr)),
              TdSupportedUpdateKind::Supergroup);
    malformed(convert(td_api::make_object<td_api::updateNewChat>(nullptr)),
              TdSupportedUpdateKind::NewChat);
    malformed(convert(td_api::make_object<td_api::updateChatAddedToList>(-1001, nullptr)),
              TdSupportedUpdateKind::ChatAddedToList);
    malformed(convert(td_api::make_object<td_api::updateMessageEdited>(-1001, 123, -1, nullptr)),
              TdSupportedUpdateKind::MessageEdited);
    malformed(convert(td_api::make_object<td_api::updateDeleteMessages>(
                  -1001, std::vector<std::int64_t>{0}, true, false)),
              TdSupportedUpdateKind::DeleteMessages);
    malformed(convert(td_api::make_object<td_api::updateChatUnreadReactionCount>(-1001, -1)),
              TdSupportedUpdateKind::ChatUnreadReactionCount);

    auto unsupported = convert(td_api::make_object<td_api::updateChatPhoto>(-1001, nullptr));
    CHECK(unsupported.get_if<TdMalformedSupportedUpdate>() == nullptr);
    CHECK(unsupported.get_if<TdUpdateChatTitle>() == nullptr);
}

TEST_CASE("getCurrentState returns only typed supported inputs and retains malformed entries",
          "[stream][core][tdlib][td-runtime-converter][bootstrap]") {
    std::vector<td_api::object_ptr<td_api::Update>> items;
    items.emplace_back(td_api::make_object<td_api::updateChatTitle>(-1001, "Current"));
    items.emplace_back(td_api::make_object<td_api::updateChatPhoto>(-1001, nullptr));
    items.emplace_back(td_api::make_object<td_api::updateNewChat>(nullptr));
    td_api::object_ptr<td_api::Object> native =
        td_api::make_object<td_api::updates>(std::move(items));
    auto converted = detail::convert_production_direct_response_for_test(
        TdFunctionKind::GetCurrentState, TdValue::from(std::move(native)));
    const auto* state = converted.get_if<TdCurrentState>();
    REQUIRE(state != nullptr);
    REQUIRE(state->updates.size() == 2);
    REQUIRE(state->updates[0].get_if<TdUpdateChatTitle>() != nullptr);
    malformed(state->updates[1], TdSupportedUpdateKind::NewChat);

    td_api::object_ptr<td_api::Object> wrong = td_api::make_object<td_api::ok>();
    auto invalid = detail::convert_production_direct_response_for_test(
        TdFunctionKind::GetCurrentState, TdValue::from(std::move(wrong)));
    CHECK(invalid.get_if<TdDirectConversionError>() != nullptr);
}

TEST_CASE("M5 resolver read factories and responses match the pinned TDLib boundary",
          "[stream][resolver][core][tdlib][td-runtime-factory]") {
    auto current = detail::make_production_get_current_state_for_test();
    CHECK(detail::production_function_matches_for_test(current, TdFunctionKind::GetCurrentState));

    auto contacts = detail::make_production_get_contacts_for_test();
    CHECK(detail::production_function_matches_for_test(contacts, TdFunctionKind::GetContacts));

    auto basic = detail::make_production_get_basic_group_full_info_for_test(51);
    CHECK(
        detail::production_function_matches_for_test(basic, TdFunctionKind::GetBasicGroupFullInfo));
    CHECK(detail::production_get_basic_group_full_info_matches_for_test(basic, 51));

    auto members = detail::make_production_get_supergroup_members_for_test(55, "ada", 20, 100);
    CHECK(detail::production_function_matches_for_test(members,
                                                       TdFunctionKind::GetSupergroupMembers));
    CHECK(detail::production_get_supergroup_members_matches_for_test(members, 55, "ada", 20, 100));

    td_api::object_ptr<td_api::Object> native_users =
        td_api::make_object<td_api::users>(2, std::vector<std::int64_t>{42, 43});
    auto converted = detail::convert_production_direct_response_for_test(
        TdFunctionKind::GetContacts, TdValue::from(std::move(native_users)));
    REQUIRE(converted.get_if<TdUsers>() != nullptr);
    CHECK(converted.get_if<TdUsers>()->user_ids == std::vector<std::int64_t>{42, 43});

    auto full = td_api::make_object<td_api::basicGroupFullInfo>();
    full->description_ = "team";
    full->creator_user_id_ = 42;
    full->members_.push_back(td_api::make_object<td_api::chatMember>(
        td_api::make_object<td_api::messageSenderUser>(42), "owner", 42, 1'700'000'000,
        td_api::make_object<td_api::chatMemberStatusCreator>(false, true)));
    td_api::object_ptr<td_api::Object> native_full = std::move(full);
    converted = detail::convert_production_direct_response_for_test(
        TdFunctionKind::GetBasicGroupFullInfo, TdValue::from(std::move(native_full)));
    REQUIRE(converted.get_if<TdBasicGroupFullInfo>() != nullptr);
    REQUIRE(converted.get_if<TdBasicGroupFullInfo>()->members.size() == 1);
    CHECK(converted.get_if<TdBasicGroupFullInfo>()->members.front().member.id == 42);

    std::vector<td_api::object_ptr<td_api::chatMember>> member_items;
    member_items.push_back(td_api::make_object<td_api::chatMember>(
        td_api::make_object<td_api::messageSenderChat>(-1002), "linked", 42, 0,
        td_api::make_object<td_api::chatMemberStatusBanned>(0)));
    td_api::object_ptr<td_api::Object> native_members =
        td_api::make_object<td_api::chatMembers>(1, std::move(member_items));
    converted = detail::convert_production_direct_response_for_test(
        TdFunctionKind::GetSupergroupMembers, TdValue::from(std::move(native_members)));
    REQUIRE(converted.get_if<TdChatMembers>() != nullptr);
    CHECK(converted.get_if<TdChatMembers>()->members.front().status.kind ==
          TdChatMemberStatusKind::Banned);
}
