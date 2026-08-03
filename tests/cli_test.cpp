// Client-path contract tests: commands driven through cli::run_command with
// stdout/stderr captured, asserting output JSON, exit codes, and stream
// discipline — the observables DESIGN.md §5 pins.

#include "cli/client.hpp"
#include "cli/prompt.hpp"
#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "daemon/server.hpp"
#include "proto/frame.hpp"
#include "proto/frame_io.hpp"
#include "schema_matcher.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tgcli/version.hpp>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

// Points every XDG dir at a fresh temp tree so tests never touch the real
// account state, and keeps the socket path short (sun_path limit).
class IsolatedEnv {
  public:
    IsolatedEnv() : root_("/tmp/tgcli-cli-test-" + std::to_string(getpid())) {
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        std::filesystem::permissions(root_, std::filesystem::perms::owner_all);
        for (const auto* name : kVars) {
            const char* old = std::getenv(name);
            saved_.emplace_back(name,
                                old != nullptr ? std::optional<std::string>(old) : std::nullopt);
            setenv(name, root_.c_str(), 1);
        }
    }

    IsolatedEnv(const IsolatedEnv&) = delete;
    IsolatedEnv& operator=(const IsolatedEnv&) = delete;
    IsolatedEnv(IsolatedEnv&&) = delete;
    IsolatedEnv& operator=(IsolatedEnv&&) = delete;

    ~IsolatedEnv() {
        for (const auto& [name, value] : saved_) {
            if (value) {
                setenv(name.c_str(), value->c_str(), 1);
            } else {
                unsetenv(name.c_str());
            }
        }
        std::filesystem::remove_all(root_);
    }

    [[nodiscard]] const std::string& root() const {
        return root_;
    }

  private:
    static constexpr std::array<const char*, 4> kVars = {"XDG_RUNTIME_DIR", "XDG_STATE_HOME",
                                                         "XDG_DATA_HOME", "XDG_CONFIG_HOME"};
    std::string root_;
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

// Redirects an stdio fd into a file for the duration of a callback.
class CaptureStream {
  public:
    CaptureStream(FILE* stream, int fd, std::string path)
        : stream_(stream), fd_(fd), saved_(::dup(fd)), path_(std::move(path)) {
        std::fflush(stream_);
        const int target = ::open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        ::dup2(target, fd_);
        ::close(target);
    }

    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;
    CaptureStream(CaptureStream&&) = delete;
    CaptureStream& operator=(CaptureStream&&) = delete;

    ~CaptureStream() {
        std::fflush(stream_);
        ::dup2(saved_, fd_);
        ::close(saved_);
    }

    [[nodiscard]] std::string contents() const {
        std::fflush(stream_);
        const std::ifstream in(path_);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

  private:
    FILE* stream_;
    int fd_;
    int saved_;
    std::string path_;
};

struct RunOutcome {
    int exit_code = -1;
    std::string out;
    std::string err;
};

RunOutcome run_captured(const std::vector<std::string>& command, const cli::RunOptions& options,
                        const IsolatedEnv& env) {
    proto::Request request;
    request.id = 1;
    request.command = command;
    request.context.json = options.json;
    request.context.cwd = "/";

    RunOutcome outcome;
    {
        const CaptureStream out(stdout, STDOUT_FILENO, env.root() + "/stdout.txt");
        const CaptureStream err(stderr, STDERR_FILENO, env.root() + "/stderr.txt");
        outcome.exit_code = cli::run_command(request, options);
        outcome.out = out.contents();
        outcome.err = err.contents();
    }
    return outcome;
}

RunOutcome run_request_captured(const proto::Request& request, const cli::RunOptions& options,
                                const IsolatedEnv& env) {
    RunOutcome outcome;
    {
        const CaptureStream out(stdout, STDOUT_FILENO, env.root() + "/stdout.txt");
        const CaptureStream err(stderr, STDERR_FILENO, env.root() + "/stderr.txt");
        outcome.exit_code = cli::run_command(request, options);
        outcome.out = out.contents();
        outcome.err = err.contents();
    }
    return outcome;
}

class InjectedPrompt final : public cli::ChallengePrompt {
  public:
    explicit InjectedPrompt(cli::PromptResultKind result) : result_(result) {}

    cli::PromptResult prompt(const json& challenge) override {
        if (result_ == cli::PromptResultKind::Unavailable ||
            result_ == cli::PromptResultKind::Error) {
            return {result_};
        }
        json answer{{"nonce", challenge["nonce"]},
                    {"sequence", challenge["sequence"]},
                    {"client_generation", challenge["client_generation"]},
                    {"auth_sequence", challenge["auth_sequence"]}};
        if (result_ == cli::PromptResultKind::Cancelled) {
            answer["cancelled"] = true;
        } else {
            answer["value"] = "12345";
        }
        return {result_, std::move(answer)};
    }

  private:
    cli::PromptResultKind result_;
};

void install_client_challenge(daemon::Dispatcher& dispatcher) {
    dispatcher.register_command(
        "client challenge",
        {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& session) {
             const daemon::ChallengeSpec spec{
                 proto::ChallengeKind::AuthenticationCode,
                 4,
                 9,
                 "Code: ",
                 {{"delivery_type", "sms"}, {"expected_length", 5}, {"resend_timeout", 30}}};
             const auto outcome = session.challenge(spec);
             switch (outcome.status) {
             case daemon::ChallengeStatus::Answered:
                 if (session.reserve_in_flight()) {
                     session.settle_in_flight();
                     session.result({{"value", std::get<std::string>(outcome.value)}});
                 }
                 return;
             case daemon::ChallengeStatus::Cancelled:
                 session.error("AUTH_CANCELLED", "authentication cancelled",
                               {{"account", "main"},
                                {"state", "wait_code"},
                                {"challenge", "authentication_code"}},
                               kNotAuthed);
                 return;
             case daemon::ChallengeStatus::TimedOut:
                 session.error("TIMEOUT", "authentication timed out",
                               {{"operation", "login"}, {"state", "wait_code"}}, kTimeout);
                 return;
             case daemon::ChallengeStatus::NoTty:
             case daemon::ChallengeStatus::Superseded:
                 session.error("INTERNAL", "unexpected challenge state", json::object(), kGeneric);
                 return;
             case daemon::ChallengeStatus::Disconnected:
             case daemon::ChallengeStatus::Shutdown:
             case daemon::ChallengeStatus::ProtocolError:
                 return;
             }
         }});
}

proto::Request client_challenge_request() {
    proto::Request request;
    request.id = 1;
    request.command = {"client", "challenge"};
    request.context.tty = true;
    request.context.json = true;
    request.context.timeout_seconds = 2.0;
    request.context.cwd = "/";
    return request;
}

void prepare_account_layout() {
    const auto env = paths::real_environment();
    std::string error;
    REQUIRE(paths::ensure_private_dir(paths::runtime_dir(env), env.uid, error));
    const std::string state_dir = paths::account_state_dir("main", env);
    const auto accounts_separator = state_dir.rfind('/');
    REQUIRE(accounts_separator != std::string::npos);
    const std::string accounts_dir = state_dir.substr(0, accounts_separator);
    const auto tgcli_separator = accounts_dir.rfind('/');
    REQUIRE(tgcli_separator != std::string::npos);
    REQUIRE(paths::ensure_private_dir(accounts_dir.substr(0, tgcli_separator), env.uid, error));
    REQUIRE(paths::ensure_private_dir(accounts_dir, env.uid, error));
    REQUIRE(paths::ensure_private_dir(state_dir, env.uid, error));
}

class ScopedSleeper {
  public:
    ScopedSleeper() : pid_(::fork()) {
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::signal(SIGTERM, SIG_DFL);
            for (;;) {
                ::pause();
            }
        }
    }

