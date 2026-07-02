#pragma once

// Daemon-side only: this header carries td_api.h (via Client.h) and must
// never be included from client-side code (cli, output, prompts) — see the
// td_api.h invariant in CLAUDE.md.

#include "core/query_registry.hpp"
#include "core/update_bus.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

namespace tgcli::core {

// Owns tdlib's ClientManager and its receive loop on a dedicated thread
// (DESIGN.md §7). tdlib object lifecycle and receive-loop rules live
// entirely here; command handlers construct typed requests but never touch
// the ClientManager or the update loop directly.
class TdClient {
  public:
    using ObjectPtr = td::td_api::object_ptr<td::td_api::Object>;
    using UpdateHandler = std::function<void(const td::td_api::Object&)>;

    TdClient();
    ~TdClient();
    TdClient(const TdClient&) = delete;
    TdClient& operator=(const TdClient&) = delete;
    TdClient(TdClient&&) = delete;
    TdClient& operator=(TdClient&&) = delete;

    // Thread-safe. The future resolves with the raw response object, which
    // may be a td_api::error.
    std::future<ObjectPtr> send(td::td_api::object_ptr<td::td_api::Function> request);

    // Handlers run on the receive thread under the bus lock: fast, no tdlib
    // calls, no (un)subscribe from within a handler (see UpdateBus).
    std::uint64_t subscribe_updates(UpdateHandler handler);
    void unsubscribe_updates(std::uint64_t id);

    // Graceful shutdown (DESIGN.md §10): asks tdlib to close, waits for
    // authorizationStateClosed so the database is flushed, then stops the
    // receive thread and breaks any still-pending futures. Idempotent.
    void close();

    // tdlib version string, synchronously and without a client.
    static std::string tdlib_version();

  private:
    void receive_loop();
    void handle_update(ObjectPtr update);

    td::ClientManager manager_;
    std::int32_t client_id_ = 0;
    QueryRegistry<ObjectPtr> queries_;
    UpdateBus<td::td_api::Object> updates_;
    std::atomic<bool> stop_{false};
    std::mutex closed_mutex_;
    std::condition_variable closed_cv_;
    bool closed_ = false;
    std::once_flag close_once_;
    std::thread receive_thread_;
};

} // namespace tgcli::core
