#include "support/scripted_td_runtime.hpp"

#include "common/secure_wipe.hpp"

#include <stdexcept>
#include <utility>

namespace tgcli::test {

namespace {

void require_message_locator(std::int64_t chat_id, std::int64_t message_id) {
    if (!core::valid_td_message_locator(chat_id, message_id)) {
        throw std::invalid_argument("scripted direct TD request contains an invalid locator");
    }
}

template <typename Request> void require_direct_request(const Request& request) {
    if (!core::valid_td_direct_request(request)) {
        throw std::invalid_argument("scripted direct TD request is invalid");
    }
}

} // namespace

ScriptedTdRuntime::ScriptedTdRuntime(bool close_automatically)
    : close_automatically_(close_automatically) {}

void ScriptedTdRuntime::initialize_process(const core::TdLogConfiguration& logging) {
    const std::lock_guard<std::mutex> lock(mutex_);
    logging_configuration_ = logging;
    initialized_ = true;
}

std::int32_t ScriptedTdRuntime::create_client(std::uint64_t client_generation) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        throw std::logic_error("TDLib process initialization must precede client creation");
    }
    if (clients_.empty()) {
        initialized_before_first_client_ = true;
    }
    const auto client_id = static_cast<std::int32_t>(1000 + client_generation);
    clients_.push_back({.client_id = client_id, .client_generation = client_generation});
    cv_.notify_all();
    return client_id;
}

core::TdValue ScriptedTdRuntime::make_function(core::TdBuiltinFunction function) {
    switch (function) {
    case core::TdBuiltinFunction::GetAuthorizationState:
        return core::TdValue::scripted_function(
            core::TdFunctionData{core::TdFunctionKind::GetAuthorizationState});
    case core::TdBuiltinFunction::LogOut:
        return core::TdValue::scripted_function(core::TdFunctionData{core::TdFunctionKind::LogOut});
    case core::TdBuiltinFunction::Close:
        return core::TdValue::scripted_function(core::TdFunctionData{core::TdFunctionKind::Close});
    }
    throw std::logic_error("unknown built-in TDLib function");
}

core::TdValue ScriptedTdRuntime::make_set_tdlib_parameters(core::TdlibParameters parameters) {
    return core::TdValue::scripted_function(core::describe_tdlib_parameters(parameters));
}

core::TdValue ScriptedTdRuntime::make_auth_function(core::TdAuthRequest request) {
    before_make(request.function);
    std::vector<core::TdFunctionField> fields;
    switch (request.function) {
    case core::TdFunctionKind::SetAuthenticationPhoneNumber:
        fields.emplace_back("phone_number", core::TdRedactedValue::Credential);
        fields.emplace_back("settings_allow_flash_call", false);
        fields.emplace_back("settings_allow_missed_call", false);
        fields.emplace_back("settings_is_current_phone_number", false);
        fields.emplace_back("settings_has_unknown_phone_number", false);
        fields.emplace_back("settings_allow_sms_retriever_api", false);
        fields.emplace_back("settings_has_firebase", false);
        fields.emplace_back("settings_token_count", std::int64_t{0});
        break;
    case core::TdFunctionKind::RequestQrCodeAuthentication:
        fields.emplace_back("other_user_ids", std::vector<std::int64_t>{});
        break;
    case core::TdFunctionKind::RegisterUser:
        fields.emplace_back("first_name", core::TdRedactedValue::Credential);
        fields.emplace_back("last_name", core::TdRedactedValue::Credential);
        fields.emplace_back("disable_notification", false);
        break;
    case core::TdFunctionKind::GetMe:
        break;
    default:
        fields.emplace_back("credential", core::TdRedactedValue::Credential);
        break;
    }
    return core::TdValue::scripted_function(
        core::TdFunctionData{request.function, std::move(fields)});
}

core::TdValue
ScriptedTdRuntime::make_get_saved_messages_tags(std::int64_t saved_messages_topic_id) {
    before_make(core::TdFunctionKind::GetSavedMessagesTags);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetSavedMessagesTags,
                             {{"saved_messages_topic_id", saved_messages_topic_id}}});
}