    ScopedSleeper(const ScopedSleeper&) = delete;
    ScopedSleeper& operator=(const ScopedSleeper&) = delete;
    ScopedSleeper(ScopedSleeper&&) = delete;
    ScopedSleeper& operator=(ScopedSleeper&&) = delete;

    ~ScopedSleeper() {
        terminate();
    }

    [[nodiscard]] pid_t pid() const {
        return pid_;
    }

    [[nodiscard]] bool running() const {
        return pid_ > 0 && (::kill(pid_, 0) == 0 || errno == EPERM);
    }

  private:
    void terminate() {
        if (pid_ <= 0) {
            return;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == 0) {
            ::kill(pid_, SIGTERM);
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
    }

    pid_t pid_ = -1;
};

struct ChildDaemonOptions {
    enum class ShutdownOrder { EndpointsThenLock, LockThenEndpoints };

    bool malformed_identity = false;
    bool ignore_control_token = false;
    bool protocol_mismatch = true;
    ShutdownOrder shutdown_order = ShutdownOrder::EndpointsThenLock;
    std::chrono::milliseconds shutdown_gap{0};
};

struct BoundEndpoint {
    int fd = -1;
    paths::SocketIdentity identity;
};

std::optional<BoundEndpoint> bind_private_endpoint(const std::string& endpoint, int type,
                                                   std::string& error) {
    if (!paths::prepare_socket_endpoint(endpoint, getuid(), error)) {
        return std::nullopt;
    }
    const int fd = net::socket_cloexec(AF_UNIX, type, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);
    const mode_t old_umask = ::umask(0177);
    const int bind_result = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::umask(old_umask);
    if (bind_result != 0 || (type == SOCK_STREAM && ::listen(fd, SOMAXCONN) != 0)) {
        ::close(fd);
        return std::nullopt;
    }
    const auto identity = paths::inspect_socket_endpoint(endpoint, getuid(), error);
    if (!identity) {
        ::close(fd);
        return std::nullopt;
    }
    return BoundEndpoint{fd, *identity};
}

class ChildProtocolDaemon {
  public:
    explicit ChildProtocolDaemon(ChildDaemonOptions options = {}) {
        prepare_account_layout();
        std::array<int, 2> ready_fds{-1, -1};
        REQUIRE(::pipe(ready_fds.data()) == 0);
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::signal(SIGTERM, SIG_DFL);
            ::close(ready_fds[0]);
            run_child(ready_fds[1], options);
        }
        ::close(ready_fds[1]);
        char ready = 0;
        ssize_t count = -1;
        do {
            count = ::read(ready_fds[0], &ready, 1);
        } while (count < 0 && errno == EINTR);
        ::close(ready_fds[0]);
        if (count != 1 || ready != '1') {
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
        REQUIRE(pid_ > 0);
    }

    ChildProtocolDaemon(const ChildProtocolDaemon&) = delete;
    ChildProtocolDaemon& operator=(const ChildProtocolDaemon&) = delete;
    ChildProtocolDaemon(ChildProtocolDaemon&&) = delete;
    ChildProtocolDaemon& operator=(ChildProtocolDaemon&&) = delete;

    ~ChildProtocolDaemon() {
        terminate();
    }

    [[nodiscard]] bool running() const {
        return pid_ > 0 && (::kill(pid_, 0) == 0 || errno == EPERM);
    }

