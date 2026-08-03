#include "cli/client.hpp"

#include "cli/prompt.hpp"
#include "cli/render.hpp"
#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "daemon/daemon_run.hpp"
#include "proto/frame_io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tgcli/version.hpp>
#include <thread>
#include <unistd.h>
#include <variant>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace tgcli::cli {

namespace {

using nlohmann::json;

void print_error(const std::string& code, const std::string& message, const json& details) {
    const json error{{"error", {{"code", code}, {"message", message}, {"details", details}}}};
    std::fputs((error.dump() + "\n").c_str(), stderr);
}

std::string command_key(const std::vector<std::string>& command) {
    std::string key;
    for (const auto& part : command) {
        if (!key.empty()) {
            key += ' ';
        }
        key += part;
    }
    return key;
}

// Renders response frames and remembers the terminal outcome.
class FrameRenderer {
  public:
    FrameRenderer(std::string command, bool json_mode)
        : command_(std::move(command)), json_(json_mode) {}

    static void on_item(const json& data) {
        // Streams are NDJSON in both modes until a command needs more.
        std::fputs((data.dump() + "\n").c_str(), stdout);
    }

    void on_progress(const json& data) const {
        if (json_) {
            std::fputs((json{{"progress", data}}.dump() + "\n").c_str(), stderr);
        } else {
            std::fputs(("progress: " + data.dump() + "\n").c_str(), stderr);
        }
    }

    void on_result(const json& data) {
        if (json_) {
            std::fputs((data.dump() + "\n").c_str(), stdout);
        } else {
            std::fputs(render_human(command_, data).c_str(), stdout);
        }
        exit_code_ = kOk;
        done_ = true;
    }

    void on_error(const std::string& code, const std::string& message, const json& details,
                  int exit_code) {
        print_error(code, message, details);
        exit_code_ = exit_code;
        done_ = true;
    }

    [[nodiscard]] bool done() const {
        return done_;
    }
    [[nodiscard]] int exit_code() const {
        return exit_code_;
    }

  private:
    std::string command_;
    bool json_;
    bool done_ = false;
    int exit_code_ = kGeneric;
};

class InProcessSink final : public daemon::ResponseSink {
  public:
    InProcessSink(FrameRenderer& renderer, ChallengePrompt& prompt, bool tty)
        : renderer_(renderer), prompt_(prompt), tty_(tty) {}

  private:
    void emit_item(json data) override {
        FrameRenderer::on_item(data);
    }
    void emit_progress(json data) override {
        renderer_.on_progress(data);
    }
    void emit_result(json data) override {
        renderer_.on_result(data);
    }
    void emit_error(std::string code, std::string message, json details, int exit_code) override {
        renderer_.on_error(code, message, details, exit_code);
    }
    daemon::ChallengeReply emit_challenge(json challenge) override {
        if (!tty_) {
            return {{},
                    daemon::ChallengeFailure{"INTERNAL", "daemon challenged a non-TTY client",
                                             json::object(), kGeneric}};
        }
        auto response = prompt_.prompt(challenge);
        switch (response.kind) {
        case PromptResultKind::Answer:
        case PromptResultKind::Cancelled:
            return {std::move(response.answer), std::nullopt};
        case PromptResultKind::Unavailable:
        case PromptResultKind::Error:
            return {{},
                    daemon::ChallengeFailure{"INTERNAL", "cannot read challenge response",
                                             json::object(), kGeneric}};
        }
        return {{},
                daemon::ChallengeFailure{"INTERNAL", "cannot read challenge response",
                                         json::object(), kGeneric}};
    }

    FrameRenderer& renderer_;
    ChallengePrompt& prompt_;
    bool tty_;
};

using Deadline = proto::IoDeadline;

bool sleep_until_retry(Deadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return false;
    }
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds(25),
                 std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    return true;
}

