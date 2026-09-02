#pragma once

#include "common/cancellation.hpp"
#include "common/paths.hpp"
#include "daemon/activity_tracker.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_observer.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tgcli::daemon {

class ConnectionState;
class RequestSession;

class ServerOptions {
  private:
    std::string account_;
    std::string socket_path;
    std::string binary_version;
    int protocol_version = 0;
    std::string control_socket_path;
    std::string control_token;
    secure::WipeObserver wipe_observer;
    testing::RequestObservationObserver request_observer;
    testing::RequestAdmissionProbe request_admission_probe;
    ConfigRuntime* config_runtime = nullptr;
    std::shared_ptr<const testing::ActivityTrackerHooks> activity_hooks;
    testing::RequestWallClock request_wall_clock;

    friend class Server;

  public:
    ServerOptions(std::string account_value, std::string socket_path_value,
                  std::string binary_version_value, int protocol_version_value,
                  std::string control_socket_path_value, std::string control_token_value,
                  secure::WipeObserver wipe_observer_value = {},
                  testing::RequestObservationObserver request_observer_value = {},
                  testing::RequestAdmissionProbe request_admission_probe_value = {},
                  ConfigRuntime* config_runtime_value = nullptr,
                  std::shared_ptr<const testing::ActivityTrackerHooks> activity_hooks_value = {},
                  testing::RequestWallClock request_wall_clock_value = {})
        : account_(std::move(account_value)), socket_path(std::move(socket_path_value)),
          binary_version(std::move(binary_version_value)), protocol_version(protocol_version_value),
          control_socket_path(std::move(control_socket_path_value)),
          control_token(std::move(control_token_value)),
          wipe_observer(std::move(wipe_observer_value)),
          request_observer(std::move(request_observer_value)),
          request_admission_probe(std::move(request_admission_probe_value)),
          config_runtime(config_runtime_value), activity_hooks(std::move(activity_hooks_value)),
          request_wall_clock(std::move(request_wall_clock_value)) {
        if (!paths::valid_account_name(account_)) {
            throw std::invalid_argument("server account is invalid");
        }
    }

    [[nodiscard]] const std::string& account() const {
        return account_;
    }
};

// Accepts connections on the account's unix socket and serves the JSONL
// frame protocol (DESIGN.md §10): peer-uid check, hello handshake, then
// requests dispatched through the Dispatcher. One detached reader thread
// per connection admits one request at a time; a terminal response releases
// admission even if the completed handler still has cleanup to finish.
class Server {
  public:
    Server(ServerOptions options, const Dispatcher& dispatcher);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Validates the private socket directory and any stale endpoints, binds
    // (0600), listens, and starts the accept thread. False + reason on failure.
    bool start(std::string& error);

    // Stops accepting new work and sends a shutdown terminal to every active
    // request that has not already completed. Safe to call from a handler
    // thread (`daemon stop`); the owning thread performs actual teardown.
    void request_stop();
    bool stop_requested() const;

    // Blocks until request_stop() has claimed every non-stop terminal and
    // every admitted daemon-stop request has emitted its success terminal.
    void wait_for_stop();

    // Graceful teardown (DESIGN.md §10): stop accepting, shut down live
    // connections, wait for their threads, unlink the socket. Idempotent.
    // Must not be called from a connection thread.
    void stop();

  private:
    struct ActiveRequest {
        std::shared_ptr<RequestSession> session;
        bool shutdown_request = false;
    };

    void accept_loop();
    bool consume_control_request();
    void serve_connection(const std::shared_ptr<ConnectionState>& connection);
    void start_runtime_lifecycle();
    void stop_runtime_lifecycle();
    static bool peer_uid_ok(int fd);

    ServerOptions options_;
    const Dispatcher& dispatcher_;
    ActivityTracker activity_;
    cancellation::Thread activity_watcher_;
    cancellation::Source admission_cancellation_;
    int listen_fd_ = -1;
    int control_fd_ = -1;
    std::optional<paths::SocketIdentity> socket_identity_;
    std::optional<paths::SocketIdentity> control_socket_identity_;
    // Self-pipe waking the accept loop: shutdown() on a listening socket
    // does not unblock accept() on macOS/BSD, so the loop polls on both fds.
    int wake_read_fd_ = -1;
    int wake_write_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> stopping_{false};
    bool shutdown_terminals_sent_ = false;
    std::thread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;
    std::vector<std::shared_ptr<ConnectionState>> connections_;
    std::vector<ActiveRequest> active_requests_;
    int active_connections_ = 0;
    std::uint64_t next_connection_id_ = 1;
};

} // namespace tgcli::daemon
