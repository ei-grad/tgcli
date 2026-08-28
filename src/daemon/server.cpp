#include "daemon/server.hpp"

#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "daemon/request_session.hpp"
#include "proto/frame_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

class ConnectionState {
  public:
    ConnectionState(int fd, std::uint64_t id) : fd_(fd), id_(id) {}
    ConnectionState(const ConnectionState&) = delete;
    ConnectionState& operator=(const ConnectionState&) = delete;
    ConnectionState(ConnectionState&&) = delete;
    ConnectionState& operator=(ConnectionState&&) = delete;

    ~ConnectionState() {
        ::close(fd_);
    }

    [[nodiscard]] int fd() const {
        return fd_;
    }

    [[nodiscard]] std::uint64_t id() const {
        return id_;
    }

    bool send(const proto::Frame& frame) {
        std::string error;
        const std::lock_guard<std::mutex> lock(write_mutex_);
        return proto::write_frame_until(fd_, frame,
                                        std::chrono::steady_clock::now() + kWriteTimeout, error);
    }

    void shutdown() const {
        ::shutdown(fd_, SHUT_RDWR);
    }

  private:
    static constexpr auto kWriteTimeout = std::chrono::seconds(5);

    int fd_;
    std::uint64_t id_;
    std::mutex write_mutex_;
};

namespace {

enum class HandshakeState { AwaitingHello, Matched, BinaryMismatch };

class AdmissionCancellation {
  public:
    AdmissionCancellation(int socket_fd, const std::stop_token& shutdown) : socket_fd_(socket_fd) {
        if (!net::pipe_cloexec(wake_read_fd_, wake_write_fd_)) {
            throw std::runtime_error("cannot create config-admission wake pipe");
        }
        monitor_ = std::thread([this] { monitor(); });
        shutdown_callback_.emplace(shutdown, std::function<void()>([this] {
                                       cancellation_.request_stop();
                                       wake_monitor();
                                   }));
    }

    ~AdmissionCancellation() {
        static_cast<void>(finish());
        ::close(wake_read_fd_);
        ::close(wake_write_fd_);
    }

    AdmissionCancellation(const AdmissionCancellation&) = delete;
    AdmissionCancellation& operator=(const AdmissionCancellation&) = delete;
    AdmissionCancellation(AdmissionCancellation&&) = delete;
    AdmissionCancellation& operator=(AdmissionCancellation&&) = delete;

    [[nodiscard]] std::stop_token token() const {
        return cancellation_.get_token();
    }

    bool finish() {
        if (!finished_) {
            shutdown_callback_.reset();
            wake_monitor();
            if (monitor_.joinable()) {
                monitor_.join();
            }
            finished_ = true;
        }
        return peer_disconnected_.load(std::memory_order_acquire);
    }

  private:
    void wake_monitor() const {
        constexpr char byte = 1;
        auto written = ::write(wake_write_fd_, &byte, 1);
        while (written < 0 && errno == EINTR) {
            written = ::write(wake_write_fd_, &byte, 1);
        }
    }

    void disconnect() {
        peer_disconnected_.store(true, std::memory_order_release);
        cancellation_.request_stop();
    }

    void monitor() {
        std::array<pollfd, 2> descriptors{{{socket_fd_, POLLIN, 0}, {wake_read_fd_, POLLIN, 0}}};
        for (;;) {
            const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                disconnect();
                return;
            }
            const auto socket_events = descriptors[0].revents;
            if ((socket_events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                disconnect();
                return;
            }
            if ((socket_events & POLLIN) != 0) {
                char byte = 0;
                const auto count = ::recv(socket_fd_, &byte, 1, MSG_PEEK);
                if (count == 0) {
                    disconnect();
                    return;
                }
                if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    disconnect();
                    return;
                }
                if (count > 0) {
                    descriptors[0].events = 0;
                }
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
                return;
            }
        }
    }

    using ShutdownCallback = std::stop_callback<std::function<void()>>;

    int socket_fd_ = -1;
    int wake_read_fd_ = -1;
    int wake_write_fd_ = -1;
    std::stop_source cancellation_;
    std::optional<ShutdownCallback> shutdown_callback_;
    std::thread monitor_;
    std::atomic<bool> peer_disconnected_{false};
    bool finished_ = false;
};