bool wait_for_fd(int fd, short events, Deadline deadline, std::string_view operation,
                 std::string& error) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = "timed out " + std::string(operation);
            return false;
        }
        const auto remaining = deadline - now;
        auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (timeout < remaining) {
            timeout += std::chrono::milliseconds(1);
        }
        const auto bounded = std::min<std::chrono::milliseconds::rep>(
            timeout.count(), std::numeric_limits<int>::max());
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(bounded));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            error = "poll while " + std::string(operation) + ": " + std::strerror(errno);
            return false;
        }
        if (result == 0) {
            error = "timed out " + std::string(operation);
            return false;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            error = "invalid descriptor while " + std::string(operation);
            return false;
        }
        return true;
    }
}

struct ConnectedSocket {
    int fd = -1;
    paths::SocketIdentity identity;
};

enum class ConnectStatus { Connected, Unavailable, Failed };

ConnectStatus connect_socket(const std::string& socket_path, uid_t uid, Deadline deadline,
                             ConnectedSocket& connected, std::string& error) {
    std::optional<paths::SocketIdentity> identity;
    if (!paths::find_socket_endpoint(socket_path, uid, identity, error)) {
        return ConnectStatus::Failed;
    }
    if (!identity) {
        error = "socket is not present: " + socket_path;
        return ConnectStatus::Unavailable;
    }

    const int fd = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "cannot create client socket: " + std::string(std::strerror(errno));
        return ConnectStatus::Failed;
    }
    const int old_flags = ::fcntl(fd, F_GETFL);
    if (old_flags < 0 || ::fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) != 0) {
        error = "cannot make client socket non-blocking: " + std::string(std::strerror(errno));
        ::close(fd);
        return ConnectStatus::Failed;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    int connect_error = 0;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) {
            connect_error = errno;
        } else if (!wait_for_fd(fd, POLLOUT, deadline, "connecting to daemon", error)) {
            ::close(fd);
            return ConnectStatus::Failed;
        } else {
            socklen_t length = sizeof(connect_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_error, &length) != 0) {
                error = "cannot inspect daemon connection: " + std::string(std::strerror(errno));
                ::close(fd);
                return ConnectStatus::Failed;
            }
        }
    }
    if (connect_error != 0) {
        error = "cannot connect to " + socket_path + ": " + std::strerror(connect_error);
        ::close(fd);
        return connect_error == ECONNREFUSED || connect_error == ENOENT ? ConnectStatus::Unavailable
                                                                        : ConnectStatus::Failed;
    }
    if (::fcntl(fd, F_SETFL, old_flags) != 0) {
        error = "cannot restore client socket flags: " + std::string(std::strerror(errno));
        ::close(fd);
        return ConnectStatus::Failed;
    }
    const auto current = paths::inspect_socket_endpoint(socket_path, uid, error);
    if (!current) {
        ::close(fd);
        return ConnectStatus::Unavailable;
    }
    if (*current != *identity) {
        error = "daemon socket was replaced while connecting";
        ::close(fd);
        return ConnectStatus::Unavailable;
    }
    connected = ConnectedSocket{fd, *identity};
    return ConnectStatus::Connected;
}

// Absolute path to the running executable, re-exec'd as the daemon. Linux
// uses the /proc self-symlink directly; macOS has no /proc, so ask dyld.
// Resolved before fork so no allocation happens in the forked child.
std::string current_exe_path() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // sets size to the required length
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::strlen(path.c_str()));
    return path;
#else
    return "/proc/self/exe";
#endif
}

[[noreturn]] void run_spawn_child(const std::string& account, const std::string& executable) {
    const pid_t grandchild = ::fork();
    if (grandchild != 0) {
        ::_exit(grandchild < 0 ? 1 : 0);
    }
    ::setsid();
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            ::close(devnull);
        }
    }
    ::execl(executable.c_str(), "tgcli", "daemon", "run", "--account", account.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(127);
}

