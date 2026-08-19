#pragma once

#include "core/td_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace tgcli::daemon {
class LoginCoordinator;
class LogoutLifecycle;
class LogoutCoordinator;
} // namespace tgcli::daemon

namespace tgcli::core {

class AuthBootstrap;

struct TdClientEventHooks {
    std::function<TdEventClock::time_point()> now;
    // Runs after publication prerequisites are held but before entering the publication gate.
    std::function<void(TdEventClock::time_point)> after_observed;
    std::function<void()> before_lifecycle_callback_drain_wait;
    std::function<void()> before_closed_decisions_drain_wait;
};

class TdOwnerLease {
  public:
    TdOwnerLease();
    ~TdOwnerLease();
    TdOwnerLease(const TdOwnerLease&) = delete;
    TdOwnerLease& operator=(const TdOwnerLease&) = delete;
    TdOwnerLease(TdOwnerLease&&) noexcept;
    TdOwnerLease& operator=(TdOwnerLease&& other) noexcept;

    [[nodiscard]] TdRequestOwner owner() const noexcept;
    explicit operator bool() const noexcept;

  private:
    struct State;
    explicit TdOwnerLease(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
    friend class TdClient;
};

class TdSendLease {
  public:
    TdSendLease();
    ~TdSendLease();
    TdSendLease(const TdSendLease&) = delete;
    TdSendLease& operator=(const TdSendLease&) = delete;
    TdSendLease(TdSendLease&&) noexcept;
    TdSendLease& operator=(TdSendLease&& other) noexcept;

    explicit operator bool() const noexcept;

  private:
    struct State;
    explicit TdSendLease(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
    friend class TdClient;
};

// Pins a matching client generation at authorizationStateClosed until the
// destructive coordinator has durably settled its one-shot lifecycle result.
enum class TdLifecycleClaimStatus { Active, Disconnected, Shutdown, TimedOut, Rejected };
enum class TdClosedDecisionStatus {
    Pending,
    Closed,
    Error,
    Disconnected,
    Shutdown,
    TimedOut,
    Rejected
};

class TdClosedDecision {
  public:
    TdClosedDecision();
    ~TdClosedDecision();
    TdClosedDecision(const TdClosedDecision&) = delete;
    TdClosedDecision& operator=(const TdClosedDecision&) = delete;
    TdClosedDecision(TdClosedDecision&&) noexcept;
    TdClosedDecision& operator=(TdClosedDecision&& other) noexcept;

    explicit operator bool() const noexcept;
    [[nodiscard]] TdClosedDecisionStatus status() const;
    TdClosedDecisionStatus settle_terminal();

  private:
    struct State;
    explicit TdClosedDecision(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
    friend class TdClient;
};

class TdAuthorizationError final : public std::runtime_error {
  public:
    explicit TdAuthorizationError(TdAuthorizationFailure failure);

    [[nodiscard]] TdAuthorizationFailure failure() const noexcept {
        return failure_;
    }

  private:
    TdAuthorizationFailure failure_;
};

// Owns tdlib's ClientManager and its receive loop on a dedicated thread
// (DESIGN.md §7). tdlib object lifecycle and receive-loop rules live
// entirely here; command handlers construct typed requests but never touch
// the ClientManager or the update loop directly.
class TdClient {
  public:
    using UpdateHandler = std::function<void(const TdValue&)>;
    using AuthStateHandler = std::function<void(const std::shared_ptr<const AuthStateSnapshot>&)>;

    explicit TdClient(const TdLogConfiguration& logging);
    explicit TdClient(std::unique_ptr<TdRuntime> runtime);
    TdClient(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging);
    TdClient(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging,
             TdClientEventHooks event_hooks);
    ~TdClient();
    TdClient(const TdClient&) = delete;
    TdClient& operator=(const TdClient&) = delete;
    TdClient(TdClient&&) = delete;
    TdClient& operator=(TdClient&&) = delete;

    // Thread-safe. Native request/response objects stay inside TdValue so
    // generated TDLib types remain daemon-implementation details. Once
    // close begins, a valid request returns a ready future that throws
    // std::runtime_error instead of entering the request registry.
    std::future<TdValue> send(TdSendDescriptor descriptor, TdValue request);
    std::future<TdValue> send(TdSendDescriptor descriptor, TdlibParameters parameters);

    // A lease pins one admitted authorization snapshot across a local commit
    // and the following TDLib submission. It is deliberately move-only and
    // can be consumed by exactly one parameter send.
    [[nodiscard]] TdSendLease acquire_send_lease(TdSendDescriptor descriptor);
    std::future<TdValue> send(TdSendLease&& lease, TdlibParameters parameters);