bool is_canonical_binary_mismatch_stop(const proto::Request& request) {
    return request.id == 1 && request.command == std::vector<std::string>{"daemon", "stop"} &&
           request.args.is_object() && request.args.empty() && !request.context.tty &&
           !request.context.json && !request.context.yes && !request.context.dry_run &&
           !request.context.timeout_seconds && request.context.cwd == "/" &&
           !request.context.media_dir &&
           request.context.write_authority == proto::WriteAuthority::Unset &&
           !request.context.idempotency_key;
}

class ConnectionSink final : public ResponseSink {
  public:
    using TerminalHook = std::function<void(bool)>;

    ConnectionSink(std::shared_ptr<ConnectionState> connection, std::uint64_t request_id)
        : connection_(std::move(connection)), request_id_(request_id) {}

    void set_terminal_hook(TerminalHook hook) {
        terminal_hook_ = std::move(hook);
    }

  private:
    DeliveryOutcome emit_item(nlohmann::json data) override {
        bool complete = false;
        try {
            complete = connection_->send(proto::Item{request_id_, std::move(data)});
        } catch (...) {
            // Frame serialization and the connection writer expose non-enumerable exception
            // types. A begun protocol frame cannot be retried safely.
            abort();
            return DeliveryOutcome::Disconnected;
        }
        if (!complete) {
            abort();
        }
        return complete ? DeliveryOutcome::Complete : DeliveryOutcome::Disconnected;
    }
    void emit_progress(nlohmann::json data) override {
        try {
            if (!connection_->send(proto::Progress{request_id_, std::move(data)})) {
                abort();
            }
        } catch (...) {
            // Progress serialization failure is a connection abort; no later frame is retried.
            abort();
        }
    }
    DeliveryOutcome emit_result(nlohmann::json data) override {
        bool visible = false;
        try {
            visible = connection_->send(proto::Result{request_id_, std::move(data)});
        } catch (...) {
            // See emit_item: terminal serialization failure is disconnect without retry.
            abort();
            return DeliveryOutcome::Disconnected;
        }
        if (!visible) {
            connection_->shutdown();
        }
        notify_terminal(visible);
        return visible ? DeliveryOutcome::Complete : DeliveryOutcome::Disconnected;
    }
    DeliveryOutcome emit_error(std::string code, std::string message, nlohmann::json details,
                               int exit_code) override {
        bool visible = false;
        try {
            visible = connection_->send(proto::Error{
                request_id_, std::move(code), std::move(message), std::move(details), exit_code});
        } catch (...) {
            // See emit_item: terminal serialization failure is disconnect without retry.
            abort();
            return DeliveryOutcome::Disconnected;
        }
        if (!visible) {
            connection_->shutdown();
        }
        notify_terminal(visible);
        return visible ? DeliveryOutcome::Complete : DeliveryOutcome::Disconnected;
    }
    ChallengeReply emit_challenge(nlohmann::json data) override {
        try {
            if (!connection_->send(proto::Challenge{request_id_, std::move(data)})) {
                abort();
                return disconnected_challenge();
            }
        } catch (...) {
            // Challenge serialization failure follows the same transport-abort path.
            abort();
            return disconnected_challenge();
        }
        return {};
    }
    void emit_abort() noexcept override {
        abort();
    }

    void notify_terminal(bool visible) noexcept {
        if (terminal_notified_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            terminal_hook_(visible);
        } catch (...) {
            // Server lifecycle hooks are not allowed to unwind through transport teardown.
            connection_->shutdown();
        }
    }

    void abort() noexcept {
        connection_->shutdown();
        notify_terminal(false);
    }

    static ChallengeReply disconnected_challenge() {
        return {{},
                ChallengeFailure{"INTERNAL", "transport disconnected", nlohmann::json::object(),
                                 kGeneric}};
    }

    std::shared_ptr<ConnectionState> connection_;
    std::uint64_t request_id_;
    TerminalHook terminal_hook_ = [](bool) {};
    std::atomic<bool> terminal_notified_{false};
};

} // namespace

Server::Server(ServerOptions options, const Dispatcher& dispatcher)
    : options_(std::move(options)), dispatcher_(dispatcher),
      activity_([this] { request_stop(); }, options_.activity_hooks) {}

Server::~Server() {
    stop();
}

