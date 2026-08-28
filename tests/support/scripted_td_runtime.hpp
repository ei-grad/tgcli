#pragma once

#include "core/td_runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace tgcli::test {

struct ScriptedClient {
    std::int32_t client_id;
    std::uint64_t client_generation;

    bool operator==(const ScriptedClient&) const = default;
};

struct SentTdFunction {
    std::int32_t client_id;
    std::uint64_t client_generation;
    std::uint64_t query_id;
    core::TdFunctionData function;

    bool operator==(const SentTdFunction&) const = default;
};

class ScriptedTdRuntime final : public core::TdRuntime {
  public:
    explicit ScriptedTdRuntime(bool close_automatically = true);

    void initialize_process(const core::TdLogConfiguration& logging) override;
    std::int32_t create_client(std::uint64_t client_generation) override;
    core::TdValue make_function(core::TdBuiltinFunction function) override;
    core::TdValue make_get_current_state() override;
    core::TdValue make_get_contacts() override;
    core::TdValue make_m6_function(core::TdM6Request request) override;
    core::TdValue make_set_tdlib_parameters(core::TdlibParameters parameters) override;
    core::TdValue make_auth_function(core::TdAuthRequest request) override;
    core::TdValue make_get_saved_messages_tags(std::int64_t saved_messages_topic_id) override;
    core::TdValue make_search_saved_messages(core::TdSearchSavedMessagesRequest request) override;
    core::TdValue make_get_active_sessions() override;
    core::TdValue make_terminate_session(std::int64_t session_id) override;
    core::TdValue make_get_chat(std::int64_t chat_id) override;
    core::TdValue make_get_chat_history(std::int64_t chat_id, std::int64_t from_message_id,
                                        std::int32_t offset, std::int32_t limit,
                                        bool only_local) override;
    core::TdValue make_get_chat_message_by_date(std::int64_t chat_id, std::int32_t date) override;
    core::TdValue make_get_message_thread(std::int64_t chat_id, std::int64_t message_id) override;
    core::TdValue make_get_forum_topic_history(std::int64_t chat_id, std::int32_t forum_topic_id,
                                               std::int64_t from_message_id, std::int32_t offset,
                                               std::int32_t limit) override;
    core::TdValue make_get_message_thread_history(std::int64_t chat_id, std::int64_t message_id,
                                                  std::int64_t from_message_id, std::int32_t offset,
                                                  std::int32_t limit) override;
    core::TdValue make_get_direct_messages_chat_topic_history(std::int64_t chat_id,
                                                              std::int64_t topic_id,
                                                              std::int64_t from_message_id,
                                                              std::int32_t offset,
                                                              std::int32_t limit) override;
    core::TdValue make_get_saved_messages_topic_history(std::int64_t topic_id,
                                                        std::int64_t from_message_id,
                                                        std::int32_t offset,
                                                        std::int32_t limit) override;
    core::TdValue make_get_messages(std::int64_t chat_id,
                                    std::vector<std::int64_t> message_ids) override;
    core::TdValue make_get_message_link(std::int64_t chat_id, std::int64_t message_id,
                                        std::int32_t media_timestamp,
                                        std::int32_t checklist_task_id, std::string poll_option_id,
                                        bool for_album, bool in_message_thread) override;
    core::TdValue make_get_chats(core::TdChatList list, std::int32_t limit) override;
    core::TdValue make_load_chats(core::TdChatList list, std::int32_t limit) override;
    core::TdValue make_search_public_chat(std::string username) override;
    core::TdValue
    make_get_internal_link_type(std::string_view link, bool sensitive = false,
                                const secure::WipeObserver& wipe_observer = {}) override;
    core::TdValue make_get_message_link_info(std::string url) override;
    core::TdValue
    make_check_chat_invite_link(std::string_view link,
                                const secure::WipeObserver& wipe_observer = {}) override;
    core::TdValue make_search_chat_messages(core::TdSearchChatMessagesRequest request) override;
    core::TdValue make_search_messages(core::TdSearchMessagesRequest request) override;
    core::TdValue make_get_user(std::int64_t user_id) override;
    core::TdValue make_get_user_full_info(std::int64_t user_id) override;
    core::TdValue make_get_basic_group(std::int64_t basic_group_id) override;
    core::TdValue make_get_basic_group_full_info(std::int64_t basic_group_id) override;
    core::TdValue make_get_supergroup(std::int64_t supergroup_id) override;
    core::TdValue make_get_supergroup_full_info(std::int64_t supergroup_id) override;
    core::TdValue make_get_supergroup_members(std::int64_t supergroup_id,
                                              core::TdSupergroupMembersFilter filter,
                                              std::string query, std::int32_t offset,
                                              std::int32_t limit) override;
    core::TdValue make_get_supergroup_members(std::int64_t supergroup_id, std::string query,
                                              std::int32_t offset, std::int32_t limit);
    core::TdValue make_create_private_chat(std::int64_t user_id, bool force) override;
    core::TdValue make_get_message(std::int64_t chat_id, std::int64_t message_id) override;
    core::TdValue make_get_download_message(std::int64_t chat_id, std::int64_t message_id) override;
    core::TdValue make_download_file(std::int32_t file_id) override;
    core::TdValue make_get_suggested_file_name(std::int32_t file_id,
                                               std::string directory) override;
    core::TdValue make_get_message_properties(std::int64_t chat_id,
                                              std::int64_t message_id) override;
    core::TdValue make_get_message_available_reactions(std::int64_t chat_id,
                                                       std::int64_t message_id) override;
    core::TdValue make_get_unix_time() override;
    core::TdValue make_parse_text_entities(std::string text, core::TdTextParseMode mode) override;
    core::TdValue make_send_message(core::TdSendMessageRequest request,
                                    std::uint64_t client_generation) override;
    core::TdValue make_forward_messages(core::TdForwardMessagesRequest request) override;
    core::TdValue make_edit_message_text(core::TdEditMessageTextRequest request) override;
    core::TdValue make_delete_messages(core::TdDeleteMessagesRequest request) override;
    core::TdValue make_message_reaction(core::TdMessageReactionRequest request) override;
    core::TdValue make_pin_message(core::TdPinMessageRequest request) override;
    core::TdValue make_view_messages(core::TdViewMessagesRequest request) override;
    core::TdValue make_set_chat_notification_settings(
        core::TdSetChatNotificationSettingsRequest request) override;
    core::TdValue make_toggle_chat_is_pinned(core::TdToggleChatIsPinnedRequest request) override;
    core::TdValue make_add_chat_to_list(core::TdAddChatToListRequest request) override;
    core::TdValue make_join_chat(core::TdJoinChatRequest request) override;
    core::TdValue make_leave_chat(core::TdLeaveChatRequest request) override;
    void send(std::int32_t client_id, std::uint64_t client_generation, std::uint64_t query_id,
              core::TdValue& function) override;
    std::optional<core::TdRuntimeEvent> receive(std::chrono::milliseconds timeout) override;