    int wait_for_exit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        for (;;) {
            const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if (waited < 0 && errno != EINTR) {
                pid_ = -1;
                return -1;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

  private:
    [[noreturn]] static void run_child(int ready_fd, const ChildDaemonOptions& options) {
        const auto env = paths::real_environment();
        std::string error;
        const auto socket_path = paths::socket_path("main", env, error);
        const auto control_socket_path = paths::control_socket_path("main", env, error);
        const std::string state_dir = paths::account_state_dir("main", env);
        daemon_lock::Identity identity;
        const int lock_fd = daemon_lock::acquire(state_dir + "/daemon.lock", identity, error);
        if (lock_fd < 0 || !socket_path || !control_socket_path) {
            const char failed = '0';
            static_cast<void>(::write(ready_fd, &failed, 1));
            ::_exit(2);
        }
        if (options.malformed_identity) {
            constexpr std::string_view malformed = "malformed\n";
            if (::ftruncate(lock_fd, 0) != 0 ||
                ::pwrite(lock_fd, malformed.data(), malformed.size(), 0) !=
                    static_cast<ssize_t>(malformed.size())) {
                const char failed = '0';
                static_cast<void>(::write(ready_fd, &failed, 1));
                ::_exit(3);
            }
        }

        daemon::DaemonContext context;
        context.account = "main";
        context.binary_version = "old-test-binary";
        context.protocol_version = proto::kProtocolVersion + (options.protocol_mismatch ? 1 : 0);
        context.tdlib_version = "test";
        context.socket_path = *socket_path;
        daemon::Dispatcher dispatcher;
        std::string server_token = identity.control_token;
        if (options.ignore_control_token) {
            server_token.front() = server_token.front() == '0' ? '1' : '0';
        }
        daemon::Server server({*socket_path, context.binary_version, context.protocol_version,
                               *control_socket_path, server_token},
                              dispatcher);
        context.request_shutdown = [&server] { server.request_stop(); };
        daemon::register_commands(dispatcher, context);
        if (!server.start(error)) {
            const char failed = '0';
            static_cast<void>(::write(ready_fd, &failed, 1));
            ::_exit(4);
        }
        const char ready = '1';
        static_cast<void>(::write(ready_fd, &ready, 1));
        ::close(ready_fd);
        server.wait_for_stop();
        if (options.shutdown_order == ChildDaemonOptions::ShutdownOrder::LockThenEndpoints) {
            ::close(lock_fd);
            std::this_thread::sleep_for(options.shutdown_gap);
            server.stop();
        } else {
            server.stop();
            std::this_thread::sleep_for(options.shutdown_gap);
            ::close(lock_fd);
        }
        ::_exit(0);
    }

    void terminate() {
        if (pid_ <= 0) {
            return;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == 0) {
            ::kill(pid_, SIGTERM);
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
    }

    pid_t pid_ = -1;
};

class ChildBinaryRaceDaemon {
  public:
    ChildBinaryRaceDaemon() {
        prepare_account_layout();
        std::array<int, 2> ready_fds{-1, -1};
        REQUIRE(::pipe(ready_fds.data()) == 0);
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::signal(SIGTERM, SIG_DFL);
            ::signal(SIGPIPE, SIG_IGN);
            ::close(ready_fds[0]);
            run_child(ready_fds[1]);
        }
        ::close(ready_fds[1]);
        char ready = 0;
        ssize_t count = -1;
        do {
            count = ::read(ready_fds[0], &ready, 1);
        } while (count < 0 && errno == EINTR);
        ::close(ready_fds[0]);
        if (count != 1 || ready != '1') {
            reap_blocking();
        }
        REQUIRE(pid_ > 0);
    }

    ChildBinaryRaceDaemon(const ChildBinaryRaceDaemon&) = delete;
    ChildBinaryRaceDaemon& operator=(const ChildBinaryRaceDaemon&) = delete;
    ChildBinaryRaceDaemon(ChildBinaryRaceDaemon&&) = delete;
    ChildBinaryRaceDaemon& operator=(ChildBinaryRaceDaemon&&) = delete;

    ~ChildBinaryRaceDaemon() {
        terminate();
    }

    int wait_for_exit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if (waited < 0 && errno != EINTR) {
                pid_ = -1;
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return -1;
    }

  private:
    static bool read_client_frames(int fd, proto::IoDeadline deadline) {
        proto::FrameReader reader(fd);
        std::string error;
        const auto hello_line = reader.read_line_until(deadline, error);
        if (!hello_line) {
            return false;
        }
        std::string parse_error;
        const auto hello = proto::parse(*hello_line, parse_error);
        if (!hello || std::get_if<proto::Hello>(&*hello) == nullptr) {
            return false;
        }
        const auto stop_line = reader.read_line_until(deadline, error);
        if (!stop_line) {
            return false;
        }
        const auto stop = proto::parse(*stop_line, parse_error);
        const auto* request = stop ? std::get_if<proto::Request>(&*stop) : nullptr;
        return request != nullptr && request->command == std::vector<std::string>{"daemon", "stop"};
    }

    [[noreturn]] static void run_child(int ready_fd) {
        const auto env = paths::real_environment();
        std::string error;
        const auto socket_path = paths::socket_path("main", env, error);
        const auto control_path = paths::control_socket_path("main", env, error);
        const std::string state_dir = paths::account_state_dir("main", env);
        daemon_lock::Identity identity;
        const int lock_fd = daemon_lock::acquire(state_dir + "/daemon.lock", identity, error);
        if (lock_fd < 0 || !socket_path || !control_path) {
            report_ready(ready_fd, false);
            ::_exit(2);
        }
        auto main = bind_private_endpoint(*socket_path, SOCK_STREAM, error);
        auto control = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
        if (!main || !control) {
            report_ready(ready_fd, false);
            ::_exit(3);
        }
        report_ready(ready_fd, true);
        ::close(ready_fd);

        std::array<int, 2> connections{-1, -1};
        for (int& connection : connections) {
            do {
                connection = net::accept_cloexec(main->fd);
            } while (connection < 0 && errno == EINTR);
            if (connection < 0 ||
                !proto::write_frame(
                    connection, proto::Hello{"old-test-binary", proto::kProtocolVersion}, error)) {
                ::_exit(4);
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        const bool first_stop = read_client_frames(connections[0], deadline);
        const bool duplicate_stop = read_client_frames(connections[1], deadline);
        for (const int connection : connections) {
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
        }
        ::close(main->fd);
        ::close(control->fd);
        paths::unlink_socket_endpoint_if_same(*socket_path, main->identity);
        paths::unlink_socket_endpoint_if_same(*control_path, control->identity);
        ::close(lock_fd);
        ::_exit(first_stop && duplicate_stop ? 0 : 5);
    }

    static void report_ready(int fd, bool ready) {
        const char value = ready ? '1' : '0';
        static_cast<void>(::write(fd, &value, 1));
    }

    void reap_blocking() {
        if (pid_ <= 0) {
            return;
        }
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

    void terminate() {
        if (pid_ <= 0) {
            return;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == 0) {
            ::kill(pid_, SIGTERM);
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
    }

    pid_t pid_ = -1;
};

constexpr std::string_view kFrozenControlToken = "0123456789abcdef0123456789abcdef";

bool frozen_fixture_process_start(std::string& process_start) {
#if defined(__linux__)
    std::ifstream input("/proc/self/stat");
    std::string data;
    if (!input || !std::getline(input, data)) {
        return false;
    }
    const auto command_end = data.rfind(')');
    if (command_end == std::string::npos || command_end + 2 >= data.size()) {
        return false;
    }
    std::istringstream fields(data.substr(command_end + 2));
    std::string value;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value)) {
            return false;
        }
    }
    process_start = "linux:" + value;
    return true;
#elif defined(__APPLE__)
    proc_bsdinfo info{};
    if (::proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) !=
        static_cast<int>(sizeof(info))) {
        return false;
    }
    process_start = "macos:" + std::to_string(info.pbi_start_tvsec) + ":" +
                    std::to_string(info.pbi_start_tvusec);
    return true;
#else
    (void)process_start;
    return false;
#endif
}

int acquire_frozen_fixture_lock(const std::string& lock_path, daemon_lock::Identity& identity,
                                std::string& error) {
    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(lock_path.c_str(), flags, 0600);
    if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        error = "cannot acquire frozen fixture lock";
        if (fd >= 0) {
            ::close(fd);
        }
        return -1;
    }
    struct flock owner_lock {};
    owner_lock.l_type = F_WRLCK;
    owner_lock.l_whence = SEEK_SET;
    if (::fcntl(fd, F_SETLK, &owner_lock) != 0 ||
        !frozen_fixture_process_start(identity.process_start)) {
        error = "cannot establish frozen fixture identity";
        ::close(fd);
        return -1;
    }
    identity.pid = getpid();
    identity.control_token = kFrozenControlToken;
    const std::string record = "tgcli-lock-v1 " + std::to_string(identity.pid) + " " +
                               identity.process_start + " " + identity.control_token + "\n";
    if (::ftruncate(fd, 0) != 0 ||
        ::pwrite(fd, record.data(), record.size(), 0) != static_cast<ssize_t>(record.size())) {
        error = "cannot write frozen fixture identity";
        ::close(fd);
        return -1;
    }
    return fd;
}

// Signal-handler communication requires a namespace-lifetime sig_atomic_t.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile sig_atomic_t bootstrap_signal_stop = 0;

extern "C" void bootstrap_signal_handler(int /*signal*/) {
    bootstrap_signal_stop = 1;
}

enum class BootstrapHelloMode { Incompatible, Stall };

struct BootstrapDaemonOptions {
    BootstrapHelloMode hello_mode = BootstrapHelloMode::Incompatible;
    bool consume_control = true;
    bool malformed_identity = false;
    std::chrono::milliseconds control_stop_delay{0};
};

class ChildBootstrapDaemon {
  public:
    explicit ChildBootstrapDaemon(BootstrapDaemonOptions options = {}) {
        prepare_account_layout();
        const auto env = paths::real_environment();
        std::string error;
        const auto control_path = paths::control_socket_path("main", env, error);
        REQUIRE(control_path.has_value());
        control_path_ = *control_path;

        std::array<int, 2> ready_fds{-1, -1};
        REQUIRE(::pipe(ready_fds.data()) == 0);
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::close(ready_fds[0]);
            run_child(ready_fds[1], options);
        }
        ::close(ready_fds[1]);
        char ready = 0;
        ssize_t count = -1;
        do {
            count = ::read(ready_fds[0], &ready, 1);
        } while (count < 0 && errno == EINTR);
        ::close(ready_fds[0]);
        if (count != 1 || ready != '1') {
            reap_blocking();
        }
        REQUIRE(pid_ > 0);
    }