core::TdValue
ScriptedTdRuntime::make_search_saved_messages(core::TdSearchSavedMessagesRequest request) {
    before_make(core::TdFunctionKind::SearchSavedMessages);
    const std::string selector = request.tag.kind == core::TdReactionKind::Emoji
                                     ? request.tag.emoji
                                     : "custom:" + std::to_string(request.tag.custom_emoji_id);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::SearchSavedMessages,
                             {{"saved_messages_topic_id", request.saved_messages_topic_id},
                              {"tag", selector},
                              {"query", request.query},
                              {"from_message_id", request.from_message_id},
                              {"offset", static_cast<std::int64_t>(request.offset)},
                              {"limit", static_cast<std::int64_t>(request.limit)}}});
}

core::TdValue ScriptedTdRuntime::make_get_active_sessions() {
    before_make(core::TdFunctionKind::GetActiveSessions);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetActiveSessions});
}

core::TdValue ScriptedTdRuntime::make_terminate_session(std::int64_t session_id) {
    before_make(core::TdFunctionKind::TerminateSession);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::TerminateSession, {{"session_id", session_id}}});
}

core::TdValue ScriptedTdRuntime::make_get_chat(std::int64_t chat_id) {
    before_make(core::TdFunctionKind::GetChat);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetChat, {{"chat_id", chat_id}}});
}

core::TdValue ScriptedTdRuntime::make_get_chat_history(std::int64_t chat_id,
                                                       std::int64_t from_message_id,
                                                       std::int32_t offset, std::int32_t limit,
                                                       bool only_local) {
    before_make(core::TdFunctionKind::GetChatHistory);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetChatHistory,
                             {{"chat_id", chat_id},
                              {"from_message_id", from_message_id},
                              {"offset", static_cast<std::int64_t>(offset)},
                              {"limit", static_cast<std::int64_t>(limit)},
                              {"only_local", only_local}}});
}

core::TdValue ScriptedTdRuntime::make_get_chat_message_by_date(std::int64_t chat_id,
                                                               std::int32_t date) {
    before_make(core::TdFunctionKind::GetChatMessageByDate);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetChatMessageByDate,
                             {{"chat_id", chat_id}, {"date", static_cast<std::int64_t>(date)}}});
}

core::TdValue ScriptedTdRuntime::make_get_message_thread(std::int64_t chat_id,
                                                         std::int64_t message_id) {
    before_make(core::TdFunctionKind::GetMessageThread);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetMessageThread,
                             {{"chat_id", chat_id}, {"message_id", message_id}}});
}

core::TdValue ScriptedTdRuntime::make_get_forum_topic_history(std::int64_t chat_id,
                                                              std::int32_t forum_topic_id,
                                                              std::int64_t from_message_id,
                                                              std::int32_t offset,
                                                              std::int32_t limit) {
    before_make(core::TdFunctionKind::GetForumTopicHistory);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetForumTopicHistory,
                             {{"chat_id", chat_id},
                              {"forum_topic_id", static_cast<std::int64_t>(forum_topic_id)},
                              {"from_message_id", from_message_id},
                              {"offset", static_cast<std::int64_t>(offset)},
                              {"limit", static_cast<std::int64_t>(limit)}}});
}

core::TdValue ScriptedTdRuntime::make_get_message_thread_history(std::int64_t chat_id,
                                                                 std::int64_t message_id,
                                                                 std::int64_t from_message_id,
                                                                 std::int32_t offset,
                                                                 std::int32_t limit) {
    before_make(core::TdFunctionKind::GetMessageThreadHistory);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetMessageThreadHistory,
                             {{"chat_id", chat_id},
                              {"message_id", message_id},
                              {"from_message_id", from_message_id},
                              {"offset", static_cast<std::int64_t>(offset)},
                              {"limit", static_cast<std::int64_t>(limit)}}});
}

