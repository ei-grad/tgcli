#pragma once

#include "common/paths.hpp"
#include "daemon/dispatch.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tgcli::daemon {

class ConnectionState;

struct ServerOptions {
    std::string socket_path;
    std::string binary_version;
    int protocol_version = 0;
    std::string control_socket_path;
    std::string control_token;
};

// Accepts connections on the account's unix socket and serves the JSONL
// frame protocol (DESIGN.md §10): peer-uid check, hello handshake, then
// requests dispatched through the Dispatcher. One detached thread per
// connection; requests on a connection are served sequentially, concurrency
// comes from concurrent connections.
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
        std::shared_ptr<ResponseSink> sink;
        bool shutdown_request = false;
    };

    void accept_loop();
    bool consume_control_request();
    void serve_connection(const std::shared_ptr<ConnectionState>& connection);
    static bool peer_uid_ok(int fd);

    ServerOptions options_;
    const Dispatcher& dispatcher_;
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
};

} // namespace tgcli::daemon