    ChildBootstrapDaemon(const ChildBootstrapDaemon&) = delete;
    ChildBootstrapDaemon& operator=(const ChildBootstrapDaemon&) = delete;
    ChildBootstrapDaemon(ChildBootstrapDaemon&&) = delete;
    ChildBootstrapDaemon& operator=(ChildBootstrapDaemon&&) = delete;

    ~ChildBootstrapDaemon() {
        terminate();
    }

    [[nodiscard]] bool running() const {
        return pid_ > 0 && (::kill(pid_, 0) == 0 || errno == EPERM);
    }

    void request_external_stop() const {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
        }
    }

    int wait_for_exit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if (waited < 0 && errno != EINTR) {
                pid_ = -1;
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return -1;
    }

    int fill_control_queue() {
        const int fd = net::socket_cloexec(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) {
            return -1;
        }
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            ::close(fd);
            return -1;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, control_path_.c_str(), sizeof(addr.sun_path) - 1);
        int sent = 0;
        constexpr char invalid = 'x';
        for (;;) {
            const ssize_t count = ::sendto(fd, &invalid, 1, MSG_DONTWAIT,
                                           reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
            if (count == 1) {
                ++sent;
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
                break;
            }
            sent = -1;
            break;
        }
        if (sent > 0) {
            queue_filler_fd_ = fd;
        } else {
            ::close(fd);
        }
        return sent;
    }

  private:
    [[noreturn]] static void run_child(int ready_fd, const BootstrapDaemonOptions& options) {
        bootstrap_signal_stop = 0;
        ::signal(SIGTERM, bootstrap_signal_handler);
        ::signal(SIGPIPE, SIG_IGN);
        const auto env = paths::real_environment();
        std::string error;
        const auto socket_path = paths::socket_path("main", env, error);
        const auto control_path = paths::control_socket_path("main", env, error);
        const std::string state_dir = paths::account_state_dir("main", env);
        daemon_lock::Identity identity;
        const int lock_fd =
            acquire_frozen_fixture_lock(state_dir + "/daemon.lock", identity, error);
        if (lock_fd < 0 || !socket_path || !control_path) {
            report_ready(ready_fd, false);
            ::_exit(2);
        }
        if (options.malformed_identity) {
            constexpr std::string_view malformed = "malformed\n";
            if (::ftruncate(lock_fd, 0) != 0 ||
                ::pwrite(lock_fd, malformed.data(), malformed.size(), 0) !=
                    static_cast<ssize_t>(malformed.size())) {
                report_ready(ready_fd, false);
                ::_exit(3);
            }
        }
        auto main = bind_private_endpoint(*socket_path, SOCK_STREAM, error);
        auto control = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
        if (!main || !control) {
            report_ready(ready_fd, false);
            ::_exit(4);
        }
        const int control_flags = ::fcntl(control->fd, F_GETFL);
        if (control_flags < 0 || ::fcntl(control->fd, F_SETFL, control_flags | O_NONBLOCK) != 0) {
            report_ready(ready_fd, false);
            ::_exit(5);
        }
        report_ready(ready_fd, true);
        ::close(ready_fd);

        std::vector<int> connections;
        std::optional<std::chrono::steady_clock::time_point> stop_at;
        bool stopping = false;
        while (!stopping) {
            if (bootstrap_signal_stop != 0) {
                break;
            }
            if (stop_at && std::chrono::steady_clock::now() >= *stop_at) {
                break;
            }
            std::array<pollfd, 2> descriptors{
                {{main->fd, POLLIN, 0}, {options.consume_control ? control->fd : -1, POLLIN, 0}}};
            const int result = ::poll(descriptors.data(), descriptors.size(), 20);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0) {
                break;
            }
            if ((descriptors[0].revents & POLLIN) != 0) {
                const int connection = net::accept_cloexec(main->fd);
                if (connection >= 0) {
                    connections.push_back(connection);
                    if (options.hello_mode == BootstrapHelloMode::Incompatible) {
                        constexpr std::string_view old_hello =
                            "TGCLI-ANCIENT-HELLO binary=old framing=not-json\n";
                        int send_flags = 0;
#if defined(MSG_NOSIGNAL)
                        send_flags |= MSG_NOSIGNAL;
#endif
                        static_cast<void>(
                            ::send(connection, old_hello.data(), old_hello.size(), send_flags));
                    }
                }
            }
            if ((descriptors[1].revents & POLLIN) != 0) {
                std::array<char, 256> data{};
                const ssize_t count = ::recv(control->fd, data.data(), data.size(), 0);
                if (count == static_cast<ssize_t>(identity.control_token.size()) &&
                    std::equal(identity.control_token.begin(), identity.control_token.end(),
                               data.begin())) {
                    if (options.control_stop_delay.count() == 0) {
                        stopping = true;
                    } else {
                        stop_at = std::chrono::steady_clock::now() + options.control_stop_delay;
                    }
                }
            }
        }