core::TdValue ScriptedTdRuntime::make_get_direct_messages_chat_topic_history(
    std::int64_t chat_id, std::int64_t topic_id, std::int64_t from_message_id, std::int32_t offset,
    std::int32_t limit) {
    before_make(core::TdFunctionKind::GetDirectMessagesChatTopicHistory);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetDirectMessagesChatTopicHistory,
                             {{"chat_id", chat_id},
                              {"topic_id", topic_id},
                              {"from_message_id", from_message_id},
                              {"offset", static_cast<std::int64_t>(offset)},
                              {"limit", static_cast<std::int64_t>(limit)}}});
}

core::TdValue ScriptedTdRuntime::make_get_saved_messages_topic_history(std::int64_t topic_id,
                                                                       std::int64_t from_message_id,
                                                                       std::int32_t offset,
                                                                       std::int32_t limit) {
    before_make(core::TdFunctionKind::GetSavedMessagesTopicHistory);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetSavedMessagesTopicHistory,
                             {{"saved_messages_topic_id", topic_id},
                              {"from_message_id", from_message_id},
                              {"offset", static_cast<std::int64_t>(offset)},
                              {"limit", static_cast<std::int64_t>(limit)}}});
}

core::TdValue ScriptedTdRuntime::make_get_messages(std::int64_t chat_id,
                                                   std::vector<std::int64_t> message_ids) {
    before_make(core::TdFunctionKind::GetMessages);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetMessages,
                             {{"chat_id", chat_id}, {"message_ids", std::move(message_ids)}}});
}

core::TdValue ScriptedTdRuntime::make_get_message_link(std::int64_t chat_id,
                                                       std::int64_t message_id,
                                                       std::int32_t media_timestamp,
                                                       std::int32_t checklist_task_id,
                                                       std::string poll_option_id, bool for_album,
                                                       bool in_message_thread) {
    before_make(core::TdFunctionKind::GetMessageLink);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetMessageLink,
                             {{"chat_id", chat_id},
                              {"message_id", message_id},
                              {"media_timestamp", static_cast<std::int64_t>(media_timestamp)},
                              {"checklist_task_id", static_cast<std::int64_t>(checklist_task_id)},
                              {"poll_option_id", std::move(poll_option_id)},
                              {"for_album", for_album},
                              {"in_message_thread", in_message_thread}}});
}

namespace {

std::string chat_list_name(core::TdChatListKind list) {
    switch (list) {
    case core::TdChatListKind::Main:
        return "main";
    case core::TdChatListKind::Archive:
        return "archive";
    case core::TdChatListKind::Folder:
        return "folder";
    case core::TdChatListKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace

core::TdValue ScriptedTdRuntime::make_get_chats(core::TdChatList list, std::int32_t limit) {
    before_make(core::TdFunctionKind::GetChats);
    std::vector<core::TdFunctionField> fields{{"list", chat_list_name(list.kind)},
                                              {"limit", static_cast<std::int64_t>(limit)}};
    if (list.kind == core::TdChatListKind::Folder) {
        fields.emplace_back("folder_id", static_cast<std::int64_t>(list.folder_id));
    }
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetChats, std::move(fields)});
}

core::TdValue ScriptedTdRuntime::make_load_chats(core::TdChatList list, std::int32_t limit) {
    before_make(core::TdFunctionKind::LoadChats);
    std::vector<core::TdFunctionField> fields{{"list", chat_list_name(list.kind)},
                                              {"limit", static_cast<std::int64_t>(limit)}};
    if (list.kind == core::TdChatListKind::Folder) {
        fields.emplace_back("folder_id", static_cast<std::int64_t>(list.folder_id));
    }
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::LoadChats, std::move(fields)});
}

core::TdValue ScriptedTdRuntime::make_search_public_chat(std::string username) {
    before_make(core::TdFunctionKind::SearchPublicChat);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::SearchPublicChat, {{"username", std::move(username)}}});
}

core::TdValue
ScriptedTdRuntime::make_get_internal_link_type(std::string_view link, bool sensitive,
                                               const secure::WipeObserver& wipe_observer) {
    const secure::SensitiveString source(link, wipe_observer, "td_internal_link_request_source");
    before_make(core::TdFunctionKind::GetInternalLinkType);
    if (sensitive) {
        return core::TdValue::scripted_function(
            core::TdFunctionData{core::TdFunctionKind::GetInternalLinkType,
                                 {{"link", core::TdRedactedValue::InviteLink}}});
    }
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetInternalLinkType, {{"link", std::string(source.view())}}});
}