bool wait_for_spawn_handoff(pid_t child, Deadline deadline, std::string& error) {
    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            break;
        }
        if (waited < 0 && errno != EINTR) {
            error = "waitpid after daemon spawn: " + std::string(std::strerror(errno));
            return false;
        }
        if (!sleep_until_retry(deadline)) {
            ::kill(child, SIGTERM);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            error = "timed out waiting for daemon spawn handoff";
            return false;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "failed to spawn the daemon";
        return false;
    }
    return true;
}

// Double-fork so the daemon is reparented to init and the client never
// leaves a zombie; the grandchild re-execs this binary as `daemon run`.
bool spawn_daemon(const std::string& account, const std::string& executable, Deadline deadline,
                  std::string& error) {
    const std::string exe = executable.empty() ? current_exe_path() : executable;
    if (exe.empty()) {
        error = "cannot determine executable path for daemon re-exec";
        return false;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        run_spawn_child(account, exe);
    }
    return wait_for_spawn_handoff(child, deadline, error);
}

std::optional<ConnectedSocket> connect_with_spawn(const std::string& account,
                                                  const std::string& socket_path, uid_t uid,
                                                  bool auto_spawn,
                                                  const std::string& daemon_executable,
                                                  Deadline deadline, std::string& error) {
    ConnectedSocket connected;
    const auto initial = connect_socket(socket_path, uid, deadline, connected, error);
    if (initial == ConnectStatus::Connected) {
        return connected;
    }
    if (initial == ConnectStatus::Failed) {
        return std::nullopt;
    }
    if (!auto_spawn) {
        return std::nullopt;
    }
    if (!spawn_daemon(account, daemon_executable, deadline, error)) {
        return std::nullopt;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = connect_socket(socket_path, uid, deadline, connected, error);
        if (status == ConnectStatus::Connected) {
            return connected;
        }
        if (status == ConnectStatus::Failed) {
            return std::nullopt;
        }
        if (!sleep_until_retry(deadline)) {
            break;
        }
    }
    error = "timed out waiting for daemon socket " + socket_path;
    return std::nullopt;
}