bool Server::start(std::string& error) {
    if (options_.control_socket_path.empty() != options_.control_token.empty()) {
        error = "control socket path and token must be configured together";
        return false;
    }
    if (!paths::prepare_socket_endpoint(options_.socket_path, getuid(), error)) {
        return false;
    }
    if (!options_.control_socket_path.empty() &&
        !paths::prepare_socket_endpoint(options_.control_socket_path, getuid(), error)) {
        return false;
    }
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
    const mode_t old_umask = ::umask(0177); // socket mode 0600 (§10)
    const int bind_rc = ::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::umask(old_umask);
    if (bind_rc != 0) {
        error = "bind " + options_.socket_path + ": " + std::strerror(errno);
        return false;
    }
    socket_identity_ = paths::inspect_socket_endpoint(options_.socket_path, getuid(), error);
    if (!socket_identity_) {
        return false;
    }
    if (::listen(listen_fd_, SOMAXCONN) != 0) {
        error = "listen: " + std::string(std::strerror(errno));
        return false;
    }
    if (!options_.control_socket_path.empty()) {
        control_fd_ = net::socket_cloexec(AF_UNIX, SOCK_DGRAM, 0);
        if (control_fd_ < 0) {
            error = std::string("control socket: ") + std::strerror(errno);
            return false;
        }
        if (::fcntl(control_fd_, F_SETFL, O_NONBLOCK) != 0) {
            error = std::string("control socket fcntl: ") + std::strerror(errno);
            return false;
        }
        sockaddr_un control_addr{};
        control_addr.sun_family = AF_UNIX;
        if (options_.control_socket_path.size() >= sizeof(control_addr.sun_path)) {
            error = "control socket path too long: " + options_.control_socket_path;
            return false;
        }
        std::strncpy(control_addr.sun_path, options_.control_socket_path.c_str(),
                     sizeof(control_addr.sun_path) - 1);
        const mode_t control_umask = ::umask(0177);
        const int control_bind_rc = ::bind(
            control_fd_, reinterpret_cast<const sockaddr*>(&control_addr), sizeof(control_addr));
        ::umask(control_umask);
        if (control_bind_rc != 0) {
            error = "bind " + options_.control_socket_path + ": " + std::strerror(errno);
            return false;
        }
        control_socket_identity_ =
            paths::inspect_socket_endpoint(options_.control_socket_path, getuid(), error);
        if (!control_socket_identity_) {
            return false;
        }
    }
    start_runtime_lifecycle();
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void Server::start_runtime_lifecycle() {
    const auto idle_exit = options_.config_runtime != nullptr
                               ? options_.config_runtime->current(options_.account()).idle_exit
                               : ActivityTracker::IdleExit{};
    if (!activity_.daemon_ready(idle_exit)) {
        throw std::logic_error("server activity lifecycle was already started");
    }
    if (options_.config_runtime != nullptr) {
        options_.config_runtime->set_publication_observer([this] {
            const auto current = options_.config_runtime->current(options_.account());
            static_cast<void>(activity_.update_idle_exit(current.idle_exit));
        });
    }
    activity_watcher_ =
        std::jthread([this](const std::stop_token& stop) { activity_.watch(stop); });
}

void Server::stop_runtime_lifecycle() {
    if (options_.config_runtime != nullptr) {
        options_.config_runtime->set_publication_observer({});
    }
    activity_watcher_.request_stop();
    if (activity_watcher_.joinable()) {
        activity_watcher_.join();
    }
}

void Server::request_stop() {
    std::vector<std::shared_ptr<RequestSession>> active_requests;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
            idle_cv_.wait(lock, [this] { return shutdown_terminals_sent_; });
            return;
        }
        admission_cancellation_.request_stop();
        for (const auto& active : active_requests_) {
            if (!active.shutdown_request) {
                active_requests.push_back(active.session);
            }
        }
    }
    if (wake_write_fd_ >= 0) {
        const char byte = 0;
        [[maybe_unused]] const ssize_t count = ::write(wake_write_fd_, &byte, 1);
    }
    for (const auto& session : active_requests) {
        session->shutdown();
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        shutdown_terminals_sent_ = true;
    }
    idle_cv_.notify_all();
}

bool Server::stop_requested() const {
    return stop_requested_.load(std::memory_order_acquire);
}

void Server::wait_for_stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] {
        return shutdown_terminals_sent_ &&
               std::none_of(active_requests_.begin(), active_requests_.end(),
                            [](const ActiveRequest& active) { return active.shutdown_request; });
    });
}