core::TdValue ScriptedTdRuntime::make_get_message_link_info(std::string url) {
    before_make(core::TdFunctionKind::GetMessageLinkInfo);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetMessageLinkInfo, {{"url", std::move(url)}}});
}

core::TdValue
ScriptedTdRuntime::make_check_chat_invite_link(std::string_view link,
                                               const secure::WipeObserver& wipe_observer) {
    const secure::SensitiveString source(link, wipe_observer, "td_check_invite_request_source");
    before_make(core::TdFunctionKind::CheckChatInviteLink);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::CheckChatInviteLink, {{"link", core::TdRedactedValue::InviteLink}}});
}

core::TdValue ScriptedTdRuntime::make_get_user(std::int64_t user_id) {
    before_make(core::TdFunctionKind::GetUser);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetUser, {{"user_id", user_id}}});
}

core::TdValue ScriptedTdRuntime::make_get_supergroup(std::int64_t supergroup_id) {
    before_make(core::TdFunctionKind::GetSupergroup);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetSupergroup, {{"supergroup_id", supergroup_id}}});
}

core::TdValue ScriptedTdRuntime::make_get_supergroup_full_info(std::int64_t supergroup_id) {
    before_make(core::TdFunctionKind::GetSupergroupFullInfo);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetSupergroupFullInfo, {{"supergroup_id", supergroup_id}}});
}

core::TdValue ScriptedTdRuntime::make_create_private_chat(std::int64_t user_id, bool force) {
    before_make(core::TdFunctionKind::CreatePrivateChat);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::CreatePrivateChat, {{"user_id", user_id}, {"force", force}}});
}

core::TdValue ScriptedTdRuntime::make_get_message(std::int64_t chat_id, std::int64_t message_id) {
    require_message_locator(chat_id, message_id);
    before_make(core::TdFunctionKind::GetMessage);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetMessage, {{"chat_id", chat_id}, {"message_id", message_id}}});
}

core::TdValue ScriptedTdRuntime::make_get_message_properties(std::int64_t chat_id,
                                                             std::int64_t message_id) {
    require_message_locator(chat_id, message_id);
    before_make(core::TdFunctionKind::GetMessageProperties);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::GetMessageProperties,
                             {{"chat_id", chat_id}, {"message_id", message_id}}});
}

core::TdValue ScriptedTdRuntime::make_get_message_available_reactions(std::int64_t chat_id,
                                                                      std::int64_t message_id) {
    require_message_locator(chat_id, message_id);
    before_make(core::TdFunctionKind::GetMessageAvailableReactions);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetMessageAvailableReactions,
        {{"chat_id", chat_id}, {"message_id", message_id}, {"row_size", std::int64_t{25}}}});
}

core::TdValue ScriptedTdRuntime::make_get_unix_time() {
    before_make(core::TdFunctionKind::GetOption);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetOption, {{"name", std::string{"unix_time"}}}});
}

core::TdValue ScriptedTdRuntime::make_parse_text_entities(std::string text,
                                                          core::TdTextParseMode mode) {
    before_make(core::TdFunctionKind::ParseTextEntities);
    const auto* const mode_name =
        mode == core::TdTextParseMode::MarkdownV2 ? "markdown_v2" : "html";
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::ParseTextEntities,
                             {{"text", std::move(text)}, {"parse_mode", std::string{mode_name}}}});
}

core::TdValue ScriptedTdRuntime::make_send_message(core::TdSendMessageRequest request,
                                                   std::uint64_t client_generation) {
    if (!core::valid_td_send_message_request(request) || client_generation == 0) {
        throw std::invalid_argument("scripted sendMessage request is invalid");
    }
    auto description = core::describe_td_send_message_request(request);
    if (request.content.parsed && !request.document) {
        const auto capability =
            request.content.formatted_text.capability
                .consume<core::TdScriptedFormattedTextCapability>(client_generation);
        if (!capability || capability->text != request.content.formatted_text.text ||
            capability->entities != request.content.formatted_text.entities) {
            throw std::invalid_argument(
                "scripted formattedText capability does not match its facts");
        }
    }
    before_make(core::TdFunctionKind::SendMessage);
    return core::TdValue::scripted_function(std::move(description));
}

