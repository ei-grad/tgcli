#pragma once

#include "core/td_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace tgcli::daemon {
class LoginCoordinator;
class LogoutCoordinator;
} // namespace tgcli::daemon

namespace tgcli::core {

class AuthBootstrap;

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

    // Graceful shutdown (DESIGN.md §10): asks tdlib to close, waits for
    // authorizationStateClosed so the database is flushed, then stops the
    // receive thread and breaks any still-pending futures. Idempotent.
    void close();

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
    friend class tgcli::daemon::LogoutCoordinator;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::core