    void push_response(ScriptedClient client, std::uint64_t query_id, core::TdValue object = {},
                       std::optional<core::AuthStateData> authorization_state = std::nullopt);
    void push_update(ScriptedClient client, core::TdValue object = {},
                     std::optional<core::AuthStateData> authorization_state = std::nullopt);
    void push_message_send_succeeded(ScriptedClient client, std::int64_t old_message_id,
                                     std::optional<core::TdWriteMessage> message);
    void push_message_send_failed(ScriptedClient client, std::int64_t old_message_id,
                                  std::optional<core::TdWriteMessage> message,
                                  std::optional<core::TdError> error);
    void push_delete_messages(ScriptedClient client, std::int64_t chat_id,
                              std::vector<std::int64_t> message_ids, bool is_permanent,
                              bool from_cache);
    static core::TdFormattedText
    parsed_formatted_text(ScriptedClient client, std::string text,
                          std::vector<core::TdTextEntity> entities = {});

    // Ordinary command traces exclude the core-owned getCurrentState bootstrap.
    bool wait_for_sent(std::size_t count,
                       std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    // Bootstrap/lifecycle contracts use the complete trace to assert query ordering.
    bool wait_for_sent_including_current_state(
        std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    bool wait_for_received(std::size_t count,
                           std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    [[nodiscard]] std::size_t received_count() const;
    bool wait_for_clients(std::size_t count,
                          std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    [[nodiscard]] std::vector<SentTdFunction> sent_functions() const;
    [[nodiscard]] std::vector<SentTdFunction> sent_functions_including_current_state() const;
    [[nodiscard]] std::vector<ScriptedClient> clients() const;
    [[nodiscard]] bool initialized_before_first_client() const;
    [[nodiscard]] core::TdLogConfiguration logging_configuration() const;
    void set_before_make(std::function<void(core::TdFunctionKind)> hook);
    void set_before_send(std::function<void(const core::TdFunctionData&)> hook);
    void set_receive_paused(bool paused);
    void set_close_automatically(bool enabled);

  private:
    void before_make(core::TdFunctionKind function);
    void push_event(core::TdRuntimeEvent event);

    bool close_automatically_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool initialized_ = false;
    bool initialized_before_first_client_ = false;
    core::TdLogConfiguration logging_configuration_;
    bool receive_paused_ = false;
    std::vector<ScriptedClient> clients_;
    std::vector<SentTdFunction> sent_;
    std::size_t received_count_ = 0;
    std::deque<core::TdRuntimeEvent> events_;
    std::function<void(core::TdFunctionKind)> before_make_;
    std::function<void(const core::TdFunctionData&)> before_send_;
};

} // namespace tgcli::test