core::TdValue ScriptedTdRuntime::make_forward_messages(core::TdForwardMessagesRequest request) {
    if (!core::valid_td_forward_messages_request(request)) {
        throw std::invalid_argument("scripted forwardMessages request is invalid");
    }
    auto description = core::describe_td_forward_messages_request(request);
    before_make(core::TdFunctionKind::ForwardMessages);
    return core::TdValue::scripted_function(std::move(description));
}

core::TdValue ScriptedTdRuntime::make_edit_message_text(core::TdEditMessageTextRequest request) {
    require_direct_request(request);
    before_make(core::TdFunctionKind::EditMessageText);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::EditMessageText,
                             {{"chat_id", request.chat_id},
                              {"message_id", request.message_id},
                              {"reply_markup_is_null", true},
                              {"text", std::move(request.text)},
                              {"entities_count", std::int64_t{0}},
                              {"link_preview_options_is_null", true},
                              {"clear_draft", false}}});
}

core::TdValue ScriptedTdRuntime::make_delete_messages(core::TdDeleteMessagesRequest request) {
    require_direct_request(request);
    before_make(core::TdFunctionKind::DeleteMessages);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::DeleteMessages,
                             {{"chat_id", request.chat_id},
                              {"message_ids", std::move(request.message_ids)},
                              {"revoke", request.revoke}}});
}

core::TdValue ScriptedTdRuntime::make_message_reaction(core::TdMessageReactionRequest request) {
    require_direct_request(request);
    const auto function = request.remove ? core::TdFunctionKind::RemoveMessageReaction
                                         : core::TdFunctionKind::AddMessageReaction;
    before_make(function);
    std::vector<core::TdFunctionField> fields{{"chat_id", request.chat_id},
                                              {"message_id", request.message_id},
                                              {"reaction", std::move(request.reaction)}};
    if (!request.remove) {
        fields.emplace_back("is_big", request.big);
        fields.emplace_back("update_recent_reactions", true);
    }
    return core::TdValue::scripted_function(core::TdFunctionData{function, std::move(fields)});
}

core::TdValue ScriptedTdRuntime::make_pin_message(core::TdPinMessageRequest request) {
    require_direct_request(request);
    const auto function = request.pinned ? core::TdFunctionKind::PinChatMessage
                                         : core::TdFunctionKind::UnpinChatMessage;
    before_make(function);
    std::vector<core::TdFunctionField> fields{{"chat_id", request.chat_id},
                                              {"message_id", request.message_id}};
    if (request.pinned) {
        fields.emplace_back("disable_notification", false);
        fields.emplace_back("only_for_self", false);
    }
    return core::TdValue::scripted_function(core::TdFunctionData{function, std::move(fields)});
}

core::TdValue ScriptedTdRuntime::make_view_messages(core::TdViewMessagesRequest request) {
    require_direct_request(request);
    before_make(core::TdFunctionKind::ViewMessages);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::ViewMessages,
                             {{"chat_id", request.chat_id},
                              {"message_ids", std::move(request.message_ids)},
                              {"source_is_null", true},
                              {"force_read", true}}});
}

core::TdValue ScriptedTdRuntime::make_set_chat_notification_settings(
    core::TdSetChatNotificationSettingsRequest request) {
    require_direct_request(request);
    before_make(core::TdFunctionKind::SetChatNotificationSettings);
    const auto& settings = request.settings;
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::SetChatNotificationSettings,
        {{"chat_id", request.chat_id},
         {"use_default_mute_for", settings.use_default_mute_for},
         {"mute_for", static_cast<std::int64_t>(settings.mute_for)},
         {"use_default_sound", settings.use_default_sound},
         {"sound_id", settings.sound_id},
         {"use_default_show_preview", settings.use_default_show_preview},
         {"show_preview", settings.show_preview},
         {"use_default_mute_stories", settings.use_default_mute_stories},
         {"mute_stories", settings.mute_stories},
         {"use_default_story_sound", settings.use_default_story_sound},
         {"story_sound_id", settings.story_sound_id},
         {"use_default_show_story_poster", settings.use_default_show_story_poster},
         {"show_story_poster", settings.show_story_poster},
         {"use_default_disable_pinned_message_notifications",
          settings.use_default_disable_pinned_message_notifications},
         {"disable_pinned_message_notifications", settings.disable_pinned_message_notifications},
         {"use_default_disable_mention_notifications",
          settings.use_default_disable_mention_notifications},
         {"disable_mention_notifications", settings.disable_mention_notifications}}});
}

