#include "cli/client.hpp"

#include "cli/control_stop.hpp"
#include "cli/prompt.hpp"
#include "cli/render.hpp"
#include "cli/routing.hpp"
#include "cli/surface_safety.hpp"
#include "common/config.hpp"
#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "daemon/account_commands.hpp"
#include "daemon/daemon_run.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/request_session.hpp"
#include "proto/destructive_plan.hpp"
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
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
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

bool is_daemon_control_command(const std::vector<std::string>& command) {
    const auto key = command_key(command);
    return key == "daemon status" || key == "daemon stop" || key == "daemon restart";
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

std::optional<ConnectedSocket>
connect_with_spawn(const std::string& account, const std::string& socket_path, uid_t uid,
                   bool auto_spawn, const std::string& daemon_executable, Deadline deadline,
                   ConnectStatus& final_status, std::string& error) {
    ConnectedSocket connected;
    const auto initial = connect_socket(socket_path, uid, deadline, connected, error);
    final_status = initial;
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
        final_status = ConnectStatus::Failed;
        return std::nullopt;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = connect_socket(socket_path, uid, deadline, connected, error);
        final_status = status;
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

struct HandshakeResult {
    HandshakeOutcome outcome = HandshakeOutcome::Failed;
    std::optional<proto::Hello> hello;
    bool clean_eof = false;
};

HandshakeResult handshake(int fd, proto::FrameReader& reader, Deadline deadline,
                          std::string& error) {
    if (std::string io_error; !proto::write_frame_until(
            fd, proto::Hello{kVersion, proto::kProtocolVersion}, deadline, io_error)) {
        error = io_error;
        return {HandshakeOutcome::Failed, std::nullopt, false};
    }
    std::string io_error;
    auto line = reader.read_line_until(deadline, io_error);
    if (!line) {
        error = io_error.empty() ? "daemon closed the connection during handshake" : io_error;
        return {HandshakeOutcome::Failed, std::nullopt, io_error.empty()};
    }
    std::string parse_error;
    const auto frame = proto::parse(std::move(*line), parse_error);
    if (!frame) {
        error = "malformed handshake frame: " + parse_error;
        return {HandshakeOutcome::IncompatibleHello, std::nullopt, false};
    }
    const auto* hello = std::get_if<proto::Hello>(&*frame);
    if (hello == nullptr) {
        error = "daemon did not open with a hello frame";
        return {HandshakeOutcome::IncompatibleHello, std::nullopt, false};
    }
    proto::Hello observed = *hello;
    if (hello->protocol_version != proto::kProtocolVersion) {
        return {HandshakeOutcome::ProtocolMismatch, std::move(observed), false};
    }
    if (hello->binary_version != kVersion) {
        return {HandshakeOutcome::BinaryMismatch, std::move(observed), false};
    }
    return {HandshakeOutcome::Ok, std::move(observed), false};
}

struct RestartTarget {
    std::optional<daemon_lock::OwnerWatch> owner;
    paths::SocketIdentity main_identity;
    std::string control_path;
    std::optional<paths::SocketIdentity> control_identity;
};

enum class RestartVerification { Verified, Transition, Invalid };

RestartVerification verify_control_target(const std::string& account, const paths::Environment& env,
                                          const std::string& socket_path, const Session& session,
                                          std::optional<RestartTarget>& target,
                                          std::string& error) {
    target.reset();
    const std::string state_dir = paths::account_state_dir(account, env);
    if (!paths::validate_private_dir(state_dir, env.uid, error)) {
        return RestartVerification::Invalid;
    }
    if (!paths::validate_private_dir(paths::runtime_dir(env), env.uid, error)) {
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

enum class SurfaceState {
    Absent,
    Starting,
    Running,
    Transition,
    Incomplete,
    Invalid,
    HandshakeFailed
};

struct SurfaceProbe {
    SurfaceState state = SurfaceState::Invalid;
    HandshakeResult handshake;
    std::unique_ptr<RestartTarget> target;
    daemon_lock::Identity owner_identity;
    std::string error;
    std::string failure_reason = "surface_invalid";
};

bool path_present(const std::string& candidate, bool& present, std::string& error) {
    struct stat metadata {};
    if (::lstat(candidate.c_str(), &metadata) == 0) {
        present = true;
        return true;
    }
    if (errno == ENOENT) {
        present = false;
        return true;
    }
    error = "cannot inspect " + candidate + ": " + std::strerror(errno);
    return false;
}

SurfaceState inspect_surface_files(const std::string& account, const paths::Environment& env,
                                   const std::string& socket_path,
                                   std::optional<RestartTarget>& frozen_target,
                                   std::string& failure_reason, std::string& error) {
    frozen_target.reset();
    const std::string state_dir = paths::account_state_dir(account, env);
    const std::string lock_path = state_dir + "/daemon.lock";
    bool lock_present = false;
    if (!path_present(lock_path, lock_present, error)) {
        return SurfaceState::Invalid;
    }
    if (lock_present && !paths::validate_private_dir(state_dir, env.uid, error)) {
        return SurfaceState::Invalid;
    }

    std::optional<daemon_lock::OwnerWatch> owner;
    const auto owner_status = daemon_lock::inspect_owner(lock_path, env.uid, owner, error);
    if (owner_status == daemon_lock::OwnerStatus::Invalid) {
        return SurfaceState::Invalid;
    }

    const auto control_path = paths::control_socket_path(account, env, error);
    if (!control_path) {
        return SurfaceState::Invalid;
    }
    std::optional<paths::SocketIdentity> main_identity;
    if (!paths::find_socket_endpoint(socket_path, env.uid, main_identity, error)) {
        return SurfaceState::Invalid;
    }
    std::optional<paths::SocketIdentity> control_identity;
    if (!paths::find_socket_endpoint(*control_path, env.uid, control_identity, error)) {
        return SurfaceState::Invalid;
    }
    if ((main_identity || control_identity) &&
        !paths::validate_private_dir(paths::runtime_dir(env), env.uid, error)) {
        return SurfaceState::Invalid;
    }

    if (owner_status == daemon_lock::OwnerStatus::Released && !main_identity && !control_identity) {
        return SurfaceState::Absent;
    }
    if (owner_status == daemon_lock::OwnerStatus::Transition) {
        if (!main_identity && !control_identity) {
            if (detail::inspect_runtime_directory(paths::runtime_dir(env), env.uid, error) ==
                detail::RuntimeDirectoryState::Invalid) {
                return SurfaceState::Invalid;
            }
            error = "daemon startup is publishing its identity";
            return SurfaceState::Starting;
        }
        error = "daemon ownership changed while inspecting the control surface";
        failure_reason = "identity_changed";
        return SurfaceState::Incomplete;
    }
    if (owner_status == daemon_lock::OwnerStatus::Held && owner && !main_identity &&
        !control_identity) {
        if (detail::inspect_runtime_directory(paths::runtime_dir(env), env.uid, error) ==
            detail::RuntimeDirectoryState::Invalid) {
            return SurfaceState::Invalid;
        }
        error = "daemon startup has not published its endpoints";
        return SurfaceState::Starting;
    }
    if (owner_status != daemon_lock::OwnerStatus::Held || !owner || !main_identity ||
        !control_identity) {
        error = "daemon control surface is incomplete";
        return SurfaceState::Incomplete;
    }
    frozen_target =
        RestartTarget{std::move(owner), *main_identity, *control_path, control_identity};
    return SurfaceState::Running;
}

enum class FrozenTargetObservation { Unchanged, Changed, Invalid };

FrozenTargetObservation observe_frozen_target(RestartTarget& target, const std::string& socket_path,
                                              uid_t uid, std::string& error) {
    if (!target.owner || !target.control_identity) {
        error = "frozen daemon target is incomplete";
        return FrozenTargetObservation::Invalid;
    }
    bool owner_released = false;
    bool main_changed = false;
    bool control_changed = false;
    if (!target.owner->owner_released(owner_released, error) ||
        !paths::socket_endpoint_changed(socket_path, uid, target.main_identity, main_changed,
                                        error) ||
        !paths::socket_endpoint_changed(target.control_path, uid, *target.control_identity,
                                        control_changed, error)) {
        return FrozenTargetObservation::Invalid;
    }
    return owner_released || main_changed || control_changed ? FrozenTargetObservation::Changed
                                                             : FrozenTargetObservation::Unchanged;
}

bool open_frozen_session(const paths::Environment& env, const std::string& socket_path,
                         Deadline deadline, RestartTarget& frozen_target, SurfaceProbe& probe,
                         Session& session, std::unique_ptr<proto::FrameReader>& reader) {
    std::string error;
    ConnectedSocket connected;
    const auto connect_status = connect_socket(socket_path, env.uid, deadline, connected, error);
    if (connect_status != ConnectStatus::Connected) {
        probe.state = SurfaceState::Invalid;
        if (connect_status == ConnectStatus::Unavailable) {
            const auto observation =
                observe_frozen_target(frozen_target, socket_path, env.uid, probe.error);
            if (observation == FrozenTargetObservation::Changed) {
                probe.state = SurfaceState::Transition;
                probe.failure_reason = "identity_changed";
            } else if (observation == FrozenTargetObservation::Unchanged) {
                probe.state = SurfaceState::Incomplete;
            }
        }
        if (probe.error.empty()) {
            probe.error = std::move(error);
        }
        return false;
    }
    if (connected.identity != frozen_target.main_identity) {
        ::close(connected.fd);
        probe.state = SurfaceState::Transition;
        probe.error = "daemon main endpoint changed while inspecting the control surface";
        probe.failure_reason = "identity_changed";
        return false;
    }
    session.fd = connected.fd;
    session.socket_identity = connected.identity;
    reader = std::make_unique<proto::FrameReader>(session.fd);
    probe.handshake = handshake(session.fd, *reader, deadline, error);
    if (probe.handshake.outcome != HandshakeOutcome::Failed) {
        return true;
    }
    probe.state =
        probe.handshake.clean_eof ? SurfaceState::Transition : SurfaceState::HandshakeFailed;
    probe.error = std::move(error);
    probe.failure_reason = probe.handshake.clean_eof ? "identity_changed" : "handshake_failed";
    return false;
}

void verify_frozen_session(const std::string& account, const paths::Environment& env,
                           const std::string& socket_path, const Session& session,
                           const paths::SocketIdentity& frozen_control_identity,
                           const daemon_lock::Identity& initial_owner, SurfaceProbe& probe) {
    std::string error;
    std::optional<RestartTarget> verified_target;
    const auto verification =
        verify_control_target(account, env, socket_path, session, verified_target, error);
    if (verification != RestartVerification::Verified || !verified_target) {
        probe.state = verification == RestartVerification::Transition ? SurfaceState::Transition
                                                                      : SurfaceState::Invalid;
        probe.error = std::move(error);
        if (verification == RestartVerification::Transition) {
            probe.failure_reason = "identity_changed";
        }
        return;
    }
    if (!verified_target->control_identity ||
        *verified_target->control_identity != frozen_control_identity) {
        probe.state = SurfaceState::Transition;
        probe.error = "daemon control endpoint changed while inspecting the control surface";
        probe.failure_reason = "identity_changed";
        return;
    }
    if (!verified_target->owner || verified_target->owner->identity() != initial_owner) {
        probe.state = SurfaceState::Transition;
        probe.error = "daemon owner identity changed while inspecting the control surface";
        probe.failure_reason = "identity_changed";
        return;
    }
    probe.state = SurfaceState::Running;
    probe.target = std::make_unique<RestartTarget>(std::move(*verified_target));
}

SurfaceProbe probe_daemon_surface(const std::string& account, const paths::Environment& env,
                                  const std::string& socket_path, Deadline deadline,
                                  Session& session, std::unique_ptr<proto::FrameReader>& reader) {
    SurfaceProbe probe;
    std::optional<RestartTarget> frozen_target;
    probe.state = inspect_surface_files(account, env, socket_path, frozen_target,
                                        probe.failure_reason, probe.error);
    if (probe.state != SurfaceState::Running) {
        return probe;
    }
    if (!frozen_target) {
        probe.state = SurfaceState::Invalid;
        probe.error = "frozen daemon control surface is incomplete";
        return probe;
    }
    if (!frozen_target->owner || !frozen_target->control_identity) {
        probe.state = SurfaceState::Invalid;
        probe.error = "frozen daemon control surface is incomplete";
        return probe;
    }
    const daemon_lock::Identity initial_owner = frozen_target->owner->identity();
    const paths::SocketIdentity frozen_control_identity = *frozen_target->control_identity;
    probe.owner_identity = initial_owner;
    probe.target = std::make_unique<RestartTarget>(std::move(*frozen_target));
    RestartTarget& frozen = *probe.target;
    if (open_frozen_session(env, socket_path, deadline, frozen, probe, session, reader)) {
        verify_frozen_session(account, env, socket_path, session, frozen_control_identity,
                              initial_owner, probe);
    }
    return probe;
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

bool request_binary_mismatched_stop(int fd, proto::FrameReader& reader, const std::string& account,
                                    RestartTarget& target, const std::string& socket_path,
                                    uid_t uid, Deadline deadline, std::string& error) {
    proto::Request stop_request(account);
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

enum class CompatibleStopResponse { Confirmed, ConcurrentShutdown, Lost, Failed };

CompatibleStopResponse read_compatible_stop_response(proto::FrameReader& reader,
                                                     std::uint64_t request_id, Deadline deadline,
                                                     std::string& error) {
    for (;;) {
        std::string io_error;
        auto line = reader.read_line_until(deadline, io_error);
        if (!line) {
            error = io_error.empty() ? "daemon closed before confirming shutdown" : io_error;
            return CompatibleStopResponse::Lost;
        }
        std::string parse_error;
        const auto frame = proto::parse(std::move(*line), parse_error);
        if (!frame) {
            error = "malformed daemon stop response: " + parse_error;
            return CompatibleStopResponse::Failed;
        }
        if (const auto* result = std::get_if<proto::Result>(&*frame)) {
            if (result->id != request_id || result->data != json{{"stopping", true}}) {
                error = "daemon returned an unexpected stop result";
                return CompatibleStopResponse::Failed;
            }
            return CompatibleStopResponse::Confirmed;
        }
        if (const auto* terminal_error = std::get_if<proto::Error>(&*frame)) {
            if (terminal_error->id == request_id && terminal_error->code == "DAEMON_SHUTDOWN") {
                return CompatibleStopResponse::ConcurrentShutdown;
            }
            error = "daemon rejected the stop request: " + terminal_error->code;
            return CompatibleStopResponse::Failed;
        }
    }
}

bool join_shutdown_after_exchange_failure(RestartTarget& target, const std::string& socket_path,
                                          uid_t uid, Deadline deadline, std::string exchange_error,
                                          std::string& error) {
    std::string shutdown_error;
    if (wait_for_old_daemon_shutdown(target, socket_path, uid, deadline, shutdown_error)) {
        error.clear();
        return true;
    }
    error = std::move(exchange_error) + "; " + shutdown_error;
    return false;
}

bool request_compatible_stop(int fd, proto::FrameReader& reader, const std::string& account,
                             RestartTarget& target, const std::string& socket_path, uid_t uid,
                             Deadline deadline, std::string& error) {
    proto::Request stop_request(account);
    stop_request.id = 1;
    stop_request.command = {"daemon", "stop"};
    stop_request.context.cwd = "/";
    std::string io_error;
    if (!proto::write_frame_until(fd, stop_request, deadline, io_error)) {
        return join_shutdown_after_exchange_failure(target, socket_path, uid, deadline,
                                                    "cannot send daemon stop request: " + io_error,
                                                    error);
    }
    const auto response = read_compatible_stop_response(reader, stop_request.id, deadline, error);
    if (response == CompatibleStopResponse::Failed) {
        return false;
    }
    if (response == CompatibleStopResponse::Lost) {
        std::string exchange_error = std::move(error);
        return join_shutdown_after_exchange_failure(target, socket_path, uid, deadline,
                                                    std::move(exchange_error), error);
    }
    return wait_for_old_daemon_shutdown(target, socket_path, uid, deadline, error);
}

bool request_out_of_band_stop(RestartTarget& target, const std::string& socket_path, uid_t uid,
                              Deadline deadline, std::string& error) {
    if (!target.owner || !target.control_identity) {
        error = "verified restart target has no lock owner or control endpoint";
        return false;
    }
    if (detail::send_verified_control_stop(target.control_path, *target.control_identity, uid,
                                           target.owner->identity().control_token, deadline,
                                           error) == detail::ControlStopOutcome::Failed) {
        return false;
    }
    return wait_for_old_daemon_shutdown(target, socket_path, uid, deadline, error);
}

bool stop_verified_daemon(HandshakeOutcome outcome, Session& session, proto::FrameReader& reader,
                          const std::string& account, RestartTarget& target,
                          const std::string& socket_path, uid_t uid, Deadline deadline,
                          std::string& error) {
    switch (outcome) {
    case HandshakeOutcome::Ok:
        return request_compatible_stop(session.fd, reader, account, target, socket_path, uid,
                                       deadline, error);
    case HandshakeOutcome::BinaryMismatch:
        return request_binary_mismatched_stop(session.fd, reader, account, target, socket_path, uid,
                                              deadline, error);
    case HandshakeOutcome::ProtocolMismatch:
    case HandshakeOutcome::IncompatibleHello:
        return request_out_of_band_stop(target, socket_path, uid, deadline, error);
    case HandshakeOutcome::Failed:
        error = "daemon handshake did not identify a controllable target";
        return false;
    }
    error = "daemon handshake outcome is invalid";
    return false;
}

bool recover_mismatched_daemon(HandshakeOutcome outcome, const std::string& account,
                               const paths::Environment& env, const std::string& socket_path,
                               Session& session, proto::FrameReader& reader, Deadline deadline,
                               std::string& error) {
    std::optional<RestartTarget> target;
    const auto verification =
        verify_control_target(account, env, socket_path, session, target, error);
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
    if (stop_verified_daemon(outcome, session, reader, account, *target, socket_path, env.uid,
                             deadline, error)) {
        return true;
    }
    error = "cannot stop mismatched daemon: " + error;
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
        auto line = reader.read_line(io_error);
        if (!line) {
            print_error("GENERIC", io_error.empty() ? "daemon closed the connection" : io_error,
                        json::object());
            return kGeneric;
        }
        std::string parse_error;
        const auto frame = proto::parse(std::move(*line), parse_error);
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

enum class AccountShowPreflightChild { RunningDaemon, OfflineObserver };

int run_routed_command(const proto::Request& request, const RunOptions& options,
                       ChallengePrompt& prompt);

bool wait_for_preflight_child(pid_t child, Deadline deadline) {
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == kOk;
        }
        if (waited < 0 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ::kill(child, SIGTERM);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}

void run_account_show_preflight_child(AccountShowPreflightChild operation,
                                      const proto::Request& request, const RunOptions& options,
                                      Deadline deadline) {
    const pid_t child = ::fork();
    if (child < 0) {
        return;
    }
    if (child == 0) {
        const int null_descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_descriptor >= 0) {
            ::dup2(null_descriptor, STDOUT_FILENO);
            ::dup2(null_descriptor, STDERR_FILENO);
            ::close(null_descriptor);
        }
        int exit_code = kGeneric;
        if (operation == AccountShowPreflightChild::OfflineObserver) {
            exit_code =
                daemon::reconcile_logout_audit_offline(request.account, deadline) ? kOk : kGeneric;
        } else {
            proto::Request doctor(request.account);
            doctor.id = request.id;
            doctor.command = {"doctor"};
            doctor.context.timeout_seconds =
                std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
            doctor.context.cwd = "/";
            RunOptions child_options = options;
            child_options.account = request.account;
            child_options.json = true;
            child_options.no_daemon = false;
            child_options.auto_spawn = false;
            child_options.restart_on_mismatch = false;
            child_options.restart_timeout = std::max(
                std::chrono::milliseconds(1), std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  deadline - std::chrono::steady_clock::now()));
            TerminalPrompt child_prompt;
            exit_code = run_routed_command(doctor, child_options, child_prompt);
        }
        ::_exit(exit_code);
    }
    static_cast<void>(wait_for_preflight_child(child, deadline));
}

bool logout_reconciliation_required(const daemon::LogoutAuditInspection& inspection) {
    return inspection.status == daemon::LogoutAuditInspectionStatus::Incomplete &&
           inspection.incomplete.has_value();
}

void reconcile_account_show_logout(const proto::Request& request, const RunOptions& options,
                                   Deadline deadline) {
    if (!request.args.is_object() || !request.args.contains("account") ||
        !request.args["account"].is_string()) {
        return;
    }
    const auto account = request.args["account"].get<std::string>();
    if (!paths::valid_account_name(account)) {
        return;
    }
    const auto environment = paths::real_environment();
    const daemon::LogoutAuditLog audit(paths::account_state_dir(account, environment), account,
                                       environment.uid);
    auto inspection = audit.inspect();
    if (!logout_reconciliation_required(inspection)) {
        return;
    }

    std::string socket_error;
    const auto socket_path = paths::socket_path(account, environment, socket_error);
    std::optional<paths::SocketIdentity> socket_identity;
    if (socket_path &&
        paths::find_socket_endpoint(*socket_path, environment.uid, socket_identity, socket_error) &&
        socket_identity) {
        proto::Request routed = request;
        routed.account = account;
        run_account_show_preflight_child(AccountShowPreflightChild::RunningDaemon, routed, options,
                                         deadline);
        inspection = audit.inspect();
        if (!logout_reconciliation_required(inspection)) {
            return;
        }
    }
    proto::Request routed = request;
    routed.account = account;
    run_account_show_preflight_child(AccountShowPreflightChild::OfflineObserver, routed, options,
                                     deadline);
}

std::optional<json> account_show_incomplete_details(const proto::Request& request) {
    if (!request.args.is_object() || !request.args.contains("account") ||
        !request.args["account"].is_string()) {
        return std::nullopt;
    }
    const auto account = request.args["account"].get<std::string>();
    if (!paths::valid_account_name(account)) {
        return std::nullopt;
    }
    const auto environment = paths::real_environment();
    const daemon::LogoutAuditLog audit(paths::account_state_dir(account, environment), account,
                                       environment.uid);
    const auto inspection = audit.inspect();
    if (inspection.status != daemon::LogoutAuditInspectionStatus::Incomplete ||
        !inspection.incomplete) {
        return std::nullopt;
    }
    json completed = json::array();
    for (const auto stage : inspection.incomplete->completed_stages) {
        completed.push_back(daemon::audit_stage_name(stage));
    }
    std::string error;
    const auto mutation = daemon::derive_mutation_state(
        daemon::DestructiveCommand::Logout, inspection.incomplete->completed_stages, error);
    if (!mutation) {
        return std::nullopt;
    }
    return json{{"account", account},
                {"path", audit.path()},
                {"mutation_state", daemon::mutation_state_name(*mutation)},
                {"completed_stages", std::move(completed)}};
}

int run_config_global(const proto::Request& request, const RunOptions& options,
                      ChallengePrompt& prompt) {
    const auto env = paths::real_environment();
    const config::Store store(paths::config_file(env), env.uid);
    const daemon::ConfigGlobalContext context{store, env};
    daemon::Dispatcher dispatcher;
    daemon::register_account_commands(dispatcher, context);

    FrameRenderer renderer(command_key(request.command), options.json);
    InProcessSink sink(renderer, prompt, request.context.tty);
    try {
        daemon::RequestSession session(request, sink);
        if (request.command == std::vector<std::string>{"account", "show"}) {
            reconcile_account_show_logout(request, options, session.deadline());
            if (std::chrono::steady_clock::now() >= session.deadline()) {
                if (auto details = account_show_incomplete_details(request)) {
                    session.error("AUDIT_INCOMPLETE", "logout audit reconciliation is incomplete",
                                  std::move(*details), kGeneric);
                    return renderer.exit_code();
                }
            }
        }
        dispatcher.dispatch(session);
    } catch (const std::invalid_argument&) {
        print_error("USAGE", "invalid request timeout",
                    {{"argument", "--timeout"}, {"reason", "invalid_argument"}});
        return kUsage;
    }
    return renderer.exit_code();
}

int run_logout_dry_run(const proto::Request& request, const RunOptions& options) {
    const auto env = paths::real_environment();
    const config::Store store(paths::config_file(env), env.uid);
    auto loaded = store.load();
    if (!loaded || !loaded.snapshot) {
        print_error(
            "CONFIG_INVALID", "cannot validate logout config",
            {{"path", store.path()},
             {"reason", loaded.error ? config::reason_name(loaded.error->reason) : "io_error"}});
        return kGeneric;
    }
    if (!loaded.snapshot->accounts.contains(request.account)) {
        print_error("ACCOUNT_NOT_FOUND", "account is not configured",
                    {{"account", request.account}});
        return kNotFound;
    }
    if (!request.args.empty()) {
        print_error("USAGE", "logout takes no command arguments",
                    {{"argument", nullptr}, {"reason", "invalid_argument"}});
        return kUsage;
    }
    std::string error;
    const auto plan = proto::make_logout_plan(request.account, error);
    if (!plan) {
        print_error("INTERNAL", "cannot build logout plan",
                    {{"operation", "logout"}, {"reason", "internal_error"}});
        return kGeneric;
    }
    FrameRenderer renderer("logout", options.json);
    renderer.on_result({{"dry_run", true}, {"plan", proto::serialize(*plan)}});
    return renderer.exit_code();
}

std::optional<Deadline> daemon_control_deadline(const proto::Request& request,
                                                const RunOptions& options) {
    const auto now = std::chrono::steady_clock::now();
    if (request.context.timeout_seconds) {
        return proto::request_deadline(request.context.timeout_seconds, now);
    }
    if (options.restart_timeout <= std::chrono::milliseconds::zero()) {
        return std::nullopt;
    }
    return now + options.restart_timeout;
}

int daemon_control_failure(std::string_view operation, std::string_view reason,
                           const RunOptions& options, const std::string& message) {
    print_error("DAEMON_CONTROL_FAILED", message,
                {{"account", options.account}, {"operation", operation}, {"reason", reason}});
    return kGeneric;
}

int daemon_not_running(const RunOptions& options, const std::string& socket_path) {
    print_error("DAEMON_NOT_RUNNING", "daemon is not running",
                {{"account", options.account}, {"socket", socket_path}});
    return kNotFound;
}

struct ReplacementFacts {
    daemon_lock::Identity owner;
    proto::Hello hello;
};

bool spawn_replacement_once(const RunOptions& options, Deadline deadline, bool& spawn_attempted,
                            std::string& error) {
    if (spawn_attempted) {
        return true;
    }
    if (!spawn_daemon(options.account, options.daemon_executable, deadline, error)) {
        return false;
    }
    spawn_attempted = true;
    return true;
}

bool accept_replacement(const SurfaceProbe& probe,
                        const std::optional<daemon_lock::Identity>& old_owner,
                        ReplacementFacts& replacement, std::string& error) {
    if (probe.handshake.outcome != HandshakeOutcome::Ok || !probe.handshake.hello ||
        !probe.target) {
        error = "replacement daemon did not complete the current Hello handshake";
        return false;
    }
    if (!probe.target->owner) {
        error = "replacement daemon did not complete the current Hello handshake";
        return false;
    }
    const auto& new_owner = probe.owner_identity;
    if (old_owner && old_owner->pid == new_owner.pid &&
        old_owner->process_start == new_owner.process_start) {
        error = "replacement daemon retained the old owner identity";
        return false;
    }
    replacement = {new_owner, *probe.handshake.hello};
    return true;
}

enum class ReplacementStep { Ready, Retry, Failed };

ReplacementStep advance_replacement(SurfaceProbe& probe, const RunOptions& options,
                                    const paths::Environment& env, const std::string& socket_path,
                                    const std::optional<daemon_lock::Identity>& old_owner,
                                    Deadline deadline, bool& spawn_attempted,
                                    ReplacementFacts& replacement, std::string& error) {
    if (probe.state == SurfaceState::Running) {
        return accept_replacement(probe, old_owner, replacement, error) ? ReplacementStep::Ready
                                                                        : ReplacementStep::Failed;
    }
    if (probe.state == SurfaceState::Transition) {
        if (!probe.target) {
            error = "replacement transition has no frozen target";
            return ReplacementStep::Failed;
        }
        if (!wait_for_old_daemon_shutdown(*probe.target, socket_path, env.uid, deadline, error) ||
            !spawn_replacement_once(options, deadline, spawn_attempted, error)) {
            return ReplacementStep::Failed;
        }
        return ReplacementStep::Retry;
    }
    if (probe.state == SurfaceState::Absent && !spawn_attempted) {
        return spawn_replacement_once(options, deadline, spawn_attempted, error)
                   ? ReplacementStep::Retry
                   : ReplacementStep::Failed;
    }
    if (probe.state == SurfaceState::Invalid || probe.state == SurfaceState::HandshakeFailed ||
        probe.state == SurfaceState::Incomplete) {
        error = probe.error;
        return ReplacementStep::Failed;
    }
    return ReplacementStep::Retry;
}

bool wait_for_replacement(const RunOptions& options, const paths::Environment& env,
                          const std::string& socket_path,
                          const std::optional<daemon_lock::Identity>& old_owner, Deadline deadline,
                          bool spawn_immediately, ReplacementFacts& replacement,
                          std::string& error) {
    bool spawn_attempted = false;
    if (spawn_immediately && !spawn_replacement_once(options, deadline, spawn_attempted, error)) {
        return false;
    }
    for (;;) {
        Session session;
        std::unique_ptr<proto::FrameReader> reader;
        auto probe =
            probe_daemon_surface(options.account, env, socket_path, deadline, session, reader);
        const auto step = advance_replacement(probe, options, env, socket_path, old_owner, deadline,
                                              spawn_attempted, replacement, error);
        if (step == ReplacementStep::Ready) {
            return true;
        }
        if (step == ReplacementStep::Failed) {
            return false;
        }
        if (!sleep_until_retry(deadline)) {
            error = "timed out waiting for replacement daemon readiness";
            return false;
        }
    }
}

int surface_probe_failure(std::string_view operation, const RunOptions& options,
                          const SurfaceProbe& probe) {
    return daemon_control_failure(operation, probe.failure_reason, options,
                                  "cannot " + std::string(operation) + " daemon: " + probe.error);
}

int run_daemon_status(const RunOptions& options, const paths::Environment& env,
                      const std::string& socket_path, Deadline deadline) {
    Session session;
    std::unique_ptr<proto::FrameReader> reader;
    auto probe = probe_daemon_surface(options.account, env, socket_path, deadline, session, reader);
    if (probe.state == SurfaceState::Absent) {
        FrameRenderer renderer("daemon status", options.json);
        renderer.on_result(
            {{"account", options.account}, {"running", false}, {"socket", socket_path}});
        return renderer.exit_code();
    }
    if (probe.state != SurfaceState::Running) {
        return surface_probe_failure("status", options, probe);
    }
    if (!probe.handshake.hello || !probe.target) {
        return daemon_control_failure("status", "handshake_failed", options,
                                      "cannot status daemon: daemon Hello is not parseable");
    }
    if (!probe.target->owner) {
        return daemon_control_failure("status", "handshake_failed", options,
                                      "cannot status daemon: daemon Hello is not parseable");
    }
    const auto& hello = *probe.handshake.hello;
    const auto& owner = probe.owner_identity;
    FrameRenderer renderer("daemon status", options.json);
    renderer.on_result({{"account", options.account},
                        {"running", true},
                        {"pid", static_cast<std::int64_t>(owner.pid)},
                        {"version", hello.binary_version},
                        {"protocol", hello.protocol_version},
                        {"socket", socket_path}});
    return renderer.exit_code();
}

int run_daemon_stop(const RunOptions& options, const paths::Environment& env,
                    const std::string& socket_path, Deadline deadline) {
    Session session;
    std::unique_ptr<proto::FrameReader> reader;
    auto probe = probe_daemon_surface(options.account, env, socket_path, deadline, session, reader);
    if (probe.state == SurfaceState::Absent) {
        return daemon_not_running(options, socket_path);
    }
    if (probe.state != SurfaceState::Running) {
        return surface_probe_failure("stop", options, probe);
    }
    if (!probe.target || !probe.target->owner || !reader) {
        return daemon_control_failure("stop", "surface_invalid", options,
                                      "cannot stop daemon: verified target is incomplete");
    }
    std::string stop_error;
    if (!stop_verified_daemon(probe.handshake.outcome, session, *reader, options.account,
                              *probe.target, socket_path, env.uid, deadline, stop_error)) {
        return daemon_control_failure("stop", "shutdown_failed", options,
                                      "cannot stop daemon: " + stop_error);
    }
    FrameRenderer renderer("daemon stop", options.json);
    renderer.on_result({{"stopping", true}});
    return renderer.exit_code();
}

int render_daemon_restart(const RunOptions& options, const std::string& socket_path,
                          const ReplacementFacts& replacement) {
    FrameRenderer renderer("daemon restart", options.json);
    renderer.on_result({{"account", options.account},
                        {"restarted", true},
                        {"pid", static_cast<std::int64_t>(replacement.owner.pid)},
                        {"version", replacement.hello.binary_version},
                        {"protocol", replacement.hello.protocol_version},
                        {"socket", socket_path}});
    return renderer.exit_code();
}

int run_daemon_restart(const RunOptions& options, const paths::Environment& env,
                       const std::string& socket_path, Deadline deadline) {
    Session session;
    std::unique_ptr<proto::FrameReader> reader;
    auto probe = probe_daemon_surface(options.account, env, socket_path, deadline, session, reader);
    std::optional<daemon_lock::Identity> old_owner;
    bool spawn_replacement = true;
    if (probe.state == SurfaceState::Starting) {
        spawn_replacement = false;
    } else if (probe.state == SurfaceState::Transition) {
        if (!probe.target) {
            return surface_probe_failure("restart", options, probe);
        }
        if (!probe.target->owner) {
            return surface_probe_failure("restart", options, probe);
        }
        old_owner = probe.owner_identity;
        std::string shutdown_error;
        if (!wait_for_old_daemon_shutdown(*probe.target, socket_path, env.uid, deadline,
                                          shutdown_error)) {
            return daemon_control_failure("restart", "shutdown_failed", options,
                                          "cannot restart daemon: " + shutdown_error);
        }
    } else if (probe.state == SurfaceState::Running) {
        if (!probe.target || !reader) {
            return daemon_control_failure("restart", "surface_invalid", options,
                                          "cannot restart daemon: verified target is incomplete");
        }
        if (!probe.target->owner) {
            return daemon_control_failure("restart", "surface_invalid", options,
                                          "cannot restart daemon: verified target is incomplete");
        }
        old_owner = probe.owner_identity;
        std::string stop_error;
        if (!stop_verified_daemon(probe.handshake.outcome, session, *reader, options.account,
                                  *probe.target, socket_path, env.uid, deadline, stop_error)) {
            return daemon_control_failure("restart", "shutdown_failed", options,
                                          "cannot restart daemon: " + stop_error);
        }
    } else if (probe.state != SurfaceState::Absent) {
        return surface_probe_failure("restart", options, probe);
    }
    ReplacementFacts replacement;
    std::string replacement_error;
    if (!wait_for_replacement(options, env, socket_path, old_owner, deadline, spawn_replacement,
                              replacement, replacement_error)) {
        return daemon_control_failure("restart", "replacement_failed", options,
                                      "cannot restart daemon: " + replacement_error);
    }
    return render_daemon_restart(options, socket_path, replacement);
}

int run_daemon_control(const proto::Request& request, const RunOptions& options) {
    const auto deadline = daemon_control_deadline(request, options);
    if (!deadline) {
        print_error("USAGE", "invalid request timeout",
                    {{"argument", "--timeout"}, {"reason", "invalid_argument"}});
        return kUsage;
    }
    const auto env = paths::real_environment();
    std::string path_error;
    const auto socket_path = paths::socket_path(options.account, env, path_error);
    if (!socket_path) {
        print_error("USAGE", path_error, json::object());
        return kUsage;
    }
    const std::string& operation = request.command.back();
    if (operation == "status") {
        return run_daemon_status(options, env, *socket_path, *deadline);
    }
    if (operation == "stop") {
        return run_daemon_stop(options, env, *socket_path, *deadline);
    }
    return run_daemon_restart(options, env, *socket_path, *deadline);
}

int report_connect_failure(const RunOptions& options, const std::string& socket_path,
                           ConnectStatus connect_status, const std::string& error, bool is_doctor) {
    if (is_doctor) {
        return run_local_doctor(options.account, socket_path, options.json, error);
    }
    if (connect_status == ConnectStatus::Unavailable && options.unavailable_route_error) {
        const auto& route_error = *options.unavailable_route_error;
        print_error(route_error.code, route_error.message, route_error.details);
        return route_error.exit_code;
    }
    print_error("GENERIC", error, json::object());
    return kGeneric;
}

int run_routed_command(const proto::Request& request, const RunOptions& options,
                       ChallengePrompt& prompt) {
    const auto env = paths::real_environment();
    std::string error;
    const auto socket_path = paths::socket_path(options.account, env, error);
    if (!socket_path) {
        print_error("USAGE", error, json::object());
        return kUsage;
    }
    if (!paths::ensure_private_dir(paths::runtime_dir(env), env.uid, error)) {
        print_error("GENERIC", error, json::object());
        return kGeneric;
    }
    const std::string requested_command = command_key(request.command);
    const bool is_doctor = requested_command == "doctor";
    const Deadline deadline = std::chrono::steady_clock::now() + options.restart_timeout;

    for (int attempt = 0; attempt < 2; ++attempt) {
        Session session;
        {
            ConnectStatus connect_status = ConnectStatus::Failed;
            auto connected =
                connect_with_spawn(options.account, *socket_path, env.uid, options.auto_spawn,
                                   options.daemon_executable, deadline, connect_status, error);
            if (!connected) {
                return report_connect_failure(options, *socket_path, connect_status, error,
                                              is_doctor);
            }
            session.fd = connected->fd;
            session.socket_identity = connected->identity;
        }
        proto::FrameReader reader(session.fd);
        const auto handshake_result = handshake(session.fd, reader, deadline, error);
        switch (handshake_result.outcome) {
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
            if (!options.restart_on_mismatch) {
                print_error("GENERIC", "daemon handshake mismatch during read-only preflight",
                            json::object());
                return kGeneric;
            }
            if (attempt > 0) {
                print_error("GENERIC", "daemon handshake mismatch persists after restart",
                            json::object());
                return kGeneric;
            }
            break;
        }

        if (!recover_mismatched_daemon(handshake_result.outcome, options.account, env, *socket_path,
                                       session, reader, deadline, error)) {
            print_error("GENERIC", error, json::object());
            return kGeneric;
        }
    }
    print_error("GENERIC", "daemon restart loop; giving up", json::object());
    return kGeneric;
}

std::optional<int> validate_request_route(const proto::Request& request,
                                          const RunOptions& options) {
    if (request.account != options.account) {
        print_error("ACCOUNT_MISMATCH", "request account does not match the selected route",
                    {{"requested_account", request.account}, {"daemon_account", options.account}});
        return kNotFound;
    }
    if (!paths::valid_account_name(request.account)) {
        print_error("USAGE", "invalid routed account", json::object());
        return kUsage;
    }
    return std::nullopt;
}

} // namespace

int run_command(const proto::Request& request, const RunOptions& options) {
    if (const auto route_error = validate_request_route(request, options); route_error) {
        return *route_error;
    }
    TerminalPrompt terminal_prompt;
    ChallengePrompt& prompt = options.prompt != nullptr ? *options.prompt : terminal_prompt;
    if (request.command == std::vector<std::string>{"logout"} && request.context.dry_run) {
        return run_logout_dry_run(request, options);
    }
    if (is_config_global_command(request.command)) {
        return run_config_global(request, options, prompt);
    }
    const bool daemon_control = is_daemon_control_command(request.command);
    if (daemon_control && options.no_daemon) {
        print_error("USAGE", "daemon lifecycle commands do not support --no-daemon",
                    json::object());
        return kUsage;
    }
    if (options.no_daemon) {
        if (options.unavailable_route_error) {
            const auto& route_error = *options.unavailable_route_error;
            print_error(route_error.code, route_error.message, route_error.details);
            return route_error.exit_code;
        }
        return run_in_process(request, options, prompt);
    }

    if (daemon_control) {
        return run_daemon_control(request, options);
    }
    return run_routed_command(request, options, prompt);
}

} // namespace tgcli::cli
