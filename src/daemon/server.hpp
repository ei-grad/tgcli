#pragma once

#include "daemon/dispatch.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tgcli::daemon {

struct ServerOptions {
    std::string socket_path;
    std::string binary_version;
    int protocol_version = 0;
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

    // Binds (0600), listens, starts the accept thread. The caller holds the
    // account lock, so an existing socket file is stale and is removed.
    // False + reason on failure.
    bool start(std::string& error);

    // Flags the server for shutdown without tearing anything down; safe to
    // call from a handler thread (`daemon stop`) — the owning thread
    // observes it via stop_requested() and performs the actual stop().
    void request_stop();
    bool stop_requested() const;

    // Blocks the calling thread until request_stop() has been observed. The
    // daemon's owning thread parks here instead of polling.
    void wait_for_stop();

    // Graceful teardown (DESIGN.md §10): stop accepting, shut down live
    // connections, wait for their threads, unlink the socket. Idempotent.
    // Must not be called from a connection thread.
    void stop();

  private:
    void accept_loop();
    void serve_connection(int fd);
    static bool peer_uid_ok(int fd);

    ServerOptions options_;
    const Dispatcher& dispatcher_;
    int listen_fd_ = -1;
    // Self-pipe waking the accept loop: shutdown() on a listening socket
    // does not unblock accept() on macOS/BSD, so the loop polls on both fds.
    int wake_read_fd_ = -1;
    int wake_write_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;
    std::vector<int> connection_fds_;
    int active_connections_ = 0;
};

} // namespace tgcli::daemon