core::TdValue
ScriptedTdRuntime::make_toggle_chat_is_pinned(core::TdToggleChatIsPinnedRequest request) {
    require_direct_request(request);
    before_make(core::TdFunctionKind::ToggleChatIsPinned);
    const auto* const list = request.list == core::TdDirectChatList::Main ? "main" : "archive";
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::ToggleChatIsPinned,
                             {{"chat_list", std::string{list}},
                              {"chat_id", request.chat_id},
                              {"is_pinned", request.pinned}}});
}

core::TdValue ScriptedTdRuntime::make_add_chat_to_list(core::TdAddChatToListRequest request) {
    require_direct_request(request);
    before_make(core::TdFunctionKind::AddChatToList);
    const auto* const list = request.list == core::TdDirectChatList::Main ? "main" : "archive";
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::AddChatToList,
                             {{"chat_id", request.chat_id}, {"chat_list", std::string{list}}}});
}

core::TdValue ScriptedTdRuntime::make_join_chat(core::TdJoinChatRequest request) {
    require_direct_request(request);
    if (request.chat_id.has_value()) {
        before_make(core::TdFunctionKind::JoinChat);
        return core::TdValue::scripted_function(core::TdFunctionData{
            core::TdFunctionKind::JoinChat, {{"chat_id", request.chat_id.value_or(0)}}});
    }
    before_make(core::TdFunctionKind::JoinChatByInviteLink);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::JoinChatByInviteLink,
                             {{"invite_link", core::TdRedactedValue::InviteLink}}});
}

core::TdValue ScriptedTdRuntime::make_leave_chat(core::TdLeaveChatRequest request) {
    require_direct_request(request);
    before_make(core::TdFunctionKind::LeaveChat);
    return core::TdValue::scripted_function(
        core::TdFunctionData{core::TdFunctionKind::LeaveChat, {{"chat_id", request.chat_id}}});
}

void ScriptedTdRuntime::send(std::int32_t client_id, std::uint64_t client_generation,
                             std::uint64_t query_id, core::TdValue& function) {
    if (!function.function_data().has_value()) {
        throw std::invalid_argument("scripted TDLib function lacks neutral data");
    }

    const auto function_data = *function.function_data();
    std::function<void(const core::TdFunctionData&)> before_send;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        before_send = before_send_;
    }
    if (before_send) {
        before_send(function_data);
    }
    bool close_automatically = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sent_.push_back({.client_id = client_id,
                         .client_generation = client_generation,
                         .query_id = query_id,
                         .function = function_data});
        close_automatically = close_automatically_ && function_data.has_type("close");
        cv_.notify_all();
    }
    if (close_automatically) {
        push_update({.client_id = client_id, .client_generation = client_generation}, {},
                    core::AuthStateData{core::AuthState::Closed});
    }
}

std::optional<core::TdRuntimeEvent> ScriptedTdRuntime::receive(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return !receive_paused_ && !events_.empty(); })) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    ++received_count_;
    cv_.notify_all();
    return event;
}

void ScriptedTdRuntime::push_response(ScriptedClient client, std::uint64_t query_id,
                                      core::TdValue object,
                                      std::optional<core::AuthStateData> authorization_state) {
    push_event({.client_id = client.client_id,
                .client_generation = client.client_generation,
                .query_id = query_id,
                .object = std::move(object),
                .authorization_state = std::move(authorization_state)});
}

