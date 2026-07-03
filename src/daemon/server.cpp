#include "daemon/server.hpp"

#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "proto/frame_io.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

namespace {

// Serializes concurrent frame writes on one connection (a handler thread and
// a future stream multiplexer may share it).
struct Connection {
    int fd;
    std::mutex write_mutex;

    bool send(const proto::Frame& frame) {
        std::string error;
        const std::lock_guard<std::mutex> lock(write_mutex);
        return proto::write_frame(fd, frame, error);
    }
};

class ConnectionSink final : public ResponseSink {
  public:
    ConnectionSink(Connection& connection, std::uint64_t request_id)
        : connection_(connection), request_id_(request_id) {}

    void item(nlohmann::json data) override {
        connection_.send(proto::Item{request_id_, std::move(data)});
    }
    void progress(nlohmann::json data) override {
        connection_.send(proto::Progress{request_id_, std::move(data)});
    }
    void result(nlohmann::json data) override {
        connection_.send(proto::Result{request_id_, std::move(data)});
    }
    void error(std::string code, std::string message, nlohmann::json details,
               int exit_code) override {
        connection_.send(proto::Error{request_id_, std::move(code), std::move(message),
                                      std::move(details), exit_code});
    }

  private:
    Connection& connection_;
    std::uint64_t request_id_;
};

} // namespace

Server::Server(ServerOptions options, const Dispatcher& dispatcher)
    : options_(std::move(options)), dispatcher_(dispatcher) {}

Server::~Server() {
    stop();
}

bool Server::start(std::string& error) {
    if (!net::pipe_cloexec(wake_read_fd_, wake_write_fd_)) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    listen_fd_ = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    // Non-blocking so a connection aborted between poll() and accept()
    // cannot re-block the loop.
    if (::fcntl(listen_fd_, F_SETFL, O_NONBLOCK) != 0) {
        error = std::string("fcntl: ") + std::strerror(errno);
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (options_.socket_path.size() >= sizeof(addr.sun_path)) {
        error = "socket path too long: " + options_.socket_path;
        return false;
    }
    std::strncpy(addr.sun_path, options_.socket_path.c_str(), sizeof(addr.sun_path) - 1);
    // The caller holds the account flock, so an existing file is a stale
    // leftover, never a live daemon's socket.
    ::unlink(options_.socket_path.c_str());
    const mode_t old_umask = ::umask(0177); // socket mode 0600 (§10)
    const int bind_rc = ::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::umask(old_umask);
    if (bind_rc != 0) {
        error = "bind " + options_.socket_path + ": " + std::strerror(errno);
        return false;
    }
    if (::listen(listen_fd_, SOMAXCONN) != 0) {
        error = "listen: " + std::string(std::strerror(errno));
        return false;
    }
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void Server::request_stop() {
    {
        // Mutate the flag under the lock so a concurrent wait_for_stop() can
        // never miss the transition between its predicate check and its wait.
        const std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_.store(true, std::memory_order_release);
    }
    idle_cv_.notify_all();
}

bool Server::stop_requested() const {
    return stop_requested_.load(std::memory_order_acquire);
}

void Server::wait_for_stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] { return stop_requested_.load(std::memory_order_acquire); });
}

void Server::stop() {
    request_stop();
    if (stopping_.exchange(true)) {
        return;
    }
    // The self-pipe wakes the accept loop's poll() — shutdown() on a
    // listening socket does not unblock accept() on macOS/BSD. Joining the
    // accept thread first guarantees no new connection appears after the
    // sweep below.
    if (wake_write_fd_ >= 0) {
        const char byte = 0;
        [[maybe_unused]] const ssize_t n = ::write(wake_write_fd_, &byte, 1);
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    for (const int fd : connection_fds_) {
        ::shutdown(fd, SHUT_RDWR);
    }
    idle_cv_.wait(lock, [this] { return active_connections_ == 0; });
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    for (int* fd : {&wake_read_fd_, &wake_write_fd_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    ::unlink(options_.socket_path.c_str());
}

bool Server::peer_uid_ok(int fd) {
#if defined(__APPLE__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0) {
        return false;
    }
    return uid == ::getuid();
#else
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return false;
    }
    return cred.uid == ::getuid();
#endif
}

void Server::accept_loop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        std::array<pollfd, 2> fds{{{listen_fd_, POLLIN, 0}, {wake_read_fd_, POLLIN, 0}}};
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if ((fds[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            break; // stop() wrote to the self-pipe
        }
        if ((fds[0].revents & POLLIN) == 0) {
            continue;
        }
        const int fd = net::accept_cloexec(listen_fd_);
        if (fd < 0) {
            // The listener is non-blocking: a connection gone between poll()
            // and accept() surfaces here instead of blocking the loop.
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            break;
        }
        // BSD sockets inherit the listener's O_NONBLOCK; connection reads
        // must block (Linux never sets it on accepted fds — harmless there).
        if (const int flags = ::fcntl(fd, F_GETFL); flags >= 0) {
            ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        if (!peer_uid_ok(fd)) {
            ::close(fd);
            continue;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            ::close(fd);
            break;
        }
        connection_fds_.push_back(fd);
        ++active_connections_;
        // Detached: serve_connection's final act is signalling idle_cv_
        // under mutex_, which stop() holds until the count reaches zero, so
        // the thread never outlives the Server.
        std::thread([this, fd] { serve_connection(fd); }).detach();
    }
}

void Server::serve_connection(int fd) {
    Connection connection{fd, {}};
    connection.send(proto::Hello{options_.binary_version, options_.protocol_version});

    proto::FrameReader reader(fd);
    bool hello_seen = false;
    while (!stopping_.load(std::memory_order_acquire)) {
        std::string io_error;
        const auto line = reader.read_line(io_error);
        if (!line) {
            break; // EOF or broken connection; nothing sensible to send back
        }
        std::string parse_error;
        auto frame = proto::parse(*line, parse_error);
        if (!frame) {
            connection.send(proto::Error{0, "USAGE", "malformed frame: " + parse_error,
                                         nlohmann::json::object(), kUsage});
            break;
        }
        if (const auto* hello = std::get_if<proto::Hello>(&*frame)) {
            hello_seen = true;
            if (hello->protocol_version != options_.protocol_version) {
                connection.send(proto::Error{
                    0, "PROTOCOL_MISMATCH",
                    "daemon speaks protocol " + std::to_string(options_.protocol_version) +
                        ", client sent " + std::to_string(hello->protocol_version),
                    nlohmann::json::object(), kGeneric});
                break;
            }
            continue;
        }
        const auto* request = std::get_if<proto::Request>(&*frame);
        if (request == nullptr || !hello_seen) {
            connection.send(proto::Error{0, "USAGE",
                                         hello_seen ? "expected a request frame"
                                                    : "expected a hello frame first",
                                         nlohmann::json::object(), kUsage});
            break;
        }
        ConnectionSink sink(connection, request->id);
        dispatcher_.dispatch(*request, sink);
    }
    ::close(fd);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::erase(connection_fds_, fd);
        --active_connections_;
        idle_cv_.notify_all();
    }
}

} // namespace tgcli::daemon