    // Safe request-coordinator seam: only the closed read allowlist can be
    // submitted, and the owner capability is minted and revoked internally.
    std::future<TdValue> send_read(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdFunctionKind function, TdValue request);
    std::future<TdValue> get_me(const std::shared_ptr<const AuthStateSnapshot>& authorization);
    std::future<TdValue>
    get_saved_messages_tags(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                            std::int64_t saved_messages_topic_id);
    std::future<TdValue>
    search_saved_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          TdSearchSavedMessagesRequest request);
    std::future<TdValue>
    get_active_sessions(const std::shared_ptr<const AuthStateSnapshot>& authorization);
    std::future<TdValue> get_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t chat_id);
    std::future<TdValue>
    get_chat_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     std::int64_t chat_id, std::int64_t from_message_id, std::int32_t offset,
                     std::int32_t limit, bool only_local);
    std::future<TdValue>
    get_chat_message_by_date(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::int64_t chat_id, std::int32_t date);
    std::future<TdValue>
    get_message_thread(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       std::int64_t chat_id, std::int64_t message_id);
    std::future<TdValue>
    get_forum_topic_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                            std::int64_t chat_id, std::int32_t forum_topic_id,
                            std::int64_t from_message_id, std::int32_t offset, std::int32_t limit);
    std::future<TdValue>
    get_message_thread_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                               std::int64_t chat_id, std::int64_t message_id,
                               std::int64_t from_message_id, std::int32_t offset,
                               std::int32_t limit);
    std::future<TdValue> get_direct_messages_chat_topic_history(
        const std::shared_ptr<const AuthStateSnapshot>& authorization, std::int64_t chat_id,
        std::int64_t topic_id, std::int64_t from_message_id, std::int32_t offset,
        std::int32_t limit);
    std::future<TdValue>
    get_saved_messages_topic_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                     std::int64_t topic_id, std::int64_t from_message_id,
                                     std::int32_t offset, std::int32_t limit);
    std::future<TdValue> get_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                      std::int64_t chat_id, std::vector<std::int64_t> message_ids);
    std::future<TdValue>
    get_message_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     std::int64_t chat_id, std::int64_t message_id, std::int32_t media_timestamp,
                     std::int32_t checklist_task_id, std::string poll_option_id, bool for_album,
                     bool in_message_thread);
    std::future<TdValue> get_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdChatListKind list, std::int32_t limit);
    std::future<TdValue> get_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdChatList list, std::int32_t limit);
    std::future<TdValue> load_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    TdChatListKind list, std::int32_t limit);
    std::future<TdValue> load_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    TdChatList list, std::int32_t limit);
    std::future<TdValue>
    search_public_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       std::string username);
    std::future<TdValue>
    get_internal_link_type(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::string link);
    std::future<TdValue>
    get_message_link_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          std::string url);
    std::future<TdValue>
    check_chat_invite_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::string link);
    std::future<TdValue> get_user(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t user_id);
    std::future<TdValue>
    get_supergroup(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                   std::int64_t supergroup_id);
    std::future<TdValue>
    get_supergroup_full_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::int64_t supergroup_id);
    std::future<TdValue>
    create_private_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                        std::int64_t user_id, bool force);

    [[nodiscard]] bool owns(const TdRequestOwner& owner, std::uint64_t client_generation) const;

    // Handlers run on the receive thread under the bus lock: fast, no tdlib
    // calls, no (un)subscribe from within a handler (see UpdateBus).
    std::uint64_t subscribe_updates(UpdateHandler handler);
    void unsubscribe_updates(std::uint64_t id);

    // Every accepted authorization update produces a new immutable snapshot,
    // including payload-equal and repeated QR updates.
    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot> auth_state() const;
    std::uint64_t subscribe_auth_states(AuthStateHandler handler);
    void unsubscribe_auth_states(std::uint64_t id);

    // Serializes a deadline decision with receive-event stamping and publication.
    // The caller must keep the returned lock while inspecting response/auth visibility.
    [[nodiscard]] std::unique_lock<std::mutex> lock_event_publication() const;

    // Graceful shutdown (DESIGN.md §10): asks tdlib to close, waits for
    // authorizationStateClosed so the database is flushed, then stops the
    // receive thread and breaks any still-pending futures. Idempotent.
    void close();

    // Begins the same intentional shutdown but never waits past deadline.
    // False means Closed was not observed and the receive loop remains alive,
    // so a later call may continue waiting for the same generation.
    [[nodiscard]] bool close_until(std::chrono::steady_clock::time_point deadline);

    // tdlib version string, synchronously and without a client.
    static std::string tdlib_version();

  private:
    [[nodiscard]] TdRequestOwner internal_auth_owner() const;
    [[nodiscard]] TdOwnerLease issue_login_owner();
    std::future<TdValue> send_login(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    const TdRequestOwner& owner, TdAuthRequest request);
    std::future<TdValue> send_logout(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                     TdClosedDecision& decision);
    [[nodiscard]] TdClosedDecision begin_logout_decision(
        const std::shared_ptr<const AuthStateSnapshot>& authorization,
        std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim);
    bool restart_generation(const std::shared_ptr<const AuthStateSnapshot>& authorization);

    friend class AuthBootstrap;
    friend class tgcli::daemon::LoginCoordinator;
    friend class tgcli::daemon::LogoutLifecycle;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::core