void ScriptedTdRuntime::push_update(ScriptedClient client, core::TdValue object,
                                    std::optional<core::AuthStateData> authorization_state) {
    push_event({.client_id = client.client_id,
                .client_generation = client.client_generation,
                .query_id = 0,
                .object = std::move(object),
                .authorization_state = std::move(authorization_state)});
}

void ScriptedTdRuntime::push_message_send_succeeded(ScriptedClient client,
                                                    std::int64_t old_message_id,
                                                    std::optional<core::TdWriteMessage> message) {
    push_update(client, core::TdValue::from(core::TdUpdateMessageSendSucceeded{
                            .client_generation = client.client_generation,
                            .old_message_id = old_message_id,
                            .message = std::move(message)}));
}

void ScriptedTdRuntime::push_message_send_failed(ScriptedClient client, std::int64_t old_message_id,
                                                 std::optional<core::TdWriteMessage> message,
                                                 std::optional<core::TdError> error) {
    push_update(client, core::TdValue::from(core::TdUpdateMessageSendFailed{
                            .client_generation = client.client_generation,
                            .old_message_id = old_message_id,
                            .message = std::move(message),
                            .error = std::move(error)}));
}

void ScriptedTdRuntime::push_delete_messages(ScriptedClient client, std::int64_t chat_id,
                                             std::vector<std::int64_t> message_ids,
                                             bool is_permanent, bool from_cache) {
    push_update(client, core::TdValue::from(core::TdUpdateDeleteMessages{
                            .client_generation = client.client_generation,
                            .chat_id = chat_id,
                            .message_ids = std::move(message_ids),
                            .is_permanent = is_permanent,
                            .from_cache = from_cache}));
}

core::TdFormattedText
ScriptedTdRuntime::parsed_formatted_text(ScriptedClient client, std::string text,
                                         std::vector<core::TdTextEntity> entities) {
    auto capability = core::TdFormattedTextCapability::from(
        core::TdScriptedFormattedTextCapability{.text = text, .entities = entities},
        client.client_generation);
    return {.text = std::move(text),
            .entities = std::move(entities),
            .capability = std::move(capability)};
}

bool ScriptedTdRuntime::wait_for_sent(std::size_t count, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] { return sent_.size() >= count; });
}

bool ScriptedTdRuntime::wait_for_received(std::size_t count,
                                          std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] { return received_count_ >= count; });
}

std::size_t ScriptedTdRuntime::received_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return received_count_;
}

bool ScriptedTdRuntime::wait_for_clients(std::size_t count,
                                         std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] { return clients_.size() >= count; });
}

std::vector<SentTdFunction> ScriptedTdRuntime::sent_functions() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sent_;
}

std::vector<ScriptedClient> ScriptedTdRuntime::clients() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return clients_;
}

bool ScriptedTdRuntime::initialized_before_first_client() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return initialized_before_first_client_;
}

core::TdLogConfiguration ScriptedTdRuntime::logging_configuration() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return logging_configuration_;
}

void ScriptedTdRuntime::set_before_make(std::function<void(core::TdFunctionKind)> hook) {
    const std::lock_guard<std::mutex> lock(mutex_);
    before_make_ = std::move(hook);
}

void ScriptedTdRuntime::set_before_send(std::function<void(const core::TdFunctionData&)> hook) {
    const std::lock_guard<std::mutex> lock(mutex_);
    before_send_ = std::move(hook);
}

void ScriptedTdRuntime::before_make(core::TdFunctionKind function) {
    std::function<void(core::TdFunctionKind)> hook;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        hook = before_make_;
    }
    if (hook) {
        hook(function);
    }
}

void ScriptedTdRuntime::set_receive_paused(bool paused) {
    const std::lock_guard<std::mutex> lock(mutex_);
    receive_paused_ = paused;
    cv_.notify_all();
}

void ScriptedTdRuntime::set_close_automatically(bool enabled) {
    const std::lock_guard<std::mutex> lock(mutex_);
    close_automatically_ = enabled;
}

void ScriptedTdRuntime::push_event(core::TdRuntimeEvent event) {
    const std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
    cv_.notify_all();
}

} // namespace tgcli::test
