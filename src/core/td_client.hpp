#pragma once

#include "core/td_runtime.hpp"

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace tgcli::core {

// Owns tdlib's ClientManager and its receive loop on a dedicated thread
// (DESIGN.md §7). tdlib object lifecycle and receive-loop rules live
// entirely here; command handlers construct typed requests but never touch
// the ClientManager or the update loop directly.
class TdClient {
  public:
    using UpdateHandler = std::function<void(const TdValue&)>;
    using AuthStateHandler = std::function<void(const std::shared_ptr<const AuthStateSnapshot>&)>;

    TdClient();
    explicit TdClient(std::unique_ptr<TdRuntime> runtime);
    ~TdClient();
    TdClient(const TdClient&) = delete;
    TdClient& operator=(const TdClient&) = delete;
    TdClient(TdClient&&) = delete;
    TdClient& operator=(TdClient&&) = delete;

    // Thread-safe. Native request/response objects stay inside TdValue so
    // generated TDLib types remain daemon-implementation details. Once
    // close begins, a valid request returns a ready future that throws
    // std::runtime_error instead of entering the request registry.
    std::future<TdValue> send(TdValue request);

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
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::core