        for (const int connection : connections) {
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
        }
        ::close(main->fd);
        ::close(control->fd);
        paths::unlink_socket_endpoint_if_same(*socket_path, main->identity);
        paths::unlink_socket_endpoint_if_same(*control_path, control->identity);
        ::close(lock_fd);
        ::_exit(0);
    }

    static void report_ready(int fd, bool ready) {
        const char value = ready ? '1' : '0';
        static_cast<void>(::write(fd, &value, 1));
    }

    void reap_blocking() {
        if (pid_ <= 0) {
            return;
        }
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

    void terminate() {
        if (queue_filler_fd_ >= 0) {
            ::close(queue_filler_fd_);
            queue_filler_fd_ = -1;
        }
        if (pid_ <= 0) {
            return;
        }
        request_external_stop();
        if (wait_for_exit(std::chrono::seconds(2)) >= 0) {
            return;
        }
        ::kill(pid_, SIGKILL);
        reap_blocking();
    }

    pid_t pid_ = -1;
    int queue_filler_fd_ = -1;
    std::string control_path_;
};

bool wait_until_missing(const std::string& first, const std::string& second) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::access(first.c_str(), F_OK) != 0 && ::access(second.c_str(), F_OK) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

class AsyncCliProcess {
  public:
    AsyncCliProcess(std::string output_path, std::string error_path)
        : output_path_(std::move(output_path)), error_path_(std::move(error_path)), pid_(::fork()) {
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            const int output = ::open(output_path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            const int errors = ::open(error_path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            if (output < 0 || errors < 0) {
                ::_exit(126);
            }
            ::dup2(output, STDOUT_FILENO);
            ::dup2(errors, STDERR_FILENO);
            ::close(output);
            ::close(errors);
            ::execl(TGCLI_TEST_BINARY, "tgcli", "--json", "version", static_cast<char*>(nullptr));
            ::_exit(127);
        }
    }

    AsyncCliProcess(const AsyncCliProcess&) = delete;
    AsyncCliProcess& operator=(const AsyncCliProcess&) = delete;
    AsyncCliProcess(AsyncCliProcess&&) = delete;
    AsyncCliProcess& operator=(AsyncCliProcess&&) = delete;

    ~AsyncCliProcess() {
        terminate();
    }

    int wait_for_exit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if (waited < 0 && errno != EINTR) {
                pid_ = -1;
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return -1;
    }

    [[nodiscard]] std::string output() const {
        return read_file(output_path_);
    }

    [[nodiscard]] std::string errors() const {
        return read_file(error_path_);
    }

  private:
    static std::string read_file(const std::string& file_path) {
        const std::ifstream input(file_path);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

    void terminate() {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

    std::string output_path_;
    std::string error_path_;
    pid_t pid_ = -1;
};

void stop_current_daemon(const IsolatedEnv& env) {
    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    const auto stopped = run_captured({"daemon", "stop"}, options, env);
    CHECK(stopped.exit_code == kOk);
    const auto real_env = paths::real_environment();
    std::string error;
    const auto socket_path = paths::socket_path("main", real_env, error);
    const auto control_path = paths::control_socket_path("main", real_env, error);
    REQUIRE(socket_path.has_value());
    REQUIRE(control_path.has_value());
    CHECK(wait_until_missing(*socket_path, *control_path));
}

} // namespace

TEST_CASE("no-daemon version: JSON on stdout, silence on stderr, exit 0", "[cli][tdlib]") {
    const IsolatedEnv env;
    cli::RunOptions options;
    options.json = true;
    options.no_daemon = true;

    const auto result = run_captured({"version"}, options, env);
    CHECK(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK(data["version"] == kVersion);
    CHECK(data["protocol"] == proto::kProtocolVersion);
    CHECK(data["tdlib"].is_string());
    CHECK_FALSE(data.contains("type"));
    CHECK_FALSE(data.contains("data"));
    CHECK_THAT(data, test::matches_json_schema("version.result.schema.json"));
}

TEST_CASE("no-daemon doctor result matches the in-process schema variant", "[cli][schema][tdlib]") {
    const IsolatedEnv env;
    cli::RunOptions options;
    options.json = true;
    options.no_daemon = true;

    const auto result = run_captured({"doctor"}, options, env);
    CHECK(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK_THAT(data, test::matches_json_schema("doctor.result.schema.json"));
}

TEST_CASE("no-daemon unknown command: USAGE error on stderr, exit 2", "[cli][tdlib]") {
    const IsolatedEnv env;
    cli::RunOptions options;
    options.no_daemon = true;

    const auto result = run_captured({"frobnicate"}, options, env);
    CHECK(result.exit_code == kUsage);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    CHECK(error["error"]["code"] == "USAGE");
}

TEST_CASE("socket and no-daemon clients map every prompt result immediately and identically",
          "[cli][challenge][process]") {
    for (const auto prompt_kind :
         {cli::PromptResultKind::Answer, cli::PromptResultKind::Cancelled,
          cli::PromptResultKind::Unavailable, cli::PromptResultKind::Error}) {
        DYNAMIC_SECTION(static_cast<int>(prompt_kind)) {
            const IsolatedEnv env;
            daemon::Dispatcher dispatcher;
            install_client_challenge(dispatcher);

            InjectedPrompt in_process_prompt(prompt_kind);
            cli::RunOptions in_process_options;
            in_process_options.json = true;
            in_process_options.no_daemon = true;
            in_process_options.prompt = &in_process_prompt;
            in_process_options.in_process_dispatcher = &dispatcher;
            const auto in_process_start = std::chrono::steady_clock::now();
            const auto in_process =
                run_request_captured(client_challenge_request(), in_process_options, env);
            const auto in_process_elapsed = std::chrono::steady_clock::now() - in_process_start;

            const auto real_env = paths::real_environment();
            std::string error;
            REQUIRE(paths::ensure_private_dir(paths::runtime_dir(real_env), real_env.uid, error));
            const auto socket_path = paths::socket_path("main", real_env, error);
            REQUIRE(socket_path.has_value());
            daemon::Server server({*socket_path, kVersion, proto::kProtocolVersion, {}, {}},
                                  dispatcher);
            REQUIRE(server.start(error));

            InjectedPrompt socket_prompt(prompt_kind);
            cli::RunOptions socket_options;
            socket_options.json = true;
            socket_options.auto_spawn = false;
            socket_options.prompt = &socket_prompt;
            const auto socket_start = std::chrono::steady_clock::now();
            const auto socket =
                run_request_captured(client_challenge_request(), socket_options, env);
            const auto socket_elapsed = std::chrono::steady_clock::now() - socket_start;
            server.stop();

            CHECK(in_process_elapsed < std::chrono::seconds(1));
            CHECK(socket_elapsed < std::chrono::seconds(1));
            CHECK(socket.exit_code == in_process.exit_code);
            CHECK(socket.out == in_process.out);
            CHECK(socket.err == in_process.err);

            if (prompt_kind == cli::PromptResultKind::Answer) {
                CHECK(socket.exit_code == kOk);
                CHECK(json::parse(socket.out) == json{{"value", "12345"}});
                CHECK(socket.err.empty());
            } else {
                CHECK(socket.out.empty());
                const auto rendered = json::parse(socket.err);
                CHECK(rendered["error"]["details"].is_object());
                if (prompt_kind == cli::PromptResultKind::Cancelled) {
                    CHECK(socket.exit_code == kNotAuthed);
                    CHECK(rendered["error"]["code"] == "AUTH_CANCELLED");
                    CHECK(rendered["error"]["message"] == "authentication cancelled");
                } else {
                    CHECK(socket.exit_code == kGeneric);
                    CHECK(rendered["error"]["code"] == "INTERNAL");
                    CHECK(rendered["error"]["message"] == "cannot read challenge response");
                    CHECK(rendered["error"]["details"] == json::object());
                }
            }
        }
    }
}

TEST_CASE("doctor degrades to local diagnostics when the daemon is unreachable", "[cli][schema]") {
    const IsolatedEnv env;
    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;

    const auto result = run_captured({"doctor"}, options, env);
    CHECK(result.exit_code == kOk);
    REQUIRE(std::count(result.err.begin(), result.err.end(), '\n') == 1);
    const auto warning = json::parse(result.err);
    REQUIRE(warning.is_object());
    REQUIRE(warning.size() == 1);
    REQUIRE(warning["warning"].is_string());
    CHECK(warning["warning"].get<std::string>().starts_with("daemon unreachable ("));
    CHECK(warning["warning"].get<std::string>().ends_with("; local diagnostics only"));
    const auto data = json::parse(result.out);
    CHECK(data["daemon"]["running"] == false);
    CHECK(data["config"].contains("exists"));
    CHECK_THAT(data, test::matches_json_schema("doctor.result.schema.json"));
}

TEST_CASE("reachable daemon JSON results match result schemas without envelopes",
          "[cli][schema][process][tdlib]") {
    const IsolatedEnv env;
    cli::RunOptions options;
    options.json = true;
    options.daemon_executable = TGCLI_TEST_BINARY;

    const auto version = run_captured({"version"}, options, env);
    CHECK(version.exit_code == kOk);
    CHECK(version.err.empty());
    const auto version_data = json::parse(version.out);
    CHECK_FALSE(version_data.contains("type"));
    CHECK_FALSE(version_data.contains("data"));
    CHECK_THAT(version_data, test::matches_json_schema("version.result.schema.json"));

    options.auto_spawn = false;
    const auto doctor = run_captured({"doctor"}, options, env);
    CHECK(doctor.exit_code == kOk);
    CHECK(doctor.err.empty());
    const auto doctor_data = json::parse(doctor.out);
    CHECK_FALSE(doctor_data.contains("type"));
    CHECK_FALSE(doctor_data.contains("data"));
    CHECK_THAT(doctor_data, test::matches_json_schema("doctor.result.schema.json"));

    const auto stopped = run_captured({"daemon", "stop"}, options, env);
    CHECK(stopped.exit_code == kOk);
    CHECK(stopped.err.empty());
    const auto stopped_data = json::parse(stopped.out);
    CHECK_FALSE(stopped_data.contains("type"));
    CHECK_FALSE(stopped_data.contains("data"));
    CHECK_THAT(stopped_data, test::matches_json_schema("daemon-stop.result.schema.json"));

    const auto real_env = paths::real_environment();
    std::string error;
    const auto socket_path = paths::socket_path("main", real_env, error);
    const auto control_path = paths::control_socket_path("main", real_env, error);
    REQUIRE(socket_path.has_value());
    REQUIRE(control_path.has_value());
    CHECK(wait_until_missing(*socket_path, *control_path));
}

TEST_CASE("client rejects a group-accessible socket directory", "[cli][paths]") {
    const IsolatedEnv env;
    const std::string socket_dir = env.root() + "/tgcli";
    std::filesystem::create_directories(socket_dir);
    std::filesystem::permissions(socket_dir, std::filesystem::perms::owner_all |
                                                 std::filesystem::perms::group_read);

    cli::RunOptions options;
    options.auto_spawn = false;
    const auto result = run_captured({"version"}, options, env);

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    CHECK(error["error"]["code"] == "GENERIC");
    CHECK(error["error"]["message"].get<std::string>().find("group/other accessible") !=
          std::string::npos);
}

TEST_CASE("daemon vanishing mid-exchange maps to a structured error, exit 1", "[cli]") {
    const IsolatedEnv env;
    // A fake daemon that completes the handshake, swallows the request, and
    // disconnects without answering.
    const std::string socket_dir = env.root() + "/tgcli";
    std::filesystem::create_directories(socket_dir);
    std::filesystem::permissions(socket_dir, std::filesystem::perms::owner_all);
    const std::string socket_path = socket_dir + "/main.sock";
    const int listen_fd = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    const mode_t old_umask = ::umask(0177);
    const int bind_result =
        ::bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::umask(old_umask);
    REQUIRE(bind_result == 0);
    REQUIRE(::listen(listen_fd, 1) == 0);

    std::thread fake_daemon([listen_fd] {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        std::string error;
        proto::write_frame(fd, proto::Hello{kVersion, proto::kProtocolVersion}, error);
        proto::FrameReader reader(fd);
        reader.read_line(error); // client hello
        reader.read_line(error); // the request — swallowed
        ::close(fd);
    });

    cli::RunOptions options;
    options.auto_spawn = false;
    const auto result = run_captured({"version"}, options, env);
    fake_daemon.join();
    ::close(listen_fd);

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    CHECK(error["error"]["code"] == "GENERIC");
}

TEST_CASE("write_frame to a closed peer reports an error instead of dying", "[cli]") {
    int fds[2] = {-1, -1}; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    REQUIRE(net::set_nosigpipe(fds[0]) >= 0);
    ::close(fds[1]);
    std::string error;
    // Two writes: the first may land in the dead peer's buffer; the second
    // must fail with EPIPE — and must not raise SIGPIPE.
    proto::write_frame(fds[0], proto::Hello{"x", 1}, error);
    CHECK_FALSE(proto::write_frame(fds[0], proto::Hello{"x", 1}, error));
    CHECK(!error.empty());
    ::close(fds[0]);
}

TEST_CASE("protocol mismatch stops the verified daemon, spawns, and re-handshakes",
          "[cli][process][tdlib]") {
    const IsolatedEnv env;
    ChildProtocolDaemon old_daemon;

    cli::RunOptions options;
    options.json = true;
    options.daemon_executable = TGCLI_TEST_BINARY;
    const auto result = run_captured({"version"}, options, env);

    CHECK(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK(data["version"] == kVersion);
    CHECK(data["protocol"] == proto::kProtocolVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
}

TEST_CASE("daemon stop succeeds after verified binary-mismatch shutdown",
          "[cli][process][restart][schema]") {
    const IsolatedEnv env;
    ChildProtocolDaemon old_daemon({.protocol_mismatch = false});

    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::seconds(3);
    const auto result = run_captured({"daemon", "stop"}, options, env);

    INFO(result.err);
    REQUIRE(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK(data == json{{"stopping", true}});
    CHECK_THAT(data, test::matches_json_schema("daemon-stop.result.schema.json"));
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);

    const auto real_env = paths::real_environment();
    std::string path_error;
    const auto socket_path = paths::socket_path("main", real_env, path_error);
    REQUIRE(socket_path.has_value());
    CHECK(::access(socket_path->c_str(), F_OK) != 0);
}

TEST_CASE("daemon stop succeeds after verified protocol-incompatible shutdown",
          "[cli][process][restart][schema]") {
    const IsolatedEnv env;
    ChildProtocolDaemon old_daemon;

    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::seconds(3);
    const auto result = run_captured({"daemon", "stop"}, options, env);

    INFO(result.err);
    REQUIRE(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK(data == json{{"stopping", true}});
    CHECK_THAT(data, test::matches_json_schema("daemon-stop.result.schema.json"));
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);

    const auto real_env = paths::real_environment();
    std::string path_error;
    const auto socket_path = paths::socket_path("main", real_env, path_error);
    REQUIRE(socket_path.has_value());
    CHECK(::access(socket_path->c_str(), F_OK) != 0);
}

TEST_CASE("frozen bootstrap stops a daemon with an incompatible non-JSON Hello",
          "[cli][restart][r1][tdlib]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon;

    cli::RunOptions options;
    options.json = true;
    options.daemon_executable = TGCLI_TEST_BINARY;
    options.restart_timeout = std::chrono::seconds(3);
    const auto result = run_captured({"version"}, options, env);

    REQUIRE(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK(data["version"] == kVersion);
    CHECK(data["protocol"] == proto::kProtocolVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
}

TEST_CASE("incompatible non-JSON Hello with malformed identity fails closed",
          "[cli][restart][r1]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon({.malformed_identity = true});

    cli::RunOptions options;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::milliseconds(100);
    const auto result = run_captured({"version"}, options, env);

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    CHECK(error["error"]["message"].get<std::string>().find("identity is malformed") !=
          std::string::npos);
    CHECK(old_daemon.running());
    old_daemon.request_external_stop();
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
}

TEST_CASE("binary mismatch waits for both shutdown orderings before real replacement",
          "[cli][restart][r2][tdlib]") {
    const IsolatedEnv env;
    ChildDaemonOptions child_options;
    child_options.protocol_mismatch = false;
    child_options.shutdown_gap = std::chrono::milliseconds(100);

    SECTION("listener disappears before lock release") {
        child_options.shutdown_order = ChildDaemonOptions::ShutdownOrder::EndpointsThenLock;
        ChildProtocolDaemon old_daemon(child_options);

        cli::RunOptions options;
        options.json = true;
        options.daemon_executable = TGCLI_TEST_BINARY;
        options.restart_timeout = std::chrono::seconds(3);
        const auto result = run_captured({"version"}, options, env);

        INFO(result.err);
        REQUIRE(result.exit_code == kOk);
        CHECK(result.err.empty());
        CHECK(json::parse(result.out)["version"] == kVersion);
        CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
        stop_current_daemon(env);
    }

    SECTION("lock releases before listener removal and reconnect") {
        child_options.shutdown_order = ChildDaemonOptions::ShutdownOrder::LockThenEndpoints;
        ChildProtocolDaemon old_daemon(child_options);

        cli::RunOptions options;
        options.json = true;
        options.daemon_executable = TGCLI_TEST_BINARY;
        options.restart_timeout = std::chrono::seconds(3);
        const auto result = run_captured({"version"}, options, env);

        INFO(result.err);
        REQUIRE(result.exit_code == kOk);
        CHECK(result.err.empty());
        CHECK(json::parse(result.out)["version"] == kVersion);
        CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
        stop_current_daemon(env);
    }
}

TEST_CASE("two incompatible clients converge on one matching replacement",
          "[cli][restart][r3][process][tdlib]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon;
    AsyncCliProcess first(env.root() + "/first.out", env.root() + "/first.err");
    AsyncCliProcess second(env.root() + "/second.out", env.root() + "/second.err");

    const int first_exit = first.wait_for_exit(std::chrono::seconds(12));
    const int second_exit = second.wait_for_exit(std::chrono::seconds(12));
    INFO(first.errors());
    INFO(second.errors());
    REQUIRE(first_exit == kOk);
    REQUIRE(second_exit == kOk);
    CHECK(first.errors().empty());
    CHECK(second.errors().empty());
    CHECK(json::parse(first.output())["version"] == kVersion);
    CHECK(json::parse(second.output())["version"] == kVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
}

TEST_CASE("two binary-mismatch clients join one replacement after duplicate-stop EOF",
          "[cli][restart][r3][process][tdlib]") {
    const IsolatedEnv env;
    ChildBinaryRaceDaemon old_daemon;
    AsyncCliProcess first(env.root() + "/binary-first.out", env.root() + "/binary-first.err");
    AsyncCliProcess second(env.root() + "/binary-second.out", env.root() + "/binary-second.err");

    const int first_exit = first.wait_for_exit(std::chrono::seconds(12));
    const int second_exit = second.wait_for_exit(std::chrono::seconds(12));
    INFO(first.errors());
    INFO(second.errors());
    REQUIRE(first_exit == kOk);
    REQUIRE(second_exit == kOk);
    CHECK(first.errors().empty());
    CHECK(second.errors().empty());
    CHECK(json::parse(first.output())["version"] == kVersion);
    CHECK(json::parse(second.output())["version"] == kVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
}

TEST_CASE("external graceful shutdown converges on replacement within one deadline",
          "[cli][restart][r3][tdlib]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon({.control_stop_delay = std::chrono::milliseconds(500)});
    std::thread external_stop([&old_daemon] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        old_daemon.request_external_stop();
    });

    cli::RunOptions options;
    options.json = true;
    options.daemon_executable = TGCLI_TEST_BINARY;
    options.restart_timeout = std::chrono::seconds(3);
    const auto result = run_captured({"version"}, options, env);
    external_stop.join();

    REQUIRE(result.exit_code == kOk);
    CHECK(result.err.empty());
    CHECK(json::parse(result.out)["version"] == kVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
}

#if defined(__linux__)
TEST_CASE("saturated bootstrap control queue obeys the total deadline", "[cli][restart][r3]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon({.consume_control = false});
    REQUIRE(old_daemon.fill_control_queue() > 0);

    cli::RunOptions options;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::milliseconds(100);
    const auto started = std::chrono::steady_clock::now();
    const auto result = run_captured({"version"}, options, env);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    INFO(error.dump());
    // Linux may admit the authenticated datagram from a fresh sender even
    // after the filling sender reached EAGAIN. Either send retry or the
    // unchanged-owner barrier must consume only the same total deadline.
    CHECK(error["error"]["message"].get<std::string>().find("timed out") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(old_daemon.running());
    old_daemon.request_external_stop();
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
}
#endif

TEST_CASE("stalled Hello obeys the total deadline without triggering stop", "[cli][restart][r3]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon({.hello_mode = BootstrapHelloMode::Stall});

    cli::RunOptions options;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::milliseconds(100);
    const auto started = std::chrono::steady_clock::now();
    const auto result = run_captured({"version"}, options, env);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    CHECK(error["error"]["message"].get<std::string>().find("timed out reading frame") !=
          std::string::npos);
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(old_daemon.running());
    old_daemon.request_external_stop();
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
}

TEST_CASE("malformed held daemon identity refuses protocol restart", "[cli][restart]") {
    const IsolatedEnv env;
    const ChildProtocolDaemon old_daemon({.malformed_identity = true});

    cli::RunOptions options;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::milliseconds(100);
    const auto result = run_captured({"version"}, options, env);

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    CHECK(error["error"]["code"] == "GENERIC");
    CHECK(error["error"]["message"].get<std::string>().find("identity is malformed") !=
          std::string::npos);
    CHECK(old_daemon.running());
}

TEST_CASE("unlocked recorded PID is never signalled on protocol mismatch", "[cli][restart]") {
    const IsolatedEnv env;
    prepare_account_layout();
    const ScopedSleeper unrelated;
    const auto real_env = paths::real_environment();
    std::string error;
    const std::string lock_path = paths::account_state_dir("main", real_env) + "/daemon.lock";
    {
        std::ofstream lock(lock_path);
        lock << "tgcli-lock-v1 " << unrelated.pid() << " stale 00000000000000000000000000000000\n";
    }
    REQUIRE(::chmod(lock_path.c_str(), 0600) == 0);

    const auto socket_path = paths::socket_path("main", real_env, error);
    REQUIRE(socket_path.has_value());
    daemon::DaemonContext context;
    context.account = "main";
    context.binary_version = "old-test-binary";
    context.protocol_version = proto::kProtocolVersion + 1;
    context.tdlib_version = "test";
    context.socket_path = *socket_path;
    daemon::Dispatcher dispatcher;
    daemon::Server server({*socket_path, context.binary_version, context.protocol_version, {}, {}},
                          dispatcher);
    context.request_shutdown = [&server] { server.request_stop(); };
    daemon::register_commands(dispatcher, context);
    REQUIRE(server.start(error));

    cli::RunOptions options;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::milliseconds(100);
    const auto result = run_captured({"version"}, options, env);

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error_json = json::parse(result.err);
    INFO(error_json.dump());
    CHECK(error_json["error"]["message"].get<std::string>().find("lock is not held") !=
          std::string::npos);
    CHECK(unrelated.running());
    server.stop();
}

TEST_CASE("ignored protocol stop request times out with structured stderr", "[cli][restart]") {
    const IsolatedEnv env;
    const ChildProtocolDaemon old_daemon({.ignore_control_token = true});

    cli::RunOptions options;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::milliseconds(100);
    const auto result = run_captured({"version"}, options, env);

    CHECK(result.exit_code == kGeneric);
    CHECK(result.out.empty());
    const auto error = json::parse(result.err);
    CHECK(error["error"]["code"] == "GENERIC");
    CHECK(error["error"]["message"].get<std::string>().find("timed out") != std::string::npos);
    CHECK(old_daemon.running());
}
