// Client-path contract tests: commands driven through cli::run_command with
// stdout/stderr captured, asserting output JSON, exit codes, and stream
// discipline — the observables DESIGN.md §5 pins.

#include "cli/client.hpp"
#include "cli/control_stop.hpp"
#include "cli/prompt.hpp"
#include "cli/surface_safety.hpp"
#include "common/config.hpp"
#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "core/td_client.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "daemon/server.hpp"
#include "proto/frame.hpp"
#include "proto/frame_io.hpp"
#include "proto/operation.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
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
    proto::Request request(options.account);
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

RunOutcome run_binary_captured(const std::vector<std::string>& arguments, const IsolatedEnv& env,
                               const std::string& stem) {
    const std::string output_path = env.root() + "/" + stem + ".out";
    const std::string error_path = env.root() + "/" + stem + ".err";
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        const int output = ::open(output_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        const int errors = ::open(error_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (output < 0 || errors < 0) {
            ::_exit(126);
        }
        ::dup2(output, STDOUT_FILENO);
        ::dup2(errors, STDERR_FILENO);
        ::close(output);
        ::close(errors);
        std::vector<std::string> argument_storage{"tgcli"};
        argument_storage.insert(argument_storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(argument_storage.size() + 1);
        for (auto& argument : argument_storage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        ::execv(TGCLI_TEST_BINARY, argv.data());
        ::_exit(127);
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    const auto read_file = [](const std::string& filename) {
        const std::ifstream input(filename);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    };
    return {WEXITSTATUS(status), read_file(output_path), read_file(error_path)};
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
             auto outcome = session.challenge(spec);
             switch (outcome.status()) {
             case daemon::ChallengeStatus::Answered:
                 if (session.reserve_in_flight()) {
                     session.settle_in_flight();
                     std::string value;
                     static_cast<void>(outcome.take_string(value));
                     session.result({{"value", value}});
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
    proto::Request request("main");
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

std::string prepare_state_layout() {
    const auto env = paths::real_environment();
    const std::string state_dir = paths::account_state_dir("main", env);
    std::filesystem::create_directories(state_dir);
    std::filesystem::permissions(state_dir, std::filesystem::perms::owner_all);
    return state_dir;
}

void write_private_file(const std::string& filename, std::string_view bytes, mode_t mode = 0600) {
    const int fd = ::open(filename.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode);
    REQUIRE(fd >= 0);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        REQUIRE(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    REQUIRE(::close(fd) == 0);
    REQUIRE(::chmod(filename.c_str(), mode) == 0);
}

bool write_child_status(int fd, bool ready) noexcept {
    const char value = ready ? '1' : '0';
    ssize_t count = -1;
    do {
        count = ::write(fd, &value, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

[[noreturn]] void exit_child_startup_failure(int fd, int exit_code) noexcept {
    if (!write_child_status(fd, false)) {
        ::_exit(126);
    }
    ::_exit(exit_code);
}

void report_child_ready_or_exit(int fd) noexcept {
    if (!write_child_status(fd, true)) {
        ::_exit(126);
    }
}

std::string read_file_bytes(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void configure_main_account() {
    const auto env = paths::real_environment();
    const config::Store store(paths::config_file(env), env.uid);
    const auto current = store.load();
    REQUIRE(current);
    const auto added = store.add_account(current.snapshot->identity, "main");
    REQUIRE(added.status == config::MutationStatus::Applied);
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
    bool require_no_dispatch = false;
    bool report_ready_before_endpoints = false;
    int protocol_version = proto::kProtocolVersion + 1;
    std::string binary_version = "old-test-binary";
    ShutdownOrder shutdown_order = ShutdownOrder::EndpointsThenLock;
    std::chrono::milliseconds startup_delay{0};
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

int connect_fixture_socket(const std::string& socket_path) {
    const int fd = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

proto::Frame read_fixture_frame(proto::FrameReader& reader) {
    std::string error;
    const auto line =
        reader.read_line_until(std::chrono::steady_clock::now() + std::chrono::seconds(3), error);
    INFO(error);
    REQUIRE(line.has_value());
    std::string parse_error;
    auto frame = proto::parse(*line, parse_error);
    INFO(parse_error);
    REQUIRE(frame.has_value());
    return std::move(*frame);
}

bool exact_json_fields(const json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) {
        return false;
    }
    return std::all_of(fields.begin(), fields.end(), [&value](std::string_view field) {
        return value.contains(std::string(field));
    });
}

json raw_hello(std::string_view binary_version, int protocol_version) {
    return {{"type", "hello"},
            {"binary_version", binary_version},
            {"protocol_version", protocol_version}};
}

json raw_request_dialect(int protocol_version, std::uint64_t id,
                         const std::vector<std::string>& command) {
    json context{
        {"tty", false},       {"json", true}, {"yes", false},         {"dry_run", false},
        {"timeout", nullptr}, {"cwd", "/"},   {"media_dir", nullptr}, {"write_authority", "unset"}};
    if (protocol_version >= 3) {
        context["idempotency_key"] = nullptr;
    }
    json request{{"type", "request"},
                 {"id", id},
                 {"command", command},
                 {"args", json::object()},
                 {"context", std::move(context)}};
    if (protocol_version >= 2) {
        request["account"] = "main";
    }
    return request;
}

std::vector<std::string> command_parts(std::string_view command_path) {
    std::vector<std::string> parts;
    while (!command_path.empty()) {
        const auto separator = command_path.find(' ');
        parts.emplace_back(command_path.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        command_path.remove_prefix(separator + 1);
    }
    return parts;
}

bool valid_raw_request_dialect(const json& request, int protocol_version, std::uint64_t id,
                               const std::vector<std::string>& command) {
    const bool request_fields =
        protocol_version == 1
            ? exact_json_fields(request, {"type", "id", "command", "args", "context"})
            : exact_json_fields(request, {"type", "id", "account", "command", "args", "context"});
    if (!request_fields || request.value("type", "") != "request" || !request.contains("id") ||
        !request["id"].is_number_unsigned() || request["id"].get<std::uint64_t>() != id ||
        request.value("command", json::array()) != command || !request.contains("args") ||
        request["args"] != json::object() ||
        (protocol_version >= 2 && request.value("account", "") != "main") ||
        !request.contains("context")) {
        return false;
    }
    const auto& context = request["context"];
    return protocol_version >= 3
               ? exact_json_fields(context, {"tty", "json", "yes", "dry_run", "timeout", "cwd",
                                             "media_dir", "write_authority", "idempotency_key"}) &&
                     context["idempotency_key"].is_null()
               : exact_json_fields(context, {"tty", "json", "yes", "dry_run", "timeout", "cwd",
                                             "media_dir", "write_authority"});
}

json read_fixture_json(proto::FrameReader& reader) {
    std::string error;
    const auto line =
        reader.read_line_until(std::chrono::steady_clock::now() + std::chrono::seconds(3), error);
    INFO(error);
    REQUIRE(line.has_value());
    auto document = json::parse(*line, nullptr, false);
    REQUIRE_FALSE(document.is_discarded());
    return document;
}

void write_fixture_json(int fd, const json& document) {
    const std::string line = document.dump() + '\n';
    std::size_t offset = 0;
    while (offset < line.size()) {
        const auto count = ::write(fd, line.data() + offset, line.size() - offset);
        REQUIRE(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}

void check_fixture_eof(proto::FrameReader& reader) {
    std::string error;
    CHECK_FALSE(
        reader.read_line_until(std::chrono::steady_clock::now() + std::chrono::seconds(2), error)
            .has_value());
    CHECK(error.empty());
}

void send_control_token(const std::string& control_path, std::string_view token) {
    const int fd = net::socket_cloexec(AF_UNIX, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, control_path.c_str(), sizeof(address.sun_path) - 1);
    REQUIRE(::sendto(fd, token.data(), token.size(), 0, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == static_cast<ssize_t>(token.size()));
    ::close(fd);
}

class ChildProtocolDaemon {
  public:
    explicit ChildProtocolDaemon(const ChildDaemonOptions& options = {}) {
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

    [[nodiscard]] pid_t pid() const {
        return pid_;
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
            exit_child_startup_failure(ready_fd, 2);
        }
        if (options.malformed_identity) {
            constexpr std::string_view malformed = "malformed\n";
            if (::ftruncate(lock_fd, 0) != 0 ||
                ::pwrite(lock_fd, malformed.data(), malformed.size(), 0) !=
                    static_cast<ssize_t>(malformed.size())) {
                exit_child_startup_failure(ready_fd, 3);
            }
        }
        bool ready_reported = false;
        if (options.report_ready_before_endpoints) {
            report_child_ready_or_exit(ready_fd);
            ::close(ready_fd);
            ready_reported = true;
            std::this_thread::sleep_for(options.startup_delay);
        }

        daemon::DaemonContext context;
        context.account = "main";
        context.binary_version = options.binary_version;
        context.protocol_version = options.protocol_version;
        context.tdlib_version = "test";
        context.socket_path = *socket_path;
        std::atomic<int> dispatches{0};
        daemon::Dispatcher dispatcher(
            [&dispatches](daemon::testing::RequestObservationStage stage) {
                if (stage == daemon::testing::RequestObservationStage::DispatcherLookup) {
                    dispatches.fetch_add(1, std::memory_order_relaxed);
                }
            });
        std::string server_token = identity.control_token;
        if (options.ignore_control_token) {
            server_token.front() = server_token.front() == '0' ? '1' : '0';
        }
        daemon::Server server({"main", *socket_path, context.binary_version,
                               context.protocol_version, *control_socket_path, server_token},
                              dispatcher);
        context.request_shutdown = [&server] { server.request_stop(); };
        daemon::register_commands(dispatcher, context);
        if (!server.start(error)) {
            if (!ready_reported) {
                exit_child_startup_failure(ready_fd, 4);
            }
            ::_exit(4);
        }
        if (!ready_reported) {
            report_child_ready_or_exit(ready_fd);
            ::close(ready_fd);
        }
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
        ::_exit(!options.require_no_dispatch || dispatches.load(std::memory_order_relaxed) == 0
                    ? 0
                    : 5);
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

enum class RawProtocolDaemonMode { MismatchOwner, Replacement };

class ChildLegacyReplacementDaemon {
  public:
    ChildLegacyReplacementDaemon(int protocol_version, std::string binary_version,
                                 RawProtocolDaemonMode mode = RawProtocolDaemonMode::Replacement)
        : protocol_version_(protocol_version), binary_version_(std::move(binary_version)),
          mode_(mode) {
        prepare_account_layout();
        std::array<int, 2> ready_fds{-1, -1};
        REQUIRE(::pipe(ready_fds.data()) == 0);
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::signal(SIGTERM, SIG_DFL);
            ::signal(SIGPIPE, SIG_IGN);
            ::close(ready_fds[0]);
            run_child(ready_fds[1], protocol_version_, binary_version_, mode_);
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

    ChildLegacyReplacementDaemon(const ChildLegacyReplacementDaemon&) = delete;
    ChildLegacyReplacementDaemon& operator=(const ChildLegacyReplacementDaemon&) = delete;
    ChildLegacyReplacementDaemon(ChildLegacyReplacementDaemon&&) = delete;
    ChildLegacyReplacementDaemon& operator=(ChildLegacyReplacementDaemon&&) = delete;

    ~ChildLegacyReplacementDaemon() {
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
    static bool read_json(proto::FrameReader& reader, json& document) {
        std::string error;
        const auto line = reader.read_line_until(
            std::chrono::steady_clock::now() + std::chrono::seconds(3), error);
        if (!line) {
            return false;
        }
        document = json::parse(*line, nullptr, false);
        return !document.is_discarded();
    }

    [[noreturn]] static void run_child(int ready_fd, int protocol_version,
                                       const std::string& binary_version,
                                       RawProtocolDaemonMode mode) {
        const auto env = paths::real_environment();
        std::string error;
        const auto socket_path = paths::socket_path("main", env, error);
        const auto control_path = paths::control_socket_path("main", env, error);
        const std::string state_dir = paths::account_state_dir("main", env);
        daemon_lock::Identity identity;
        const int lock_fd = daemon_lock::acquire(state_dir + "/daemon.lock", identity, error);
        if (lock_fd < 0 || !socket_path || !control_path) {
            exit_child_startup_failure(ready_fd, 2);
        }
        auto main = bind_private_endpoint(*socket_path, SOCK_STREAM, error);
        auto control = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
        if (!main || !control) {
            exit_child_startup_failure(ready_fd, 3);
        }
        report_child_ready_or_exit(ready_fd);
        ::close(ready_fd);

        int connection = -1;
        do {
            connection = net::accept_cloexec(main->fd);
        } while (connection < 0 && errno == EINTR);
        bool valid = connection >= 0;
        if (valid) {
            valid = proto::write_frame(connection, proto::Hello{binary_version, protocol_version},
                                       error);
        }
        proto::FrameReader reader(connection);
        json client_hello;
        if (valid) {
            const int expected_protocol = mode == RawProtocolDaemonMode::MismatchOwner
                                              ? proto::kProtocolVersion
                                              : protocol_version;
            const std::string_view expected_binary = mode == RawProtocolDaemonMode::MismatchOwner
                                                         ? std::string_view{kVersion}
                                                         : std::string_view{binary_version};
            valid =
                read_json(reader, client_hello) &&
                exact_json_fields(client_hello, {"type", "binary_version", "protocol_version"}) &&
                client_hello.value("type", "") == "hello" &&
                client_hello.value("binary_version", "") == expected_binary &&
                client_hello.value("protocol_version", 0) == expected_protocol;
        }
        std::array<char, 256> token{};
        ssize_t count = -1;
        if (mode == RawProtocolDaemonMode::Replacement) {
            json original;
            if (valid) {
                valid = read_json(reader, original) &&
                        valid_raw_request_dialect(original, protocol_version, 61, {"version"});
            }
            if (valid) {
                valid = proto::write_frame(connection,
                                           proto::Result{61,
                                                         {{"version", binary_version},
                                                          {"protocol", protocol_version},
                                                          {"tdlib", "legacy-test"}}},
                                           error);
            }
            do {
                count = ::recv(control->fd, token.data(), token.size(), 0);
            } while (count < 0 && errno == EINTR);
        } else {
            bool stopped = false;
            while (!stopped) {
                std::array<pollfd, 2> descriptors{
                    {{connection, POLLIN, 0}, {control->fd, POLLIN, 0}}};
                const int polled = ::poll(descriptors.data(), descriptors.size(), 3000);
                if (polled < 0 && errno == EINTR) {
                    continue;
                }
                if (polled <= 0) {
                    valid = false;
                    break;
                }
                if ((descriptors[0].revents & POLLIN) != 0) {
                    std::array<char, 256> unexpected{};
                    const auto received =
                        ::recv(connection, unexpected.data(), unexpected.size(), MSG_DONTWAIT);
                    if (received > 0) {
                        valid = false;
                    }
                }
                if ((descriptors[1].revents & POLLIN) != 0) {
                    count = ::recv(control->fd, token.data(), token.size(), 0);
                    stopped = true;
                }
            }
        }
        valid =
            valid && count == static_cast<ssize_t>(identity.control_token.size()) &&
            std::equal(identity.control_token.begin(), identity.control_token.end(), token.begin());

        if (connection >= 0) {
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
        }
        ::close(main->fd);
        ::close(control->fd);
        paths::unlink_socket_endpoint_if_same(*socket_path, main->identity);
        paths::unlink_socket_endpoint_if_same(*control_path, control->identity);
        ::close(lock_fd);
        ::_exit(valid ? 0 : 5);
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

    int protocol_version_ = 0;
    std::string binary_version_;
    RawProtocolDaemonMode mode_ = RawProtocolDaemonMode::Replacement;
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
        return request != nullptr && request->account == "main" &&
               request->command == std::vector<std::string>{"daemon", "stop"} &&
               !request->context.idempotency_key.has_value();
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
            exit_child_startup_failure(ready_fd, 2);
        }
        auto main = bind_private_endpoint(*socket_path, SOCK_STREAM, error);
        auto control = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
        if (!main || !control) {
            exit_child_startup_failure(ready_fd, 3);
        }
        report_child_ready_or_exit(ready_fd);
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

class ChildCompatibleStopRaceDaemon {
  public:
    ChildCompatibleStopRaceDaemon() {
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

    ChildCompatibleStopRaceDaemon(const ChildCompatibleStopRaceDaemon&) = delete;
    ChildCompatibleStopRaceDaemon& operator=(const ChildCompatibleStopRaceDaemon&) = delete;
    ChildCompatibleStopRaceDaemon(ChildCompatibleStopRaceDaemon&&) = delete;
    ChildCompatibleStopRaceDaemon& operator=(ChildCompatibleStopRaceDaemon&&) = delete;

    ~ChildCompatibleStopRaceDaemon() {
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
    static std::optional<proto::Request> read_stop_request(int fd, proto::IoDeadline deadline) {
        proto::FrameReader reader(fd);
        std::string error;
        const auto hello_line = reader.read_line_until(deadline, error);
        if (!hello_line) {
            return std::nullopt;
        }
        std::string parse_error;
        const auto hello = proto::parse(*hello_line, parse_error);
        if (!hello || std::get_if<proto::Hello>(&*hello) == nullptr) {
            return std::nullopt;
        }
        const auto request_line = reader.read_line_until(deadline, error);
        if (!request_line) {
            return std::nullopt;
        }
        const auto frame = proto::parse(*request_line, parse_error);
        const auto* request = frame ? std::get_if<proto::Request>(&*frame) : nullptr;
        if (request == nullptr || request->command != std::vector<std::string>{"daemon", "stop"}) {
            return std::nullopt;
        }
        return *request;
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
            exit_child_startup_failure(ready_fd, 2);
        }
        auto main = bind_private_endpoint(*socket_path, SOCK_STREAM, error);
        auto control = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
        if (!main || !control) {
            exit_child_startup_failure(ready_fd, 3);
        }
        report_child_ready_or_exit(ready_fd);
        ::close(ready_fd);

        std::array<int, 2> connections{-1, -1};
        for (int& connection : connections) {
            do {
                connection = net::accept_cloexec(main->fd);
            } while (connection < 0 && errno == EINTR);
            if (connection < 0 ||
                !proto::write_frame(connection, proto::Hello{kVersion, proto::kProtocolVersion},
                                    error)) {
                ::_exit(4);
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        const auto winner = read_stop_request(connections[0], deadline);
        const auto loser = read_stop_request(connections[1], deadline);
        if (!winner || !loser ||
            !proto::write_frame(connections[0], proto::Result{winner->id, json{{"stopping", true}}},
                                error) ||
            !proto::write_frame(connections[1],
                                proto::Error{loser->id,
                                             "DAEMON_SHUTDOWN",
                                             "daemon is shutting down",
                                             {{"reason", "daemon_shutdown"}},
                                             kGeneric},
                                error)) {
            ::_exit(5);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    if (::flock(fd, LOCK_UN) != 0) {
        error = "cannot publish frozen fixture identity";
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

enum class BootstrapHelloMode { Incompatible, Stall, Shutdown };

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

    [[nodiscard]] pid_t pid() const {
        return pid_;
    }

    [[nodiscard]] bool try_request_external_stop() const {
        return pid_ > 0 && ::kill(pid_, SIGTERM) == 0;
    }

    void request_external_stop() const {
        static_cast<void>(try_request_external_stop());
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
            exit_child_startup_failure(ready_fd, 2);
        }
        if (options.malformed_identity) {
            constexpr std::string_view malformed = "malformed\n";
            if (::ftruncate(lock_fd, 0) != 0 ||
                ::pwrite(lock_fd, malformed.data(), malformed.size(), 0) !=
                    static_cast<ssize_t>(malformed.size())) {
                exit_child_startup_failure(ready_fd, 3);
            }
        }
        auto main = bind_private_endpoint(*socket_path, SOCK_STREAM, error);
        auto control = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
        if (!main || !control) {
            exit_child_startup_failure(ready_fd, 4);
        }
        const int control_flags = ::fcntl(control->fd, F_GETFL);
        if (control_flags < 0 || ::fcntl(control->fd, F_SETFL, control_flags | O_NONBLOCK) != 0) {
            exit_child_startup_failure(ready_fd, 5);
        }
        report_child_ready_or_exit(ready_fd);
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
                    } else if (options.hello_mode == BootstrapHelloMode::Shutdown) {
                        proto::FrameReader reader(connection);
                        std::string read_error;
                        static_cast<void>(reader.read_line_until(std::chrono::steady_clock::now() +
                                                                     std::chrono::seconds(1),
                                                                 read_error));
                        stopping = true;
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

class DelayedExternalStop {
  public:
    DelayedExternalStop(const ChildBootstrapDaemon& daemon, std::chrono::milliseconds delay) {
        std::array<int, 2> start_fds{-1, -1};
        REQUIRE(::pipe(start_fds.data()) == 0);
        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(start_fds[0]);
            ::close(start_fds[1]);
        }
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::close(start_fds[1]);
            run_child(start_fds[0], daemon, delay);
        }
        ::close(start_fds[0]);
        start_fd_ = start_fds[1];
    }

    DelayedExternalStop(const DelayedExternalStop&) = delete;
    DelayedExternalStop& operator=(const DelayedExternalStop&) = delete;
    DelayedExternalStop(DelayedExternalStop&&) = delete;
    DelayedExternalStop& operator=(DelayedExternalStop&&) = delete;

    ~DelayedExternalStop() {
        close_start_fd();
        if (wait_for_exit(std::chrono::seconds(2)) >= 0 || pid_ <= 0) {
            return;
        }
        static_cast<void>(::kill(pid_, SIGKILL));
        reap_blocking();
    }

    [[nodiscard]] bool start() {
        if (start_fd_ < 0) {
            return false;
        }
        constexpr char start = '1';
        ssize_t count = -1;
        do {
            count = ::write(start_fd_, &start, 1);
        } while (count < 0 && errno == EINTR);
        close_start_fd();
        return count == 1;
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
    [[noreturn]] static void run_child(int start_fd, const ChildBootstrapDaemon& daemon,
                                       std::chrono::milliseconds delay) {
        char start = 0;
        ssize_t count = -1;
        do {
            count = ::read(start_fd, &start, 1);
        } while (count < 0 && errno == EINTR);
        ::close(start_fd);
        if (count != 1 || start != '1') {
            ::_exit(2);
        }

        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const int timeout = std::max(1, static_cast<int>(remaining.count()));
            if (::poll(nullptr, 0, timeout) < 0 && errno != EINTR) {
                ::_exit(3);
            }
        }
        ::_exit(daemon.try_request_external_stop() ? 0 : 4);
    }

    void close_start_fd() {
        if (start_fd_ >= 0) {
            ::close(start_fd_);
            start_fd_ = -1;
        }
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

    pid_t pid_ = -1;
    int start_fd_ = -1;
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
    AsyncCliProcess(std::string output_path, std::string error_path,
                    std::string daemon_operation = {})
        : output_path_(std::move(output_path)), error_path_(std::move(error_path)),
          daemon_operation_(std::move(daemon_operation)), pid_(::fork()) {
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
            if (daemon_operation_.empty()) {
                ::execl(TGCLI_TEST_BINARY, "tgcli", "--json", "version",
                        static_cast<char*>(nullptr));
            } else {
                ::execl(TGCLI_TEST_BINARY, "tgcli", "--json", "--timeout", "5", "daemon",
                        daemon_operation_.c_str(), static_cast<char*>(nullptr));
            }
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
    std::string daemon_operation_;
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

TEST_CASE("only M1 logout and removal dry-runs suppress daemon autospawn",
          "[cli][dry-run][protocol-v3]") {
    CHECK_FALSE(cli::uses_client_local_dry_run({"logout"}, false));
    CHECK(cli::uses_client_local_dry_run({"logout"}, true));
    CHECK(cli::uses_client_local_dry_run({"account", "remove"}, true));
    CHECK_FALSE(cli::uses_client_local_dry_run({"version"}, true));
    for (const auto& identity : proto::m3_operation_identities()) {
        INFO(identity.command_path);
        CHECK_FALSE(cli::uses_client_local_dry_run(command_parts(identity.command_path), true));
    }
}

TEST_CASE("invalid TGCLI_TEST_DC is a narrow process-level usage error", "[cli][process]") {
    const IsolatedEnv env;
    const std::string output_path = env.root() + "/invalid-test-dc.out";
    const std::string error_path = env.root() + "/invalid-test-dc.err";
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        const int output = ::open(output_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        const int errors = ::open(error_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (output < 0 || errors < 0 || ::setenv("TGCLI_TEST_DC", "true", 1) != 0) {
            ::_exit(126);
        }
        ::dup2(output, STDOUT_FILENO);
        ::dup2(errors, STDERR_FILENO);
        ::close(output);
        ::close(errors);
        ::execl(TGCLI_TEST_BINARY, "tgcli", "--json", "--no-daemon", "version",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == kUsage);

    const auto read_file = [](const std::string& file_path) {
        const std::ifstream input(file_path);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    };
    CHECK(read_file(output_path).empty());
    const auto error = json::parse(read_file(error_path));
    CHECK(error == json{{"error",
                         {{"code", "USAGE"},
                          {"message", "TGCLI_TEST_DC must be exactly 1 when set"},
                          {"details",
                           {{"argument", "TGCLI_TEST_DC"}, {"reason", "invalid_environment"}}}}}});
}

TEST_CASE("legacy bot token argv is rejected without echoing or routing it", "[cli][process]") {
    const IsolatedEnv env;
    const std::string output_path = env.root() + "/bot-token.out";
    const std::string error_path = env.root() + "/bot-token.err";
    constexpr const char* sensitive_token = "123456:must-not-be-rendered";
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        const int output = ::open(output_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        const int errors = ::open(error_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (output < 0 || errors < 0) {
            ::_exit(126);
        }
        ::dup2(output, STDOUT_FILENO);
        ::dup2(errors, STDERR_FILENO);
        ::close(output);
        ::close(errors);
        ::execl(TGCLI_TEST_BINARY, "tgcli", "--json", "login", "--bot-token", sensitive_token,
                "--bogus", static_cast<char*>(nullptr));
        ::_exit(127);
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == kUsage);

    const auto read_file = [](const std::string& file_path) {
        const std::ifstream input(file_path);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    };
    CHECK(read_file(output_path).empty());
    const auto rendered_error = read_file(error_path);
    CHECK(rendered_error.find(sensitive_token) == std::string::npos);
    CHECK(json::parse(rendered_error) ==
          json{{"error",
                {{"code", "INSECURE_SECRET_INPUT"},
                 {"message", "bot tokens are not accepted on the command line"},
                 {"details", {{"argument", "--bot-token"}, {"replacement", "--bot"}}}}}});
}

TEST_CASE("daemon lifecycle parser exposes status stop restart and rejects no-daemon",
          "[cli][daemon-control][process]") {
    const IsolatedEnv env;
    const auto help = run_binary_captured({"daemon", "--help"}, env, "daemon-help");
    REQUIRE(help.exit_code == kOk);
    const std::string help_text = help.out + help.err;
    CHECK(help_text.find("status") != std::string::npos);
    CHECK(help_text.find("stop") != std::string::npos);
    CHECK(help_text.find("restart") != std::string::npos);
    CHECK(help_text.find("run") != std::string::npos);

    for (const auto* operation : {"status", "stop", "restart"}) {
        const auto outcome = run_binary_captured({"--json", "--no-daemon", "daemon", operation},
                                                 env, std::string("no-daemon-") + operation);
        INFO(operation);
        CHECK(outcome.exit_code == kUsage);
        CHECK(outcome.out.empty());
        CHECK(json::parse(outcome.err) ==
              json{{"error",
                    {{"code", "USAGE"},
                     {"message", "daemon lifecycle commands do not support --no-daemon"},
                     {"details", json::object()}}}});
        CHECK_THAT(json::parse(outcome.err), test::matches_json_schema("daemon.error.schema.json"));
    }
}

TEST_CASE("Saved Messages parser exposes its nested surface and rejects invalid input locally",
          "[cli][saved][process][schema]") {
    const IsolatedEnv env;
    const auto root_help = run_binary_captured({"--help"}, env, "saved-root-help");
    REQUIRE(root_help.exit_code == kOk);
    CHECK((root_help.out + root_help.err).find("--cursor") != std::string::npos);

    const auto saved_help = run_binary_captured({"saved", "--help"}, env, "saved-help");
    REQUIRE(saved_help.exit_code == kOk);
    const auto saved_help_text = saved_help.out + saved_help.err;
    CHECK(saved_help_text.find("tags") != std::string::npos);
    CHECK(saved_help_text.find("search") != std::string::npos);

    const auto search_help =
        run_binary_captured({"saved", "search", "--help"}, env, "saved-search-help");
    REQUIRE(search_help.exit_code == kOk);
    const auto search_help_text = search_help.out + search_help.err;
    CHECK(search_help_text.find("query") != std::string::npos);
    CHECK(search_help_text.find("--tag") != std::string::npos);
    CHECK(search_help_text.find("-n") != std::string::npos);

    struct InvalidCase {
        std::string stem;
        std::vector<std::string> arguments;
        std::optional<std::string> argument;
        std::string reason;
    };
    const std::vector<InvalidCase> cases{
        {"saved-missing-tag", {"saved", "search"}, std::nullopt, "missing_argument"},
        {"saved-custom-zero",
         {"saved", "search", "--tag", "custom:0"},
         "--tag",
         "invalid_argument"},
        {"saved-custom-leading-zero",
         {"saved", "search", "--tag", "custom:01"},
         "--tag",
         "invalid_argument"},
        {"saved-invalid-utf8-tag",
         {"saved", "search", "--tag", std::string("\xF0\x28\x8C\x28", 4)},
         "--tag",
         "invalid_argument"},
        {"saved-invalid-utf8-query",
         {"saved", "search", std::string("\xED\xA0\x80", 3), "--tag", "🧪"},
         "query",
         "invalid_argument"},
        {"saved-limit-zero",
         {"saved", "search", "--tag", "🧪", "-n", "0"},
         "-n",
         "invalid_argument"},
        {"saved-limit-high",
         {"saved", "search", "--tag", "🧪", "-n", "101"},
         "-n",
         "invalid_argument"},
        {"saved-limit-noninteger",
         {"saved", "search", "--tag", "🧪", "-n", "twenty"},
         std::nullopt,
         "invalid_argument"},
        {"saved-tags-cursor",
         {"--cursor", "not-a-cursor", "saved", "tags"},
         "--cursor",
         "invalid_argument"},
        {"saved-cursor-limit",
         {"--cursor", "not-a-cursor", "saved", "search", "-n", "20"},
         "-n",
         "invalid_argument"},
    };
    for (const auto& test_case : cases) {
        const auto outcome = run_binary_captured(test_case.arguments, env, test_case.stem);
        INFO(test_case.stem);
        CHECK(outcome.exit_code == kUsage);
        CHECK(outcome.out.empty());
        const auto error = json::parse(outcome.err);
        CHECK(error["error"]["code"] == "USAGE");
        CHECK(error["error"]["details"]["reason"] == test_case.reason);
        if (test_case.argument) {
            CHECK(error["error"]["details"]["argument"] == *test_case.argument);
        }
        CHECK_THAT(error, test::matches_json_schema("saved.error.schema.json"));
    }
    CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli"));
}

TEST_CASE("Saved Messages no-daemon fake boundary preserves stdout and stderr discipline",
          "[cli][saved][fake-boundary][schema]") {
    const IsolatedEnv env;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    REQUIRE(scripted->wait_for_sent(1));
    REQUIRE(scripted->clients().size() == 1);
    const auto td_client = scripted->clients().front();
    scripted->push_response(td_client, 1, {}, core::AuthStateData{core::AuthState::Ready});
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.auth_state()->auth_sequence != 1 &&
           std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client.auth_state()->auth_sequence == 1);

    cli::RunOptions options;
    options.account = "main";
    options.json = true;
    options.no_daemon = true;
    options.in_process_td_client = &client;

    proto::Request tags("main");
    tags.id = 1;
    tags.command = {"saved", "tags"};
    tags.args = json::object();
    tags.context.json = true;
    tags.context.cwd = "/";
    auto tags_pending =
        std::async(std::launch::async, [&] { return run_request_captured(tags, options, env); });
    REQUIRE(scripted->wait_for_sent(2));
    auto sent = scripted->sent_functions();
    REQUIRE(sent.back().function.kind() == core::TdFunctionKind::GetMe);
    scripted->push_response(td_client, sent.back().query_id,
                            core::TdValue::from(core::TdUserSummary{.id = 42,
                                                                    .first_name = "Ada",
                                                                    .last_name = "",
                                                                    .usernames = {},
                                                                    .phone_number = "12025550123",
                                                                    .is_bot = false,
                                                                    .is_premium = true}));
    REQUIRE(scripted->wait_for_sent(3));
    sent = scripted->sent_functions();
    REQUIRE(sent.back().function.kind() == core::TdFunctionKind::GetSavedMessagesTags);
    scripted->push_response(td_client, sent.back().query_id,
                            core::TdValue::from(core::TdSavedMessagesTags{
                                .tags = {{.tag = {.kind = core::TdReactionKind::Emoji,
                                                  .emoji = "🧪",
                                                  .custom_emoji_id = 0,
                                                  .tdlib_type_id = -1942084920},
                                          .label = "experiments",
                                          .count = 7}}}));
    const auto tags_outcome = tags_pending.get();
    CHECK(tags_outcome.exit_code == kOk);
    CHECK(tags_outcome.err.empty());
    CHECK(
        tags_outcome.out ==
        json{{"items", json::array({json{{"tag", "🧪"}, {"label", "experiments"}, {"count", 7}}})},
             {"next", nullptr}}
                .dump() +
            "\n");

    proto::Request search("main");
    search.id = 2;
    search.command = {"saved", "search"};
    search.args = {{"query", nullptr}, {"tag", "🧪"}, {"limit", nullptr}, {"cursor", nullptr}};
    search.context.json = true;
    search.context.cwd = "/";
    auto bot_pending =
        std::async(std::launch::async, [&] { return run_request_captured(search, options, env); });
    REQUIRE(scripted->wait_for_sent(4));
    sent = scripted->sent_functions();
    REQUIRE(sent.back().function.kind() == core::TdFunctionKind::GetMe);
    scripted->push_response(td_client, sent.back().query_id,
                            core::TdValue::from(core::TdUserSummary{.id = 84,
                                                                    .first_name = "Bot",
                                                                    .last_name = "",
                                                                    .usernames = {},
                                                                    .phone_number = "",
                                                                    .is_bot = true,
                                                                    .is_premium = false}));
    const auto bot_outcome = bot_pending.get();
    CHECK(bot_outcome.exit_code == kUsage);
    CHECK(bot_outcome.out.empty());
    CHECK(json::parse(bot_outcome.err) ==
          json{{"error",
                {{"code", "BOT_UNSUPPORTED"},
                 {"message", "saved commands require a user account"},
                 {"details", json::object()}}}});
    CHECK(scripted->sent_functions().size() == 4);
}

TEST_CASE("verbose is client-owned diagnostics and leaves command data on stdout",
          "[cli][process][logging]") {
    const IsolatedEnv env;
    const auto help = run_binary_captured({"--help"}, env, "verbose-help");
    REQUIRE(help.exit_code == kOk);
    const auto help_text = help.out + help.err;
    CHECK(help_text.find("-v") != std::string::npos);
    CHECK(help_text.find("--verbose") != std::string::npos);

    const auto outcome =
        run_binary_captured({"-v", "--json", "daemon", "status"}, env, "verbose-status");
    REQUIRE(outcome.exit_code == kOk);
    const auto result = json::parse(outcome.out);
    CHECK(result["running"] == false);
    CHECK(json::parse(outcome.err) ==
          json{{"diagnostic", {{"account", "main"}, {"transport", "daemon"}}}});
    CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli/accounts/main/tdlib.log"));
    CHECK(outcome.err.find("setLogVerbosityLevel") == std::string::npos);
}

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

TEST_CASE("public client rejects route and frame disagreement before surface mutation",
          "[cli][account][routing]") {
    const IsolatedEnv env;
    cli::RunOptions options;
    options.account = "main";
    options.json = true;
    options.no_daemon = true;
    options.daemon_executable = "/definitely/not/a/tgcli/binary";
    proto::Request request("work");
    request.id = 1;
    request.command = {"version"};
    request.context.json = true;
    request.context.cwd = "/";

    const auto result = run_request_captured(request, options, env);
    CHECK(result.exit_code == kNotFound);
    CHECK(result.out.empty());
    CHECK(json::parse(result.err) ==
          json{{"error",
                {{"code", "ACCOUNT_MISMATCH"},
                 {"message", "request account does not match the selected route"},
                 {"details", {{"requested_account", "work"}, {"daemon_account", "main"}}}}}});
    CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli"));
    CHECK_FALSE(std::filesystem::exists(env.root() + "/accounts"));
    CHECK_FALSE(std::filesystem::exists(env.root() + "/config.toml"));
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
            daemon::Server server({"main", *socket_path, kVersion, proto::kProtocolVersion, {}, {}},
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

TEST_CASE("absent daemon status and stop are read-only and never spawn",
          "[cli][daemon-control][schema]") {
    const IsolatedEnv env;
    const auto real_env = paths::real_environment();
    const std::string runtime_dir = paths::runtime_dir(real_env);
    const std::string state_dir = paths::account_state_dir("main", real_env);

    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    const auto status = run_captured({"daemon", "status"}, options, env);
    REQUIRE(status.exit_code == kOk);
    CHECK(status.err.empty());
    const auto status_data = json::parse(status.out);
    CHECK(status_data["running"] == false);
    CHECK_THAT(status_data, test::matches_json_schema("daemon-status.result.schema.json"));
    CHECK_FALSE(std::filesystem::exists(runtime_dir));
    CHECK_FALSE(std::filesystem::exists(state_dir));

    const auto stop = run_captured({"daemon", "stop"}, options, env);
    CHECK(stop.exit_code == kNotFound);
    CHECK(stop.out.empty());
    const auto stop_error = json::parse(stop.err);
    CHECK(stop_error["error"]["code"] == "DAEMON_NOT_RUNNING");
    CHECK(stop_error["error"]["details"] ==
          json{{"account", "main"}, {"socket", runtime_dir + "/main.sock"}});
    CHECK_THAT(stop_error, test::matches_json_schema("daemon.error.schema.json"));
    CHECK_FALSE(std::filesystem::exists(runtime_dir));
    CHECK_FALSE(std::filesystem::exists(state_dir));
}

TEST_CASE("unlocked daemon identity records are validated before absent classification",
          "[cli][daemon-control][lock-safety]") {
    SECTION("valid stale identity remains an absent read-only surface") {
        const IsolatedEnv env;
        const std::string lock_path = prepare_state_layout() + "/daemon.lock";
        daemon_lock::Identity identity;
        std::string error;
        const int lock_fd = daemon_lock::acquire(lock_path, identity, error);
        REQUIRE(lock_fd >= 0);
        REQUIRE(::close(lock_fd) == 0);
        const std::string frozen_record = read_file_bytes(lock_path);

        cli::RunOptions options;
        options.json = true;
        options.auto_spawn = false;
        const auto status = run_captured({"daemon", "status"}, options, env);

        REQUIRE(status.exit_code == kOk);
        CHECK(json::parse(status.out)["running"] == false);
        CHECK(read_file_bytes(lock_path) == frozen_record);
        CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli/main.sock"));
        CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli/main.ctl"));
    }

    SECTION("malformed grammar fails every lifecycle operation without mutation or spawn") {
        for (const auto* operation : {"status", "stop", "restart"}) {
            const IsolatedEnv env;
            const std::string lock_path = prepare_state_layout() + "/daemon.lock";
            constexpr std::string_view malformed = "malformed\n";
            write_private_file(lock_path, malformed);

            cli::RunOptions options;
            options.json = true;
            options.daemon_executable = TGCLI_TEST_BINARY;
            options.restart_timeout = std::chrono::milliseconds(300);
            const auto result = run_captured({"daemon", operation}, options, env);

            INFO(operation);
            REQUIRE(result.exit_code == kGeneric);
            CHECK(result.out.empty());
            const auto rendered = json::parse(result.err);
            CHECK(rendered["error"]["details"] == json{{"account", "main"},
                                                       {"operation", operation},
                                                       {"reason", "surface_invalid"}});
            CHECK(read_file_bytes(lock_path) == malformed);
            CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli/main.sock"));
            CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli/main.ctl"));
        }
    }

    SECTION("unsafe mode and symlink lock fail closed") {
        for (const bool symlink : {false, true}) {
            const IsolatedEnv env;
            const std::string state_dir = prepare_state_layout();
            const std::string lock_path = state_dir + "/daemon.lock";
            constexpr std::string_view malformed = "malformed\n";
            if (symlink) {
                const std::string target = state_dir + "/target.lock";
                write_private_file(target, malformed);
                REQUIRE(::symlink(target.c_str(), lock_path.c_str()) == 0);
            } else {
                write_private_file(lock_path, malformed, 0640);
            }

            cli::RunOptions options;
            options.json = true;
            options.daemon_executable = TGCLI_TEST_BINARY;
            const auto result = run_captured({"daemon", "restart"}, options, env);

            INFO(symlink);
            REQUIRE(result.exit_code == kGeneric);
            CHECK(json::parse(result.err)["error"]["details"]["reason"] == "surface_invalid");
            CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli/main.sock"));
            CHECK_FALSE(std::filesystem::exists(env.root() + "/tgcli/main.ctl"));
        }
    }
}

TEST_CASE("daemon status reports verified v1 and v2 owner facts without replacement",
          "[cli][daemon-control][protocol-v1][protocol-v2][schema]") {
    for (const int old_protocol : {1, 2}) {
        DYNAMIC_SECTION("protocol " << old_protocol) {
            const IsolatedEnv env;
            const ChildProtocolDaemon old_daemon(
                {.protocol_version = old_protocol, .binary_version = "old-test-binary"});

            cli::RunOptions options;
            options.json = true;
            options.auto_spawn = false;
            options.restart_timeout = std::chrono::seconds(2);
            const auto status = run_captured({"daemon", "status"}, options, env);

            REQUIRE(status.exit_code == kOk);
            CHECK(status.err.empty());
            const auto data = json::parse(status.out);
            CHECK(data["running"] == true);
            CHECK(data["pid"] == old_daemon.pid());
            CHECK(data["version"] == "old-test-binary");
            CHECK(data["protocol"] == old_protocol);
            CHECK_THAT(data, test::matches_json_schema("daemon-status.result.schema.json"));
            CHECK(old_daemon.running());
        }
    }
}

TEST_CASE("daemon status fails closed on an unparseable Hello without stopping the owner",
          "[cli][daemon-control][schema]") {
    const IsolatedEnv env;
    const ChildBootstrapDaemon old_daemon;

    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::seconds(2);
    const auto status = run_captured({"daemon", "status"}, options, env);

    CHECK(status.exit_code == kGeneric);
    CHECK(status.out.empty());
    const auto error = json::parse(status.err);
    CHECK(error["error"]["code"] == "DAEMON_CONTROL_FAILED");
    CHECK(error["error"]["details"] ==
          json{{"account", "main"}, {"operation", "status"}, {"reason", "handshake_failed"}});
    CHECK_THAT(error, test::matches_json_schema("daemon.error.schema.json"));
    CHECK(old_daemon.running());
}

TEST_CASE("partial daemon surfaces fail closed without mutation",
          "[cli][daemon-control][socket-safety]") {
    const IsolatedEnv env;
    prepare_account_layout();
    const auto real_env = paths::real_environment();
    std::string error;
    const auto socket_path = paths::socket_path("main", real_env, error);
    REQUIRE(socket_path.has_value());
    const auto main = bind_private_endpoint(*socket_path, SOCK_STREAM, error);
    REQUIRE(main.has_value());

    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    for (const auto* operation : {"status", "stop", "restart"}) {
        const auto result = run_captured({"daemon", operation}, options, env);
        INFO(operation);
        CHECK(result.exit_code == kGeneric);
        CHECK(result.out.empty());
        const auto rendered = json::parse(result.err);
        CHECK(rendered["error"]["details"] ==
              json{{"account", "main"}, {"operation", operation}, {"reason", "surface_invalid"}});
        const auto unchanged = paths::inspect_socket_endpoint(*socket_path, real_env.uid, error);
        REQUIRE(unchanged.has_value());
        CHECK(*unchanged == main->identity);
    }
    ::close(main->fd);
}

TEST_CASE("starting runtime directory validation rejects unsafe existing paths",
          "[cli][daemon-control][socket-safety]") {
    const IsolatedEnv env;
    const std::string runtime_dir = env.root() + "/runtime-matrix";
    std::string error;
    CHECK(cli::detail::inspect_runtime_directory(runtime_dir, getuid(), error) ==
          cli::detail::RuntimeDirectoryState::Absent);

    std::filesystem::create_directory(runtime_dir);
    std::filesystem::permissions(runtime_dir, std::filesystem::perms::owner_all);
    CHECK(cli::detail::inspect_runtime_directory(runtime_dir, getuid(), error) ==
          cli::detail::RuntimeDirectoryState::Valid);
    CHECK(cli::detail::inspect_runtime_directory(runtime_dir, getuid() + 1, error) ==
          cli::detail::RuntimeDirectoryState::Invalid);
    CHECK(error.find("owned by uid") != std::string::npos);

    std::filesystem::permissions(runtime_dir, std::filesystem::perms::owner_all |
                                                  std::filesystem::perms::group_read);
    CHECK(cli::detail::inspect_runtime_directory(runtime_dir, getuid(), error) ==
          cli::detail::RuntimeDirectoryState::Invalid);
    CHECK(error.find("group/other") != std::string::npos);

    std::filesystem::remove(runtime_dir);
    const std::string target = env.root() + "/runtime-target";
    std::filesystem::create_directory(target);
    std::filesystem::permissions(target, std::filesystem::perms::owner_all);
    REQUIRE(::symlink(target.c_str(), runtime_dir.c_str()) == 0);
    CHECK(cli::detail::inspect_runtime_directory(runtime_dir, getuid(), error) ==
          cli::detail::RuntimeDirectoryState::Invalid);
    CHECK(error.find("not a directory") != std::string::npos);
}

TEST_CASE("held owner without endpoints rejects unsafe runtime instead of joining startup",
          "[cli][daemon-control][socket-safety][process]") {
    for (const bool symlink : {false, true}) {
        const IsolatedEnv env;
        const std::string state_base = env.root() + "/state";
        const std::string runtime_base = env.root() + "/runtime";
        std::filesystem::create_directory(state_base);
        std::filesystem::create_directory(runtime_base);
        std::filesystem::permissions(state_base, std::filesystem::perms::owner_all);
        std::filesystem::permissions(runtime_base, std::filesystem::perms::owner_all);
        REQUIRE(::setenv("XDG_STATE_HOME", state_base.c_str(), 1) == 0);
        REQUIRE(::setenv("XDG_RUNTIME_DIR", runtime_base.c_str(), 1) == 0);

        ChildDaemonOptions child_options;
        child_options.protocol_version = proto::kProtocolVersion;
        child_options.binary_version = std::string(kVersion);
        child_options.report_ready_before_endpoints = true;
        child_options.startup_delay = std::chrono::seconds(2);
        const ChildProtocolDaemon starting_daemon(child_options);
        const std::string runtime_dir = paths::runtime_dir(paths::real_environment());
        if (symlink) {
            std::filesystem::remove(runtime_dir);
            const std::string target = env.root() + "/unsafe-runtime-target";
            std::filesystem::create_directory(target);
            std::filesystem::permissions(target, std::filesystem::perms::owner_all);
            REQUIRE(::symlink(target.c_str(), runtime_dir.c_str()) == 0);
        } else {
            std::filesystem::permissions(runtime_dir, std::filesystem::perms::owner_all |
                                                          std::filesystem::perms::group_read);
        }

        cli::RunOptions options;
        options.json = true;
        options.daemon_executable = TGCLI_TEST_BINARY;
        options.restart_timeout = std::chrono::milliseconds(500);
        const auto started = std::chrono::steady_clock::now();
        const auto result = run_captured({"daemon", "restart"}, options, env);
        const auto elapsed = std::chrono::steady_clock::now() - started;

        INFO(symlink);
        REQUIRE(result.exit_code == kGeneric);
        CHECK(elapsed < std::chrono::milliseconds(250));
        const auto rendered = json::parse(result.err);
        CHECK(rendered["error"]["details"] ==
              json{{"account", "main"}, {"operation", "restart"}, {"reason", "surface_invalid"}});
        CHECK(starting_daemon.running());
    }
}

#if defined(__linux__)
TEST_CASE("control token retries remain connected to the frozen datagram endpoint",
          "[cli][daemon-control][socket-safety]") {
    const IsolatedEnv env;
    prepare_account_layout();
    const auto real_env = paths::real_environment();
    std::string error;
    const auto control_path = paths::control_socket_path("main", real_env, error);
    REQUIRE(control_path.has_value());
    auto frozen = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
    REQUIRE(frozen.has_value());
    const int receive_flags = ::fcntl(frozen->fd, F_GETFL);
    REQUIRE(receive_flags >= 0);
    REQUIRE(::fcntl(frozen->fd, F_SETFL, receive_flags | O_NONBLOCK) == 0);

    int sender_fd = -1;
    REQUIRE(cli::detail::connect_verified_control_endpoint(*control_path, frozen->identity,
                                                           real_env.uid, sender_fd, error) ==
            cli::detail::ControlConnectOutcome::Connected);
    constexpr char filler = 'x';
    for (;;) {
        const ssize_t count = ::send(sender_fd, &filler, 1, MSG_DONTWAIT);
        if (count == 1 || (count < 0 && errno == EINTR)) {
            continue;
        }
        REQUIRE(count < 0);
        REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS));
        break;
    }

    std::optional<BoundEndpoint> replacement;
    bool retried = false;
    const auto on_retry = [&] {
        if (retried) {
            return;
        }
        retried = true;
        paths::unlink_socket_endpoint_if_same(*control_path, frozen->identity);
        REQUIRE_FALSE(std::filesystem::exists(*control_path));
        replacement = bind_private_endpoint(*control_path, SOCK_DGRAM, error);
        REQUIRE(replacement.has_value());
        std::array<char, 256> discarded{};
        while (::recv(frozen->fd, discarded.data(), discarded.size(), MSG_DONTWAIT) >= 0) {
        }
        REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
    };

    constexpr std::string_view token = "0123456789abcdef0123456789abcdef";
    const auto outcome = cli::detail::send_connected_control_stop(
        sender_fd, token, std::chrono::steady_clock::now() + std::chrono::seconds(1), on_retry,
        error);

    INFO(error);
    REQUIRE(outcome == cli::detail::ControlStopOutcome::Sent);
    REQUIRE(retried);
    std::array<char, 64> received{};
    const ssize_t frozen_count = ::recv(frozen->fd, received.data(), received.size(), MSG_DONTWAIT);
    REQUIRE(frozen_count == static_cast<ssize_t>(token.size()));
    CHECK(std::string_view(received.data(), static_cast<std::size_t>(frozen_count)) == token);
    const int replacement_flags = ::fcntl(replacement->fd, F_GETFL);
    REQUIRE(replacement_flags >= 0);
    REQUIRE(::fcntl(replacement->fd, F_SETFL, replacement_flags | O_NONBLOCK) == 0);
    CHECK(::recv(replacement->fd, received.data(), received.size(), MSG_DONTWAIT) < 0);
    CHECK((errno == EAGAIN || errno == EWOULDBLOCK));

    int replaced_fd = -1;
    CHECK(cli::detail::connect_verified_control_endpoint(*control_path, frozen->identity,
                                                         real_env.uid, replaced_fd, error) ==
          cli::detail::ControlConnectOutcome::Failed);
    CHECK(replaced_fd == -1);
    CHECK(error == "daemon control endpoint changed while connecting");
    CHECK(::recv(replacement->fd, received.data(), received.size(), MSG_DONTWAIT) < 0);
    CHECK((errno == EAGAIN || errno == EWOULDBLOCK));

    ::close(sender_fd);
    ::close(frozen->fd);
    ::close(replacement->fd);
}
#endif

TEST_CASE("daemon status uses the request deadline for a stalled Hello",
          "[cli][daemon-control][deadline]") {
    const IsolatedEnv env;
    const ChildBootstrapDaemon old_daemon({.hello_mode = BootstrapHelloMode::Stall});
    proto::Request request("main");
    request.id = 1;
    request.command = {"daemon", "status"};
    request.context.json = true;
    request.context.cwd = "/";
    request.context.timeout_seconds = 0.1;
    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::seconds(3);

    const auto started = std::chrono::steady_clock::now();
    const auto status = run_request_captured(request, options, env);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(status.exit_code == kGeneric);
    CHECK(elapsed < std::chrono::seconds(1));
    const auto rendered = json::parse(status.err);
    CHECK(rendered["error"]["details"] ==
          json{{"account", "main"}, {"operation", "status"}, {"reason", "handshake_failed"}});
    CHECK(old_daemon.running());
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

TEST_CASE("v3 client replaces verified v1 and v2 daemons for both binary cases",
          "[cli][restart][protocol-v1][protocol-v2][protocol-v3][process][tdlib]") {
    for (const int old_protocol : {1, 2}) {
        for (const std::string_view old_binary :
             {std::string_view{kVersion}, std::string_view{"old-test-binary"}}) {
            DYNAMIC_SECTION("protocol " << old_protocol << ", binary " << old_binary) {
                const IsolatedEnv env;
                ChildLegacyReplacementDaemon old_daemon(old_protocol, std::string(old_binary),
                                                        RawProtocolDaemonMode::MismatchOwner);

                cli::RunOptions options;
                options.json = true;
                options.daemon_executable = TGCLI_TEST_BINARY;
                options.restart_timeout = std::chrono::seconds(5);
                const auto result = run_captured({"version"}, options, env);

                INFO(result.err);
                REQUIRE(result.exit_code == kOk);
                CHECK(result.err.empty());
                const auto data = json::parse(result.out);
                CHECK(data["version"] == kVersion);
                CHECK(data["protocol"] == 3);
                CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
                stop_current_daemon(env);
            }
        }
    }
}

TEST_CASE("v1 and v2 raw clients replace a verified v3 daemon for both binary cases",
          "[cli][restart][protocol-v1][protocol-v2][protocol-v3][process]") {
    for (const int client_protocol : {1, 2}) {
        for (const std::string_view client_binary :
             {std::string_view{kVersion}, std::string_view{"legacy-client-binary"}}) {
            DYNAMIC_SECTION("protocol " << client_protocol << ", binary " << client_binary) {
                const IsolatedEnv env;
                ChildDaemonOptions old_options;
                old_options.require_no_dispatch = true;
                old_options.protocol_version = 3;
                old_options.binary_version = std::string(kVersion);
                ChildProtocolDaemon old_daemon(old_options);

                const auto real_env = paths::real_environment();
                std::string error;
                const auto socket_path = paths::socket_path("main", real_env, error);
                const auto control_path = paths::control_socket_path("main", real_env, error);
                REQUIRE(socket_path.has_value());
                REQUIRE(control_path.has_value());
                const std::string lock_path =
                    paths::account_state_dir("main", real_env) + "/daemon.lock";
                auto owner = daemon_lock::verify_owner(lock_path, real_env.uid, error);
                INFO(error);
                REQUIRE(owner.has_value());
                const auto main_identity =
                    paths::inspect_socket_endpoint(*socket_path, real_env.uid, error);
                const auto control_identity =
                    paths::inspect_socket_endpoint(*control_path, real_env.uid, error);
                REQUIRE(main_identity.has_value());
                REQUIRE(control_identity.has_value());

                const int legacy_fd = connect_fixture_socket(*socket_path);
                proto::FrameReader legacy_reader(legacy_fd);
                const auto old_hello = read_fixture_json(legacy_reader);
                CHECK(exact_json_fields(old_hello, {"type", "binary_version", "protocol_version"}));
                CHECK(old_hello["protocol_version"] == 3);
                write_fixture_json(legacy_fd, raw_hello(client_binary, client_protocol));
                const auto mismatch = std::get<proto::Error>(read_fixture_frame(legacy_reader));
                CHECK(mismatch.id == 0);
                CHECK(mismatch.code == "PROTOCOL_MISMATCH");
                check_fixture_eof(legacy_reader);
                ::close(legacy_fd);

                send_control_token(*control_path, owner->identity().control_token);
                CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);

                bool lock_released = false;
                REQUIRE(owner->owner_released(lock_released, error));
                CHECK(lock_released);
                bool main_changed = false;
                bool control_changed = false;
                REQUIRE(paths::socket_endpoint_changed(*socket_path, real_env.uid, *main_identity,
                                                       main_changed, error));
                REQUIRE(paths::socket_endpoint_changed(*control_path, real_env.uid,
                                                       *control_identity, control_changed, error));
                CHECK(main_changed);
                CHECK(control_changed);
                CHECK(::access(socket_path->c_str(), F_OK) != 0);
                CHECK(::access(control_path->c_str(), F_OK) != 0);

                ChildLegacyReplacementDaemon replacement(client_protocol,
                                                         std::string(client_binary));
                auto replacement_owner = daemon_lock::verify_owner(lock_path, real_env.uid, error);
                REQUIRE(replacement_owner.has_value());
                const int replacement_fd = connect_fixture_socket(*socket_path);
                proto::FrameReader replacement_reader(replacement_fd);
                const auto replacement_hello = read_fixture_json(replacement_reader);
                CHECK(replacement_hello == raw_hello(client_binary, client_protocol));
                write_fixture_json(replacement_fd, raw_hello(client_binary, client_protocol));
                const auto original = raw_request_dialect(client_protocol, 61, {"version"});
                CHECK(valid_raw_request_dialect(original, client_protocol, 61, {"version"}));
                CHECK(original.size() == (client_protocol == 1 ? 5 : 6));
                CHECK(original["context"].size() == 8);
                write_fixture_json(replacement_fd, original);
                const auto retried = read_fixture_json(replacement_reader);
                CHECK(exact_json_fields(retried, {"type", "id", "data"}));
                CHECK(retried["id"] == 61);
                CHECK(retried["data"]["protocol"] == client_protocol);
                ::close(replacement_fd);

                send_control_token(*control_path, replacement_owner->identity().control_token);
                CHECK(replacement.wait_for_exit(std::chrono::seconds(2)) == 0);
            }
        }
    }
}

TEST_CASE("daemon stop succeeds after verified binary-mismatch shutdown",
          "[cli][process][restart][schema]") {
    const IsolatedEnv env;
    ChildProtocolDaemon old_daemon({.protocol_version = proto::kProtocolVersion});

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

TEST_CASE("daemon stop uses frozen control for verified v1 and v2 owners without spawning",
          "[cli][process][protocol-v1][protocol-v2][schema]") {
    for (const int old_protocol : {1, 2}) {
        DYNAMIC_SECTION("protocol " << old_protocol) {
            const IsolatedEnv env;
            ChildProtocolDaemon old_daemon({.protocol_version = old_protocol});

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
    }
}

TEST_CASE("daemon stop uses the frozen control surface for an unparseable Hello",
          "[cli][daemon-control][schema]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon;

    cli::RunOptions options;
    options.json = true;
    options.auto_spawn = false;
    options.restart_timeout = std::chrono::seconds(3);
    const auto result = run_captured({"daemon", "stop"}, options, env);

    INFO(result.err);
    REQUIRE(result.exit_code == kOk);
    CHECK(result.err.empty());
    CHECK(json::parse(result.out) == json{{"stopping", true}});
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
}

TEST_CASE("daemon restart starts absent and replaces compatible or unparseable owners",
          "[cli][daemon-control][schema][tdlib]") {
    SECTION("absent") {
        const IsolatedEnv env;
        cli::RunOptions options;
        options.json = true;
        options.daemon_executable = TGCLI_TEST_BINARY;
        options.restart_timeout = std::chrono::seconds(5);
        const auto result = run_captured({"daemon", "restart"}, options, env);

        INFO(result.err);
        REQUIRE(result.exit_code == kOk);
        CHECK(result.err.empty());
        const auto data = json::parse(result.out);
        CHECK(data["restarted"] == true);
        CHECK(data["version"] == kVersion);
        CHECK(data["protocol"] == proto::kProtocolVersion);
        CHECK_THAT(data, test::matches_json_schema("daemon-restart.result.schema.json"));
        stop_current_daemon(env);
    }

    SECTION("compatible") {
        const IsolatedEnv env;
        cli::RunOptions options;
        options.json = true;
        options.daemon_executable = TGCLI_TEST_BINARY;
        options.restart_timeout = std::chrono::seconds(5);
        REQUIRE(run_captured({"version"}, options, env).exit_code == kOk);
        options.auto_spawn = false;
        const auto before = run_captured({"daemon", "status"}, options, env);
        REQUIRE(before.exit_code == kOk);
        const auto old_pid = json::parse(before.out)["pid"].get<pid_t>();

        const auto result = run_captured({"daemon", "restart"}, options, env);
        INFO(result.err);
        REQUIRE(result.exit_code == kOk);
        const auto data = json::parse(result.out);
        CHECK(data["pid"].get<pid_t>() != old_pid);
        CHECK_THAT(data, test::matches_json_schema("daemon-restart.result.schema.json"));
        stop_current_daemon(env);
    }

    SECTION("unparseable Hello") {
        const IsolatedEnv env;
        ChildBootstrapDaemon old_daemon;
        const pid_t old_pid = old_daemon.pid();
        cli::RunOptions options;
        options.json = true;
        options.daemon_executable = TGCLI_TEST_BINARY;
        options.restart_timeout = std::chrono::seconds(5);
        const auto result = run_captured({"daemon", "restart"}, options, env);

        INFO(result.err);
        REQUIRE(result.exit_code == kOk);
        const auto data = json::parse(result.out);
        CHECK(data["pid"].get<pid_t>() != old_pid);
        CHECK(data["version"] == kVersion);
        CHECK_THAT(data, test::matches_json_schema("daemon-restart.result.schema.json"));
        CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
        stop_current_daemon(env);
    }
}

TEST_CASE("daemon restart joins teardown when the frozen owner closes during Hello",
          "[cli][daemon-control][process][tdlib]") {
    const IsolatedEnv env;
    ChildBootstrapDaemon old_daemon({.hello_mode = BootstrapHelloMode::Shutdown});

    cli::RunOptions options;
    options.json = true;
    options.daemon_executable = TGCLI_TEST_BINARY;
    options.restart_timeout = std::chrono::seconds(5);
    const auto result = run_captured({"daemon", "restart"}, options, env);

    INFO(result.err);
    REQUIRE(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK(data["restarted"] == true);
    CHECK(data["pid"].get<pid_t>() != old_daemon.pid());
    CHECK(data["version"] == kVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
}

TEST_CASE("daemon restart joins a proofable owner that binds its endpoints late",
          "[cli][daemon-control][process]") {
    const IsolatedEnv env;
    ChildDaemonOptions child_options;
    child_options.protocol_version = proto::kProtocolVersion;
    child_options.binary_version = std::string(kVersion);
    child_options.report_ready_before_endpoints = true;
    child_options.startup_delay = std::chrono::milliseconds(300);
    ChildProtocolDaemon starting_daemon(child_options);

    cli::RunOptions options;
    options.json = true;
    options.daemon_executable = TGCLI_TEST_BINARY;
    options.restart_timeout = std::chrono::seconds(3);
    const auto result = run_captured({"daemon", "restart"}, options, env);

    INFO(result.err);
    REQUIRE(result.exit_code == kOk);
    CHECK(result.err.empty());
    const auto data = json::parse(result.out);
    CHECK(data["restarted"] == true);
    CHECK(data["pid"] == starting_daemon.pid());
    CHECK(data["version"] == kVersion);
    CHECK(starting_daemon.running());
    const auto stopped = run_captured({"daemon", "stop"}, options, env);
    CHECK(stopped.exit_code == kOk);
    CHECK(starting_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
}

TEST_CASE("concurrent daemon restarts converge on one replacement owner",
          "[cli][daemon-control][process][tdlib]") {
    const IsolatedEnv env;
    configure_main_account();
    ChildBootstrapDaemon old_daemon({.control_stop_delay = std::chrono::milliseconds(300)});
    AsyncCliProcess first(env.root() + "/restart-first.out", env.root() + "/restart-first.err",
                          "restart");
    AsyncCliProcess second(env.root() + "/restart-second.out", env.root() + "/restart-second.err",
                           "restart");

    const int first_exit = first.wait_for_exit(std::chrono::seconds(12));
    const int second_exit = second.wait_for_exit(std::chrono::seconds(12));
    INFO(first.errors());
    INFO(second.errors());
    REQUIRE(first_exit == kOk);
    REQUIRE(second_exit == kOk);
    const auto first_result = json::parse(first.output());
    const auto second_result = json::parse(second.output());
    CHECK(first_result["pid"] == second_result["pid"]);
    CHECK(first_result["version"] == kVersion);
    CHECK(second_result["protocol"] == proto::kProtocolVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
}

TEST_CASE("concurrent daemon stops converge after one frozen-owner shutdown",
          "[cli][daemon-control][process]") {
    const IsolatedEnv env;
    configure_main_account();
    ChildBootstrapDaemon old_daemon({.control_stop_delay = std::chrono::milliseconds(300)});
    AsyncCliProcess first(env.root() + "/stop-first.out", env.root() + "/stop-first.err", "stop");
    AsyncCliProcess second(env.root() + "/stop-second.out", env.root() + "/stop-second.err",
                           "stop");

    const int first_exit = first.wait_for_exit(std::chrono::seconds(8));
    const int second_exit = second.wait_for_exit(std::chrono::seconds(8));
    INFO(first.errors());
    INFO(second.errors());
    REQUIRE(first_exit == kOk);
    REQUIRE(second_exit == kOk);
    CHECK(json::parse(first.output()) == json{{"stopping", true}});
    CHECK(json::parse(second.output()) == json{{"stopping", true}});
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
}

TEST_CASE("compatible concurrent stop joins an exact-id shutdown loser",
          "[cli][daemon-control][process]") {
    const IsolatedEnv env;
    configure_main_account();
    ChildCompatibleStopRaceDaemon old_daemon;
    AsyncCliProcess first(env.root() + "/compatible-stop-first.out",
                          env.root() + "/compatible-stop-first.err", "stop");
    AsyncCliProcess second(env.root() + "/compatible-stop-second.out",
                           env.root() + "/compatible-stop-second.err", "stop");

    const int first_exit = first.wait_for_exit(std::chrono::seconds(8));
    const int second_exit = second.wait_for_exit(std::chrono::seconds(8));
    INFO(first.errors());
    INFO(second.errors());
    REQUIRE(first_exit == kOk);
    REQUIRE(second_exit == kOk);
    CHECK(json::parse(first.output()) == json{{"stopping", true}});
    CHECK(json::parse(second.output()) == json{{"stopping", true}});
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
}

TEST_CASE("compatible concurrent restart joins an exact-id shutdown loser",
          "[cli][daemon-control][process][tdlib]") {
    const IsolatedEnv env;
    configure_main_account();
    ChildCompatibleStopRaceDaemon old_daemon;
    AsyncCliProcess first(env.root() + "/compatible-restart-first.out",
                          env.root() + "/compatible-restart-first.err", "restart");
    AsyncCliProcess second(env.root() + "/compatible-restart-second.out",
                           env.root() + "/compatible-restart-second.err", "restart");

    const int first_exit = first.wait_for_exit(std::chrono::seconds(12));
    const int second_exit = second.wait_for_exit(std::chrono::seconds(12));
    INFO(first.errors());
    INFO(second.errors());
    REQUIRE(first_exit == kOk);
    REQUIRE(second_exit == kOk);
    const auto first_result = json::parse(first.output());
    const auto second_result = json::parse(second.output());
    CHECK(first_result["pid"] == second_result["pid"]);
    CHECK(first_result["version"] == kVersion);
    CHECK(second_result["protocol"] == proto::kProtocolVersion);
    CHECK(old_daemon.wait_for_exit(std::chrono::seconds(2)) == 0);
    stop_current_daemon(env);
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
    child_options.protocol_version = proto::kProtocolVersion;
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
    configure_main_account();
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
    configure_main_account();
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
    DelayedExternalStop external_stop(old_daemon, std::chrono::milliseconds(50));

    cli::RunOptions options;
    options.json = true;
    options.daemon_executable = TGCLI_TEST_BINARY;
    options.restart_timeout = std::chrono::seconds(3);
    const bool helper_started = external_stop.start();
    RunOutcome result;
    if (helper_started) {
        result = run_captured({"version"}, options, env);
    }
    const int helper_exit = external_stop.wait_for_exit(std::chrono::seconds(2));

    REQUIRE(helper_started);
    REQUIRE(helper_exit == 0);
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
    daemon::Server server(
        {"main", *socket_path, context.binary_version, context.protocol_version, {}, {}},
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
    CHECK(error_json["error"]["message"].get<std::string>().find("identity is malformed") !=
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
