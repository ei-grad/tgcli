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
    core::TdValue make_set_tdlib_parameters(core::TdlibParameters parameters) override;
    core::TdValue make_auth_function(core::TdAuthRequest request) override;
    core::TdValue make_get_saved_messages_tags(std::int64_t saved_messages_topic_id) override;
    core::TdValue make_search_saved_messages(core::TdSearchSavedMessagesRequest request) override;
    core::TdValue make_get_active_sessions() override;
    core::TdValue make_terminate_session(std::int64_t session_id) override;
    core::TdValue make_get_chat(std::int64_t chat_id) override;
    core::TdValue make_get_messages(std::int64_t chat_id,
                                    std::vector<std::int64_t> message_ids) override;
    core::TdValue make_get_message_link(std::int64_t chat_id, std::int64_t message_id,
                                        std::int32_t media_timestamp,
                                        std::int32_t checklist_task_id, std::string poll_option_id,
                                        bool for_album, bool in_message_thread) override;
    core::TdValue make_get_chats(core::TdChatList list, std::int32_t limit) override;
    core::TdValue make_load_chats(core::TdChatList list, std::int32_t limit) override;
    core::TdValue make_search_public_chat(std::string username) override;
    core::TdValue make_get_internal_link_type(std::string link) override;
    core::TdValue make_get_message_link_info(std::string url) override;
    core::TdValue make_check_chat_invite_link(std::string link) override;
    core::TdValue make_get_user(std::int64_t user_id) override;
    core::TdValue make_get_supergroup(std::int64_t supergroup_id) override;
    core::TdValue make_get_supergroup_full_info(std::int64_t supergroup_id) override;
    core::TdValue make_create_private_chat(std::int64_t user_id, bool force) override;
    void send(std::int32_t client_id, std::uint64_t client_generation, std::uint64_t query_id,
              core::TdValue function) override;
    std::optional<core::TdRuntimeEvent> receive(std::chrono::milliseconds timeout) override;

    void push_response(ScriptedClient client, std::uint64_t query_id, core::TdValue object = {},
                       std::optional<core::AuthStateData> authorization_state = std::nullopt);
    void push_update(ScriptedClient client, core::TdValue object = {},
                     std::optional<core::AuthStateData> authorization_state = std::nullopt);

    bool wait_for_sent(std::size_t count,
                       std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    bool wait_for_received(std::size_t count,
                           std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    [[nodiscard]] std::size_t received_count() const;
    bool wait_for_clients(std::size_t count,
                          std::chrono::milliseconds timeout = std::chrono::seconds(2)) const;
    [[nodiscard]] std::vector<SentTdFunction> sent_functions() const;
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