struct Session {
    int fd = -1;
    paths::SocketIdentity socket_identity;
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

enum class HandshakeOutcome { Ok, BinaryMismatch, ProtocolMismatch, IncompatibleHello, Failed };

HandshakeOutcome handshake(int fd, proto::FrameReader& reader, Deadline deadline,
                           std::string& error) {
    if (std::string io_error; !proto::write_frame_until(
            fd, proto::Hello{kVersion, proto::kProtocolVersion}, deadline, io_error)) {
        error = io_error;
        return HandshakeOutcome::Failed;
    }
    std::string io_error;
    const auto line = reader.read_line_until(deadline, io_error);
    if (!line) {
        error = io_error.empty() ? "daemon closed the connection during handshake" : io_error;
        return HandshakeOutcome::Failed;
    }
    std::string parse_error;
    const auto frame = proto::parse(*line, parse_error);
    if (!frame) {
        error = "malformed handshake frame: " + parse_error;
        return HandshakeOutcome::IncompatibleHello;
    }
    const auto* hello = std::get_if<proto::Hello>(&*frame);
    if (hello == nullptr) {
        error = "daemon did not open with a hello frame";
        return HandshakeOutcome::IncompatibleHello;
    }
    if (hello->protocol_version != proto::kProtocolVersion) {
        return HandshakeOutcome::ProtocolMismatch;
    }
    if (hello->binary_version != kVersion) {
        return HandshakeOutcome::BinaryMismatch;
    }
    return HandshakeOutcome::Ok;
}

struct RestartTarget {
    std::optional<daemon_lock::OwnerWatch> owner;
    paths::SocketIdentity main_identity;
    std::string control_path;
    std::optional<paths::SocketIdentity> control_identity;
};

enum class RestartVerification { Verified, Transition, Invalid };

RestartVerification verify_restart_target(const std::string& account, const paths::Environment& env,
                                          const std::string& socket_path, const Session& session,
                                          std::optional<RestartTarget>& target,
                                          std::string& error) {
    target.reset();
    const std::string state_dir = paths::account_state_dir(account, env);
    if (!paths::validate_private_dir(state_dir, env.uid, error)) {
        return RestartVerification::Invalid;
    }
    const auto control_path = paths::control_socket_path(account, env, error);
    if (!control_path) {
        return RestartVerification::Invalid;
    }
    pid_t peer_pid = -1;
    if (!net::peer_pid(session.fd, peer_pid, error)) {
        return RestartVerification::Invalid;
    }
    std::optional<daemon_lock::OwnerWatch> owner;
    const auto owner_status =
        daemon_lock::inspect_owner(state_dir + "/daemon.lock", env.uid, owner, error);
    if (owner_status == daemon_lock::OwnerStatus::Invalid) {
        return RestartVerification::Invalid;
    }
    std::optional<paths::SocketIdentity> main_identity;
    if (!paths::find_socket_endpoint(socket_path, env.uid, main_identity, error)) {
        return RestartVerification::Invalid;
    }
    if (owner_status == daemon_lock::OwnerStatus::Released ||
        owner_status == daemon_lock::OwnerStatus::Transition || !main_identity ||
        *main_identity != session.socket_identity) {
        error = owner_status == daemon_lock::OwnerStatus::Released
                    ? "daemon lock is not held"
                    : "daemon ownership or endpoint transitioned during restart verification";
        target =
            RestartTarget{std::move(owner), session.socket_identity, *control_path, std::nullopt};
        return RestartVerification::Transition;
    }
    if (!owner || !daemon_lock::owner_pid_matches(owner->identity().pid, peer_pid, error)) {
        return RestartVerification::Invalid;
    }
    std::optional<paths::SocketIdentity> control_identity;
    if (!paths::find_socket_endpoint(*control_path, env.uid, control_identity, error)) {
        return RestartVerification::Invalid;
    }
    if (!control_identity) {
        error = "daemon control endpoint transitioned during restart verification";
        target =
            RestartTarget{std::move(owner), session.socket_identity, *control_path, std::nullopt};
        return RestartVerification::Transition;
    }
    target =
        RestartTarget{std::move(owner), session.socket_identity, *control_path, control_identity};
    return RestartVerification::Verified;
}

bool wait_for_socket_change(const std::string& socket_path, uid_t uid,
                            const paths::SocketIdentity& identity, Deadline deadline,
                            std::string_view timeout_error, std::string& error) {
    for (;;) {
        bool changed = false;
        if (!paths::socket_endpoint_changed(socket_path, uid, identity, changed, error)) {
            return false;
        }
        if (changed) {
            return true;
        }
        if (!sleep_until_retry(deadline)) {
            error = timeout_error;
            return false;
        }
    }
}

bool wait_for_old_daemon_shutdown(RestartTarget& target, const std::string& socket_path, uid_t uid,
                                  Deadline deadline, std::string& error) {
    bool lock_released = !target.owner.has_value();
    bool socket_changed = false;
    bool control_changed = !target.control_identity.has_value();
    for (;;) {
        if (!lock_released && !target.owner->owner_released(lock_released, error)) {
            return false;
        }
        if (!socket_changed && !paths::socket_endpoint_changed(
                                   socket_path, uid, target.main_identity, socket_changed, error)) {
            return false;
        }
        if (!control_changed &&
            !paths::socket_endpoint_changed(target.control_path, uid, *target.control_identity,
                                            control_changed, error)) {
            return false;
        }
        if (lock_released && socket_changed && control_changed) {
            return true;
        }
        if (!sleep_until_retry(deadline)) {
            break;
        }
    }
    error = "timed out waiting for old daemon shutdown (lock " +
            std::string(lock_released ? "released" : "held") + ", socket " +
            (socket_changed ? "replaced/removed" : "still owned") + ", control socket " +
            (control_changed ? "replaced/removed" : "still owned") + ")";
    return false;
}

bool restart_binary_mismatched_daemon(int fd, proto::FrameReader& reader, RestartTarget& target,
                                      const std::string& socket_path, uid_t uid, Deadline deadline,
                                      std::string& error) {
    proto::Request stop_request;
    stop_request.id = 1;
    stop_request.command = {"daemon", "stop"};
    stop_request.context.cwd = "/";
    std::string io_error;
    if (proto::write_frame_until(fd, stop_request, deadline, io_error)) {
        // Drain until EOF; the old daemon may speak an older frame dialect, so
        // nothing here is interpreted.
        while (reader.read_line_until(deadline, io_error)) {
        }
    }
    std::string shutdown_error;
    if (wait_for_old_daemon_shutdown(target, socket_path, uid, deadline, shutdown_error)) {
        error.clear();
        return true;
    }
    error = io_error.empty() ? shutdown_error : io_error + "; " + shutdown_error;
    return false;
}

enum class ControlStopOutcome { Sent, AlreadyGone, Failed };

ControlStopOutcome send_control_stop(const std::string& control_socket_path,
                                     std::string_view control_token, Deadline deadline,
                                     std::string& error) {
    const int fd = net::socket_cloexec(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        error = "cannot create daemon control socket: " + std::string(std::strerror(errno));
        return ControlStopOutcome::Failed;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error =
            "cannot make daemon control socket non-blocking: " + std::string(std::strerror(errno));
        ::close(fd);
        return ControlStopOutcome::Failed;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (control_socket_path.size() >= sizeof(addr.sun_path)) {
        error = "control socket path too long: " + control_socket_path;
        ::close(fd);
        return ControlStopOutcome::Failed;
    }
    std::strncpy(addr.sun_path, control_socket_path.c_str(), sizeof(addr.sun_path) - 1);
    for (;;) {
        int send_flags = 0;
#if defined(MSG_NOSIGNAL)
        send_flags |= MSG_NOSIGNAL;
#endif
#if defined(MSG_DONTWAIT)
        send_flags |= MSG_DONTWAIT;
#endif
        const ssize_t count = ::sendto(fd, control_token.data(), control_token.size(), send_flags,
                                       reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (count == static_cast<ssize_t>(control_token.size())) {
            ::close(fd);
            return ControlStopOutcome::Sent;
        }
        if (count >= 0) {
            error = "short daemon control datagram";
            ::close(fd);
            return ControlStopOutcome::Failed;
        }
        const int send_error = errno;
        if (send_error == EINTR) {
            continue;
        }
        if (send_error == ENOENT || send_error == ECONNREFUSED) {
            ::close(fd);
            return ControlStopOutcome::AlreadyGone;
        }
        if (send_error == EAGAIN || send_error == EWOULDBLOCK || send_error == ENOBUFS) {
            if (!sleep_until_retry(deadline)) {
                error = "timed out sending daemon control datagram";
                ::close(fd);
                return ControlStopOutcome::Failed;
            }
            continue;
        }
        error = "cannot request daemon shutdown through " + control_socket_path + ": " +
                std::strerror(send_error);
        ::close(fd);
        return ControlStopOutcome::Failed;
    }
}

bool restart_out_of_band(RestartTarget& target, const std::string& socket_path, uid_t uid,
                         Deadline deadline, std::string& error) {
    if (!target.owner) {
        error = "verified restart target has no lock owner";
        return false;
    }
    if (send_control_stop(target.control_path, target.owner->identity().control_token, deadline,
                          error) == ControlStopOutcome::Failed) {
        return false;
    }
    return wait_for_old_daemon_shutdown(target, socket_path, uid, deadline, error);
}

bool recover_mismatched_daemon(HandshakeOutcome outcome, const std::string& account,
                               const paths::Environment& env, const std::string& socket_path,
                               Session& session, proto::FrameReader& reader, Deadline deadline,
                               std::string& error) {
    std::optional<RestartTarget> target;
    const auto verification =
        verify_restart_target(account, env, socket_path, session, target, error);
    if (verification == RestartVerification::Transition && target) {
        const std::string transition_error = error;
        if (wait_for_old_daemon_shutdown(*target, socket_path, env.uid, deadline, error)) {
            return true;
        }
        error = "cannot join concurrent daemon restart: " + transition_error + "; " + error;
        return false;
    }
    if (verification == RestartVerification::Invalid || !target) {
        error = "cannot verify mismatched daemon for restart: " + error;
        return false;
    }
    if (outcome == HandshakeOutcome::BinaryMismatch) {
        if (restart_binary_mismatched_daemon(session.fd, reader, *target, socket_path, env.uid,
                                             deadline, error)) {
            return true;
        }
        error = "cannot restart binary-mismatched daemon: " + error;
        return false;
    }
    if (restart_out_of_band(*target, socket_path, env.uid, deadline, error)) {
        return true;
    }
    error = "cannot restart protocol-incompatible daemon: " + error;
    return false;
}

int run_local_doctor(const std::string& account, const std::string& socket_path, bool json_mode,
                     const std::string& reason) {
    const std::string warning = "daemon unreachable (" + reason + "); local diagnostics only";
    if (json_mode) {
        std::fputs((json{{"warning", warning}}.dump() + "\n").c_str(), stderr);
    } else {
        std::fputs(("warning: " + warning + "\n").c_str(), stderr);
    }
    const auto env = paths::real_environment();
    const auto config = paths::config_file(env);
    const json data{{"account", account},
                    {"daemon", {{"running", false}, {"socket", socket_path}}},
                    {"config", {{"path", config}, {"exists", std::filesystem::exists(config)}}}};
    FrameRenderer renderer("doctor", json_mode);
    renderer.on_result(data);
    return renderer.exit_code();
}

void handle_challenge(int fd, const proto::Request& request, ChallengePrompt& prompt,
                      FrameRenderer& renderer, const proto::Challenge& challenge) {
    if (!request.context.tty) {
        renderer.on_error("INTERNAL", "daemon challenged a non-TTY client",
                          nlohmann::json::object(), kGeneric);
        return;
    }
    auto response = prompt.prompt(challenge.challenge);
    if (response.kind == PromptResultKind::Unavailable ||
        response.kind == PromptResultKind::Error) {
        renderer.on_error("INTERNAL", "cannot read challenge response", nlohmann::json::object(),
                          kGeneric);
        return;
    }
    std::string io_error;
    if (!proto::write_frame(fd, proto::Answer{challenge.id, std::move(response.answer)},
                            io_error)) {
        renderer.on_error("INTERNAL", "cannot send challenge response", nlohmann::json::object(),
                          kGeneric);
    }
}

// Sends the request and renders response frames until the terminal one.
int exchange(int fd, proto::FrameReader& reader, const proto::Request& request,
             const RunOptions& options, ChallengePrompt& prompt) {
    if (std::string io_error; !proto::write_frame(fd, request, io_error)) {
        print_error("GENERIC", "cannot send request: " + io_error, json::object());
        return kGeneric;
    }
    FrameRenderer renderer(command_key(request.command), options.json);
    while (!renderer.done()) {
        std::string io_error;
        const auto line = reader.read_line(io_error);
        if (!line) {
            print_error("GENERIC", io_error.empty() ? "daemon closed the connection" : io_error,
                        json::object());
            return kGeneric;
        }
        std::string parse_error;
        const auto frame = proto::parse(*line, parse_error);
        if (!frame) {
            print_error("GENERIC", "malformed frame from daemon: " + parse_error, json::object());
            return kGeneric;
        }
        std::visit(
            [&renderer, &prompt, fd, &request](const auto& f) {
                using T = std::decay_t<decltype(f)>;
                if constexpr (std::is_same_v<T, proto::Item>) {
                    FrameRenderer::on_item(f.data);
                } else if constexpr (std::is_same_v<T, proto::Progress>) {
                    renderer.on_progress(f.data);
                } else if constexpr (std::is_same_v<T, proto::Result>) {
                    renderer.on_result(f.data);
                } else if constexpr (std::is_same_v<T, proto::Error>) {
                    renderer.on_error(f.code, f.message, f.details, f.exit_code);
                } else if constexpr (std::is_same_v<T, proto::Challenge>) {
                    handle_challenge(fd, request, prompt, renderer, f);
                }
            },
            *frame);
    }
    return renderer.exit_code();
}

int run_in_process(const proto::Request& request, const RunOptions& options,
                   ChallengePrompt& prompt) {
    FrameRenderer renderer(command_key(request.command), options.json);
    InProcessSink sink(renderer, prompt, request.context.tty);
    std::string error;
    if (!daemon::run_no_daemon(request, sink, options.account, error,
                               options.in_process_dispatcher)) {
        print_error("GENERIC", error, json::object());
        return kGeneric;
    }
    return renderer.exit_code();
}

} // namespace

int run_command(const proto::Request& request, const RunOptions& options) {
    TerminalPrompt terminal_prompt;
    ChallengePrompt& prompt = options.prompt != nullptr ? *options.prompt : terminal_prompt;
    if (options.no_daemon) {
        return run_in_process(request, options, prompt);
    }

    const auto env = paths::real_environment();
    std::string error;
    const auto socket_path = paths::socket_path(options.account, env, error);
    if (!socket_path) {
        print_error("USAGE", error, json::object());
        return kUsage;
    }
    // The client verifies the socket directory just like the daemon does
    // (DESIGN.md §9): on the /tmp fallback a foreign-uid directory could
    // otherwise plant an imposter socket the client would happily talk to.
    if (!paths::ensure_private_dir(paths::runtime_dir(env), env.uid, error)) {
        print_error("GENERIC", error, json::object());
        return kGeneric;
    }
    const std::string requested_command = command_key(request.command);
    const bool is_doctor = requested_command == "doctor";
    const bool is_daemon_stop = requested_command == "daemon stop";
    const Deadline deadline = std::chrono::steady_clock::now() + options.restart_timeout;

    for (int attempt = 0; attempt < 2; ++attempt) {
        Session session;
        {
            auto connected =
                connect_with_spawn(options.account, *socket_path, env.uid, options.auto_spawn,
                                   options.daemon_executable, deadline, error);
            if (!connected) {
                if (is_doctor) {
                    return run_local_doctor(options.account, *socket_path, options.json, error);
                }
                print_error("GENERIC", error, json::object());
                return kGeneric;
            }
            session.fd = connected->fd;
            session.socket_identity = connected->identity;
        }
        proto::FrameReader reader(session.fd);
        const auto outcome = handshake(session.fd, reader, deadline, error);
        switch (outcome) {
        case HandshakeOutcome::Failed: {
            const std::string handshake_error = error;
            if (attempt == 0 &&
                wait_for_socket_change(*socket_path, env.uid, session.socket_identity, deadline,
                                       "timed out waiting for failed daemon socket to change",
                                       error)) {
                continue;
            }
            error = handshake_error;
            print_error("GENERIC", "handshake failed: " + error, json::object());
            return kGeneric;
        }
        case HandshakeOutcome::Ok:
            return exchange(session.fd, reader, request, options, prompt);
        case HandshakeOutcome::BinaryMismatch:
        case HandshakeOutcome::ProtocolMismatch:
        case HandshakeOutcome::IncompatibleHello:
            if (attempt > 0) {
                print_error("GENERIC", "daemon handshake mismatch persists after restart",
                            json::object());
                return kGeneric;
            }
            break;
        }

        if (!recover_mismatched_daemon(outcome, options.account, env, *socket_path, session, reader,
                                       deadline, error)) {
            print_error("GENERIC", error, json::object());
            return kGeneric;
        }
        if (is_daemon_stop) {
            FrameRenderer renderer(requested_command, options.json);
            renderer.on_result({{"stopping", true}});
            return renderer.exit_code();
        }
    }
    print_error("GENERIC", "daemon restart loop; giving up", json::object());
    return kGeneric;
}

} // namespace tgcli::cli