void Server::stop() {
    request_stop();
    if (stopping_.exchange(true)) {
        return;
    }
    stop_runtime_lifecycle();
    // request_stop() wakes the accept loop before active sessions are closed,
    // so no new connection can appear after the sweep below.
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::vector<std::shared_ptr<ConnectionState>> connections;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_cv_.wait(lock, [this] {
            return std::none_of(
                active_requests_.begin(), active_requests_.end(),
                [](const ActiveRequest& active) { return active.shutdown_request; });
        });
        connections = connections_;
    }
    for (const auto& connection : connections) {
        connection->shutdown();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] { return active_connections_ == 0; });
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (control_fd_ >= 0) {
        ::close(control_fd_);
        control_fd_ = -1;
    }
    for (int* fd : {&wake_read_fd_, &wake_write_fd_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    if (socket_identity_) {
        paths::unlink_socket_endpoint_if_same(options_.socket_path, *socket_identity_);
        socket_identity_.reset();
    }
    if (control_socket_identity_) {
        paths::unlink_socket_endpoint_if_same(options_.control_socket_path,
                                              *control_socket_identity_);
        control_socket_identity_.reset();
    }
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
        std::array<pollfd, 3> fds{
            {{listen_fd_, POLLIN, 0}, {wake_read_fd_, POLLIN, 0}, {control_fd_, POLLIN, 0}}};
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if ((fds[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            break; // stop() wrote to the self-pipe
        }
        if ((fds[2].revents & POLLIN) != 0 && consume_control_request()) {
            request_stop();
            break;
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
        auto connection = std::make_shared<ConnectionState>(fd, next_connection_id_++);
        connections_.push_back(connection);
        ++active_connections_;
        // Detached: serve_connection's final act is signalling idle_cv_
        // under mutex_, which stop() holds until the count reaches zero, so
        // the thread never outlives the Server.
        std::thread([this, connection = std::move(connection)] {
            serve_connection(connection);
        }).detach();
    }
}

bool Server::consume_control_request() {
    std::array<char, 256> data{};
    const ssize_t count = ::recv(control_fd_, data.data(), data.size(), 0);
    if (count < 0) {
        return false;
    }
    return static_cast<std::size_t>(count) == options_.control_token.size() &&
           std::equal(options_.control_token.begin(), options_.control_token.end(), data.begin());
}

// This reader loop is the connection-ordering boundary: Hello, Answer, EOF,
// admission and worker replacement must be classified in wire order.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Server::serve_connection(const std::shared_ptr<ConnectionState>& connection) {
    connection->send(proto::Hello{options_.binary_version, options_.protocol_version});

    proto::FrameReader reader(connection->fd(), options_.wipe_observer);
    HandshakeState handshake = HandshakeState::AwaitingHello;
    std::shared_ptr<RequestSession> active_session;
    std::shared_ptr<std::atomic<bool>> active_terminal_visible;
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::optional<Worker> active_worker;
    std::vector<Worker> retired_workers;
    const auto reap_workers = [&retired_workers] {
        std::erase_if(retired_workers, [](Worker& retired) {
            if (!retired.done->load(std::memory_order_acquire)) {
                return false;
            }
            retired.thread.join();
            return true;
        });
    };
    const auto release_terminal_admission = [&] {
        if (active_session && active_terminal_visible &&
            active_terminal_visible->load(std::memory_order_acquire)) {
            retired_workers.push_back(std::move(*active_worker));
            active_worker.reset();
            active_session.reset();
            active_terminal_visible.reset();
        }
        reap_workers();
    };
    try {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::string io_error;
            auto line = reader.read_line(io_error);
            if (!line) {
                break; // EOF or broken connection; nothing sensible to send back
            }
            release_terminal_admission();
            std::string parse_error;
            std::optional<proto::Answer> answer_candidate;
            auto frame = proto::parse(std::move(*line), parse_error, options_.wipe_observer,
                                      handshake != HandshakeState::AwaitingHello ? &answer_candidate
                                                                                 : nullptr);
            if (!frame) {
                if (answer_candidate) {
                    if (handshake == HandshakeState::BinaryMismatch) {
                        connection->send(proto::Error{0, "USAGE",
                                                      "binary-mismatched client may only stop the "
                                                      "daemon",
                                                      nlohmann::json::object(), kUsage});
                        break;
                    }
                    if (active_session) {
                        active_session->receive_answer(std::move(*answer_candidate));
                    } else {
                        connection->send(proto::Error{
                            answer_candidate->id,
                            "PROTOCOL_ANSWER_INVALID",
                            "invalid challenge answer",
                            {{"request_id", answer_candidate->id}, {"reason", "unknown_request"}},
                            kUsage});
                    }
                    continue;
                }
                connection->send(proto::Error{0, "USAGE", "malformed frame: " + parse_error,
                                              nlohmann::json::object(), kUsage});
                break;
            }
            if (const auto* hello = std::get_if<proto::Hello>(&*frame)) {
                if (handshake != HandshakeState::AwaitingHello) {
                    connection->send(proto::Error{0, "USAGE", "hello frame was already accepted",
                                                  nlohmann::json::object(), kUsage});
                    break;
                }
                if (hello->protocol_version != options_.protocol_version) {
                    connection->send(proto::Error{
                        0, "PROTOCOL_MISMATCH",
                        "daemon speaks protocol " + std::to_string(options_.protocol_version) +
                            ", client sent " + std::to_string(hello->protocol_version),
                        nlohmann::json::object(), kGeneric});
                    break;
                }
                handshake = hello->binary_version == options_.binary_version
                                ? HandshakeState::Matched
                                : HandshakeState::BinaryMismatch;
                continue;
            }
            if (auto* answer = std::get_if<proto::Answer>(&*frame)) {
                if (handshake == HandshakeState::AwaitingHello) {
                    connection->send(proto::Error{0, "USAGE", "expected a hello frame first",
                                                  nlohmann::json::object(), kUsage});
                    break;
                }
                if (handshake == HandshakeState::BinaryMismatch) {
                    connection->send(proto::Error{0, "USAGE",
                                                  "binary-mismatched client may only stop the "
                                                  "daemon",
                                                  nlohmann::json::object(), kUsage});
                    break;
                }
                if (active_session) {
                    active_session->receive_answer(std::move(*answer));
                } else {
                    connection->send(
                        proto::Error{answer->id,
                                     "PROTOCOL_ANSWER_INVALID",
                                     "invalid challenge answer",
                                     {{"request_id", answer->id}, {"reason", "unknown_request"}},
                                     kUsage});
                }
                continue;
            }
            const auto* request = std::get_if<proto::Request>(&*frame);
            if (request == nullptr || handshake == HandshakeState::AwaitingHello) {
                connection->send(proto::Error{0, "USAGE",
                                              handshake == HandshakeState::AwaitingHello
                                                  ? "expected a hello frame first"
                                                  : "expected a request frame",
                                              nlohmann::json::object(), kUsage});
                break;
            }
            if (active_session) {
                connection->send(proto::Error{request->id, "USAGE",
                                              "connection already has an active request",
                                              nlohmann::json::object(), kUsage});
                continue;
            }
            if (request->account != options_.account()) {
                connection->send(proto::Error{request->id,
                                              "ACCOUNT_MISMATCH",
                                              "request account does not match this daemon",
                                              {{"requested_account", request->account},
                                               {"daemon_account", options_.account()}},
                                              kNotFound});
                break;
            }
            if (handshake == HandshakeState::BinaryMismatch &&
                !is_canonical_binary_mismatch_stop(*request)) {
                connection->send(proto::Error{0, "USAGE",
                                              "binary-mismatched client may only stop the daemon",
                                              nlohmann::json::object(), kUsage});
                break;
            }
            const auto admitted_at = RequestClock::now();
            const auto admission_wall_time = options_.request_wall_clock
                                                 ? options_.request_wall_clock()
                                                 : RequestSession::WallClock::now();
            const auto deadline =
                request_deadline(request->context.timeout_seconds,
                                 dispatcher_.deadline_default(*request), admitted_at);
            if (!deadline) {
                connection->send(proto::Error{request->id, "USAGE", "invalid request timeout",
                                              nlohmann::json::object(), kUsage});
                break;
            }
            std::shared_ptr<const AdmittedAccountConfig> admitted_config;
            if (options_.config_runtime != nullptr) {
                if (options_.request_observer) {
                    options_.request_observer(testing::RequestObservationStage::ConfigRead);
                }
                AdmissionCancellation admission_cancellation(connection->fd(),
                                                             admission_cancellation_.get_token());
                const auto admission = options_.config_runtime->admit(
                    request->account, *deadline, admission_cancellation.token());
                if (admission_cancellation.finish()) {
                    break;
                }
                if (admission.refresh_status == ConfigRefreshStatus::TimedOut) {
                    connection->send(
                        proto::Error{request->id,
                                     "TIMEOUT",
                                     "config admission timed out",
                                     {{"operation", "config_admission"}, {"state", nullptr}},
                                     kTimeout});
                    continue;
                }
                if (admission.refresh_status != ConfigRefreshStatus::Completed ||
                    !admission.decision) {
                    connection->send(proto::Error{request->id,
                                                  "DAEMON_SHUTDOWN",
                                                  "daemon is shutting down",
                                                  {{"reason", "daemon_shutdown"}},
                                                  kGeneric});
                    break;
                }
                if (const auto* accepted =
                        std::get_if<std::shared_ptr<const AdmittedAccountConfig>>(
                            &*admission.decision)) {
                    admitted_config = *accepted;
                } else {
                    const auto& denied = std::get<ConfigAdmissionDenied>(*admission.decision);
                    if (denied.state == ConfigAdmissionState::AccountMissing) {
                        connection->send(proto::Error{request->id,
                                                      "ACCOUNT_NOT_FOUND",
                                                      "account is not configured",
                                                      {{"account", denied.account}},
                                                      kNotFound});
                    } else {
                        const auto reason =
                            denied.reload_diagnostic
                                ? config::reason_name(denied.reload_diagnostic->reason)
                                : std::string_view{"io_error"};
                        connection->send(proto::Error{
                            request->id,
                            "CONFIG_INVALID",
                            "cannot use current config.toml",
                            {{"path", options_.config_runtime->config_path()}, {"reason", reason}},
                            kGeneric});
                    }
                    continue;
                }
            }
            if (options_.request_admission_probe) {
                options_.request_admission_probe();
            }
            auto activity = activity_.try_request();
            if (!activity) {
                connection->send(proto::Error{request->id,
                                              "DAEMON_SHUTDOWN",
                                              "daemon is shutting down",
                                              {{"reason", "daemon_shutdown"}},
                                              kGeneric});
                break;
            }
            if (options_.request_observer) {
                options_.request_observer(testing::RequestObservationStage::ActivityAdmission);
            }
            auto sink = std::make_shared<ConnectionSink>(connection, request->id);
            active_session = std::make_shared<RequestSession>(
                *request, sink, connection->id(), RequestSession::NonceGenerator{},
                std::move(*activity), std::move(admitted_config), *deadline,
                ConfigAdmissionMode::FrozenRuntime, admission_wall_time);
            if (options_.request_observer) {
                options_.request_observer(testing::RequestObservationStage::SessionConstruction);
            }
            active_terminal_visible = std::make_shared<std::atomic<bool>>(false);
            sink->set_terminal_hook([this, weak_session = std::weak_ptr(active_session),
                                     terminal_visible = active_terminal_visible](bool visible) {
                terminal_visible->store(visible, std::memory_order_release);
                if (const auto session = weak_session.lock()) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    std::erase_if(active_requests_, [&session](const ActiveRequest& active) {
                        return active.session == session;
                    });
                    idle_cv_.notify_all();
                }
            });
            const bool shutdown_request =
                request->command == std::vector<std::string>{"daemon", "stop"};
            bool shutting_down = false;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                // Admission and stop-request designation share the same lock as
                // the shutdown sweep: either shutdown wins, or an admitted stop
                // remains exempt from shutdown errors until its own terminal.
                shutting_down = stop_requested_.load(std::memory_order_acquire);
                if (!shutting_down) {
                    active_requests_.push_back({active_session, shutdown_request});
                }
            }
            if (shutting_down) {
                active_session->shutdown();
                break;
            }
            auto worker_done = std::make_shared<std::atomic<bool>>(false);
            std::thread worker([this, session = active_session, worker_done] {
                dispatcher_.dispatch(*session);
                worker_done->store(true, std::memory_order_release);
            });
            active_worker = Worker{std::move(worker), std::move(worker_done)};
        }
    } catch (...) {
        // No exception may cross a detached reader boundary: doing so invokes
        // std::terminate. Close only this connection and run common cleanup.
        connection->shutdown();
    }
    if (active_session) {
        active_session->disconnect();
    }
    if (active_worker) {
        active_worker->thread.join();
    }
    for (auto& retired : retired_workers) {
        retired.thread.join();
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::erase(connections_, connection);
        --active_connections_;
        idle_cv_.notify_all();
    }
}

} // namespace tgcli::daemon
