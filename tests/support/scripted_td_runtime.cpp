#include "support/scripted_td_runtime.hpp"

#include <stdexcept>
#include <utility>

namespace tgcli::test {

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

namespace {

std::string chat_list_name(core::TdChatListKind list) {
    return list == core::TdChatListKind::Main ? "main" : "archive";
}

} // namespace

core::TdValue ScriptedTdRuntime::make_get_chats(core::TdChatListKind list, std::int32_t limit) {
    before_make(core::TdFunctionKind::GetChats);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetChats,
        {{"list", chat_list_name(list)}, {"limit", static_cast<std::int64_t>(limit)}}});
}

core::TdValue ScriptedTdRuntime::make_load_chats(core::TdChatListKind list, std::int32_t limit) {
    before_make(core::TdFunctionKind::LoadChats);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::LoadChats,
        {{"list", chat_list_name(list)}, {"limit", static_cast<std::int64_t>(limit)}}});
}

core::TdValue ScriptedTdRuntime::make_search_public_chat(std::string username) {
    before_make(core::TdFunctionKind::SearchPublicChat);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::SearchPublicChat, {{"username", std::move(username)}}});
}

core::TdValue ScriptedTdRuntime::make_get_internal_link_type(std::string link) {
    before_make(core::TdFunctionKind::GetInternalLinkType);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetInternalLinkType, {{"link", std::move(link)}}});
}

core::TdValue ScriptedTdRuntime::make_get_message_link_info(std::string url) {
    before_make(core::TdFunctionKind::GetMessageLinkInfo);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::GetMessageLinkInfo, {{"url", std::move(url)}}});
}

core::TdValue ScriptedTdRuntime::make_check_chat_invite_link(std::string link) {
    before_make(core::TdFunctionKind::CheckChatInviteLink);
    return core::TdValue::scripted_function(core::TdFunctionData{
        core::TdFunctionKind::CheckChatInviteLink, {{"link", std::move(link)}}});
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

void ScriptedTdRuntime::send(std::int32_t client_id, std::uint64_t client_generation,
                             std::uint64_t query_id, core::TdValue function) {
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

bool ScriptedTdRuntime::wait_for_sent(std::size_t count, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] { return sent_.size() >= count; });
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
