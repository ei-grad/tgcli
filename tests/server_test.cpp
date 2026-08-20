// Socket-level round-trip: a real Server on a real unix socket, a raw client
// speaking the frame protocol. td-free (fake context), so TSan covers it.

#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "daemon/activity_tracker.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/read_domain.hpp"
#include "daemon/request_session.hpp"
#include "daemon/server.hpp"
#include "proto/frame_io.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <latch>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {

std::string test_socket_path(std::string_view account = "main") {
    // Short and per-pid: sun_path is ~104 bytes and tests may run parallel.
    return "/tmp/tgcli-test-" + std::to_string(getpid()) + "/" + std::string(account) + ".sock";
}

int connect_to(const std::string& path) {
    const int fd = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

proto::Frame read_frame_until(proto::FrameReader& reader,
                              std::chrono::steady_clock::time_point deadline) {
    std::string io_error;
    const auto line = reader.read_line_until(deadline, io_error);
    INFO("io error: " << io_error);
    REQUIRE(line.has_value());
    std::string parse_error;
    auto frame = proto::parse(*line, parse_error);
    INFO("parse error: " << parse_error);
    REQUIRE(frame.has_value());
    return std::move(*frame);
}

proto::Frame read_frame(proto::FrameReader& reader) {
    return read_frame_until(reader, std::chrono::steady_clock::now() + std::chrono::seconds(5));
}

void send_frame(int fd, const proto::Frame& frame) {
    std::string error;
    REQUIRE(proto::write_frame(fd, frame, error));
}

void send_line(int fd, std::string_view line) {
    REQUIRE(::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
    constexpr char newline = '\n';
    REQUIRE(::write(fd, &newline, 1) == 1);
}

void check_eof(proto::FrameReader& reader) {
    std::string io_error;
    CHECK_FALSE(
        reader.read_line_until(std::chrono::steady_clock::now() + std::chrono::seconds(2), io_error)
            .has_value());
    CHECK(io_error.empty());
}

proto::Request make_request(std::vector<std::string> command, std::uint64_t id = 1,
                            std::string account = "test") {
    proto::Request request(std::move(account));
    request.id = id;
    request.command = std::move(command);
    request.context.tty = true;
    request.context.cwd = "/";
    return request;
}

constexpr std::array<std::pair<daemon::testing::RequestObservationStage, std::string_view>, 7>
    kRequestObservationStages{
        {{daemon::testing::RequestObservationStage::ConfigRead, "config"},
         {daemon::testing::RequestObservationStage::HookExecution, "hook"},
         {daemon::testing::RequestObservationStage::AuthStateRead, "auth"},
         {daemon::testing::RequestObservationStage::PathResolution, "path"},
         {daemon::testing::RequestObservationStage::ActivityAdmission, "activity"},
         {daemon::testing::RequestObservationStage::SessionConstruction, "session"},
         {daemon::testing::RequestObservationStage::DispatcherLookup, "dispatch"}}};

struct RequestObservationCounters {
    std::array<std::atomic<int>, kRequestObservationStages.size()> counts{};

    void observe(daemon::testing::RequestObservationStage stage) {
        counts.at(static_cast<std::size_t>(stage)).fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] daemon::testing::RequestObservationObserver observer() {
        return [this](daemon::testing::RequestObservationStage stage) { observe(stage); };
    }

    void reset() {
        for (auto& count : counts) {
            count.store(0, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] int get(daemon::testing::RequestObservationStage stage) const {
        return counts.at(static_cast<std::size_t>(stage)).load(std::memory_order_relaxed);
    }
};

struct RoutedAccountProbe {
    std::string account;
    RequestObservationCounters* observations;

    RoutedAccountProbe(std::string account_value, RequestObservationCounters& observations_value)
        : account(std::move(account_value)), observations(&observations_value) {}

    void install(daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "observe",
            {daemon::Tier::Read, [this](const proto::Request&, daemon::RequestSession& session) {
                 session.result({{"account", account}});
             }});
    }

    [[nodiscard]] daemon::testing::RequestAdmissionProbe admission_probe() {
        return [this] {
            read_config();
            execute_hook();
            read_auth_state();
            static_cast<void>(resolve_data_root());
        };
    }

  private:
    void read_config() const {
        observations->observe(daemon::testing::RequestObservationStage::ConfigRead);
    }

    void execute_hook() const {
        observations->observe(daemon::testing::RequestObservationStage::HookExecution);
    }

    void read_auth_state() const {
        observations->observe(daemon::testing::RequestObservationStage::AuthStateRead);
    }

    [[nodiscard]] std::string resolve_data_root() const {
        observations->observe(daemon::testing::RequestObservationStage::PathResolution);
        paths::Environment environment;
        environment.home = "/tmp/tgcli-observation-home";
        environment.uid = getuid();
        return paths::account_data_dir(account, environment);
    }
};

void check_request_observations(const RequestObservationCounters& observations, int expected) {
    for (const auto& [stage, name] : kRequestObservationStages) {
        INFO(name);
        CHECK(observations.get(stage) == expected);
    }
}

class RuntimeConfig {
  public:
    RuntimeConfig() {
        std::string pattern = "/tmp/tgcli-server-runtime-XXXXXX";
        pattern.push_back('\0');
        const char* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        directory_ = root_ / "tgcli";
        REQUIRE(std::filesystem::create_directory(directory_));
        REQUIRE(::chmod(directory_.c_str(), 0700) == 0);
    }

    ~RuntimeConfig() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    RuntimeConfig(const RuntimeConfig&) = delete;
    RuntimeConfig& operator=(const RuntimeConfig&) = delete;
    RuntimeConfig(RuntimeConfig&&) = delete;
    RuntimeConfig& operator=(RuntimeConfig&&) = delete;

    [[nodiscard]] std::string file() const {
        return (directory_ / "config.toml").string();
    }

    void write_initial(std::string_view bytes) const {
        write_file(file(), bytes);
    }

    void replace(std::string_view bytes) const {
        const auto replacement = directory_ / "replacement.toml";
        write_file(replacement, bytes);
        REQUIRE(::rename(replacement.c_str(), file().c_str()) == 0);
    }

  private:
    static void write_file(const std::filesystem::path& file, std::string_view bytes) {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(file.c_str(), 0600) == 0);
    }

    std::filesystem::path root_;
    std::filesystem::path directory_;
};

std::string runtime_account_config(std::string_view idle_exit, bool allow_write = false,
                                   std::string_view password_hook = {}) {
    std::string result = "default_account = \"main\"\n[accounts.main]\nallow_write = ";
    result += allow_write ? "true\n" : "false\n";
    if (!idle_exit.empty()) {
        result += "idle_exit = " + std::string(idle_exit) + "\n";
    }
    if (!password_hook.empty()) {
        result += "password_cmd = \"" + std::string(password_hook) + "\"\n";
    }
    return result;
}

template <typename Predicate>
bool wait_for_condition(std::chrono::steady_clock::duration timeout, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

struct TestDaemon {
    daemon::DaemonContext context;
    daemon::Dispatcher dispatcher;
    std::string account;
    std::string socket;
    daemon::Server server;

    TestDaemon(const TestDaemon&) = delete;
    TestDaemon& operator=(const TestDaemon&) = delete;
    TestDaemon(TestDaemon&&) = delete;
    TestDaemon& operator=(TestDaemon&&) = delete;

    explicit TestDaemon(
        const std::function<void(daemon::Dispatcher&)>& configure = {},
        bool register_default_commands = true, secure::WipeObserver wipe_observer = {},
        std::string account_value = "test",
        const daemon::testing::RequestObservationObserver& request_observer = {},
        daemon::testing::RequestAdmissionProbe request_admission_probe = {},
        daemon::ConfigRuntime* config_runtime = nullptr,
        std::shared_ptr<const daemon::testing::ActivityTrackerHooks> activity_hooks = {},
        daemon::testing::RequestWallClock request_wall_clock = {})
        : dispatcher(request_observer), account(std::move(account_value)),
          socket(test_socket_path(account)), server({account,
                                                     socket,
                                                     "9.9.9",
                                                     proto::kProtocolVersion,
                                                     {},
                                                     {},
                                                     std::move(wipe_observer),
                                                     request_observer,
                                                     std::move(request_admission_probe),
                                                     config_runtime,
                                                     std::move(activity_hooks),
                                                     std::move(request_wall_clock)},
                                                    dispatcher) {
        std::string error;
        const auto separator = socket.rfind('/');
        REQUIRE(separator != std::string::npos);
        REQUIRE(paths::ensure_private_dir(socket.substr(0, separator), getuid(), error));
        context.account = account;
        context.binary_version = "9.9.9";
        context.protocol_version = proto::kProtocolVersion;
        context.tdlib_version = "1.2.3";
        context.socket_path = socket;
        context.request_shutdown = [this] { server.request_stop(); };
        if (register_default_commands) {
            daemon::register_commands(dispatcher, context);
        }
        if (configure) {
            configure(dispatcher);
        }
        INFO("server start error: " << error);
        REQUIRE(server.start(error));
    }

    ~TestDaemon() {
        server.stop();
        const auto separator = socket.rfind('/');
        if (separator != std::string::npos) {
            ::rmdir(socket.substr(0, separator).c_str());
        }
    }
};

proto::Frame send_request(const TestDaemon& daemon, proto::Request request) {
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    static_cast<void>(read_frame(reader));
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, std::move(request));
    auto terminal = read_frame(reader);
    ::close(fd);
    return terminal;
}

struct BlockingCommand {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    bool finished = false;

    void install(daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "block",
            {daemon::Tier::Read, [this](const proto::Request&, daemon::RequestSession& sink) {
                 {
                     std::unique_lock<std::mutex> lock(mutex);
                     entered = true;
                     cv.notify_all();
                     cv.wait_for(lock, std::chrono::seconds(10), [this] { return release; });
                 }
                 sink.result({{"late", true}});
                 {
                     const std::lock_guard<std::mutex> lock(mutex);
                     finished = true;
                 }
                 cv.notify_all();
             }});
    }

    void wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }));
    }

    void release_and_wait() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return finished; }));
    }
};

struct AdmissionInspector {
    static void install(daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "inspect admission",
            {daemon::Tier::Read,
             [](const proto::Request& request, daemon::RequestSession& session) {
                 const auto& admission = session.admitted_config();
                 if (!admission || !admission->account_snapshot) {
                     session.error("INTERNAL", "config admission is missing", json::object(),
                                   kGeneric);
                     return;
                 }
                 const auto authority = daemon::evaluate_destructive_authority(
                     request.context, {.grant_valid = admission->standing_write_grants_valid,
                                       .allow_write = admission->settings.allow_write});
                 std::string authority_value;
                 if (const auto* granted = std::get_if<daemon::GrantedAuthority>(&authority)) {
                     authority_value = std::string(daemon::authority_source_name(granted->source));
                 } else if (const auto* denied = std::get_if<daemon::DeniedAuthority>(&authority)) {
                     authority_value =
                         std::string(daemon::write_denial_reason_name(denied->reason));
                 } else {
                     authority_value = "dry_run";
                 }
                 const auto& account = admission->account_snapshot->accounts.at("main");
                 session.result({{"state", static_cast<int>(admission->state)},
                                 {"generation", admission->generation},
                                 {"authority", authority_value},
                                 {"allow_write", admission->settings.allow_write},
                                 {"standing_grant_valid", admission->standing_write_grants_valid},
                                 {"password_cmd", account.password_cmd.value_or("")},
                                 {"account_count", admission->account_snapshot->accounts.size()},
                                 {"reload_reason", admission->reload_diagnostic
                                                       ? json(config::reason_name(
                                                             admission->reload_diagnostic->reason))
                                                       : json(nullptr)}});
             }});
    }
};

struct BlockingAdmissionCommand {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;

    void install(daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "block admission",
            {daemon::Tier::Read, [this](const proto::Request&, daemon::RequestSession& session) {
                 const auto admission = session.admitted_config();
                 {
                     std::unique_lock lock(mutex);
                     entered = true;
                     cv.notify_all();
                     cv.wait(lock, [this] { return release; });
                 }
                 if (!admission) {
                     session.error("INTERNAL", "config admission is missing", json::object(),
                                   kGeneric);
                     return;
                 }
                 session.result({{"generation", admission->generation},
                                 {"idle_exit", admission->settings.idle_exit
                                                   ? json(admission->settings.idle_exit->count())
                                                   : json(nullptr)}});
             }});
    }

    void wait_until_entered() {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }));
    }

    void unblock() {
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
    }
};

class ForcedReloadGate {
  public:
    void before_reload(bool forced) {
        if (!forced) {
            return;
        }
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        REQUIRE(condition_.wait_for(lock, std::chrono::seconds(5), [this] { return entered_; }));
    }

    void release() {
        {
            const std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

class AdmissionFinishProbe {
  public:
    void notify(daemon::ConfigRefreshStatus status) {
        const std::lock_guard lock(mutex_);
        statuses_.push_back(status);
        condition_.notify_all();
    }

    bool wait_for(daemon::ConfigRefreshStatus status, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, timeout, [&] { return std::ranges::find(statuses_, status) != statuses_.end(); });
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<daemon::ConfigRefreshStatus> statuses_;
};

struct BlockingStopCommand {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    bool finished = false;

    void install(daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "daemon stop",
            {daemon::Tier::Read, [this](const proto::Request&, daemon::RequestSession& sink) {
                 {
                     std::unique_lock<std::mutex> lock(mutex);
                     entered = true;
                     cv.notify_all();
                     cv.wait_for(lock, std::chrono::seconds(10), [this] { return release; });
                 }
                 sink.result({{"stopping", true}});
                 {
                     const std::lock_guard<std::mutex> lock(mutex);
                     finished = true;
                 }
                 cv.notify_all();
             }});
    }

    void wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }));
    }

    void release_and_wait() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return finished; }));
    }
};

struct TerminalThenBlockCommand {
    std::mutex mutex;
    std::condition_variable cv;
    bool terminal_sent = false;
    bool release = false;
    bool finished = false;

    void install(daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "terminal then block",
            {daemon::Tier::Read, [this](const proto::Request&, daemon::RequestSession& sink) {
                 sink.result({{"first", true}});
                 {
                     std::unique_lock<std::mutex> lock(mutex);
                     terminal_sent = true;
                     cv.notify_all();
                     cv.wait_for(lock, std::chrono::seconds(10), [this] { return release; });
                     finished = true;
                 }
                 cv.notify_all();
             }});
    }

    void wait_for_terminal() {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return terminal_sent; }));
    }

    void release_and_wait() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return finished; }));
    }
};

daemon::ChallengeSpec code_challenge(std::uint64_t auth_sequence = 9) {
    return {proto::ChallengeKind::AuthenticationCode,
            4,
            auth_sequence,
            "Code: ",
            {{"delivery_type", "sms"}, {"expected_length", 5}, {"resend_timeout", 30}}};
}

proto::Answer answer_for(std::uint64_t request_id, const json& challenge, json value = "12345") {
    return {request_id,
            {{"nonce", challenge["nonce"]},
             {"sequence", challenge["sequence"]},
             {"client_generation", challenge["client_generation"]},
             {"auth_sequence", challenge["auth_sequence"]},
             {"value", std::move(value)}}};
}

void install_challenge_command(daemon::Dispatcher& dispatcher) {
    dispatcher.register_command(
        "challenge socket",
        {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& session) {
             auto outcome = session.challenge(code_challenge());
             switch (outcome.status()) {
             case daemon::ChallengeStatus::Answered: {
                 if (!session.reserve_in_flight()) {
                     return;
                 }
                 session.settle_in_flight();
                 std::string value;
                 static_cast<void>(outcome.take_string(value));
                 session.result({{"value", value}});
                 return;
             }
             case daemon::ChallengeStatus::Cancelled:
                 session.error("AUTH_CANCELLED", "authentication cancelled",
                               {{"account", "test"},
                                {"state", "wait_code"},
                                {"challenge", "authentication_code"}},
                               kNotAuthed);
                 return;
             case daemon::ChallengeStatus::NoTty:
                 session.error("AUTH_INPUT_REQUIRED", "authentication input required",
                               {{"account", "test"},
                                {"state", "wait_code"},
                                {"challenge", "authentication_code"}},
                               kNotAuthed);
                 return;
             case daemon::ChallengeStatus::TimedOut:
                 session.error("TIMEOUT", "authentication timed out",
                               {{"operation", "login"}, {"state", "wait_code"}}, kTimeout);
                 return;
             case daemon::ChallengeStatus::Disconnected:
             case daemon::ChallengeStatus::Shutdown:
             case daemon::ChallengeStatus::ProtocolError:
                 return;
             case daemon::ChallengeStatus::Superseded:
                 session.error("INTERNAL", "unexpected supersession", json::object(), kGeneric);
                 return;
             }
         }});
}

} // namespace

TEST_CASE("challenge answer round-trip keeps the socket reader live", "[server][challenge]") {
    const TestDaemon daemon(install_challenge_command);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"challenge", "socket"}, 71));

    const auto challenge_frame = read_frame(reader);
    const auto* challenge = std::get_if<proto::Challenge>(&challenge_frame);
    REQUIRE(challenge != nullptr);
    CHECK(challenge->id == 71);
    send_frame(fd, answer_for(71, challenge->challenge));
    const auto terminal = read_frame(reader);
    const auto* result = std::get_if<proto::Result>(&terminal);
    REQUIRE(result != nullptr);
    CHECK(result->data == json{{"value", "12345"}});
    ::close(fd);
}

TEST_CASE("answer sources are wiped before the challenged worker resumes",
          "[server][challenge][secret][ordering]") {
    std::atomic<bool> parsed_line_wiped{false};
    std::atomic<bool> answer_source_wiped{false};
    std::atomic<bool> answer_move_source_wiped{false};
    std::atomic<bool> answer_payload_wiped{false};
    std::atomic<bool> challenge_value_source_wiped{false};
    std::atomic<bool> candidate_copy_seen{false};
    const auto observer = [&](std::string_view stage, const char* bytes, std::size_t size) {
        const bool all_zero =
            size == 0 || std::all_of(bytes, bytes + static_cast<std::ptrdiff_t>(size),
                                     [](char value) { return value == '\0'; });
        if (stage == "parsed_line" && all_zero) {
            parsed_line_wiped.store(true, std::memory_order_release);
        } else if (stage == "answer_source" && all_zero) {
            answer_source_wiped.store(true, std::memory_order_release);
        } else if (stage == "answer_move_source" && all_zero) {
            answer_move_source_wiped.store(true, std::memory_order_release);
        } else if (stage == "answer_payload" && all_zero) {
            answer_payload_wiped.store(true, std::memory_order_release);
        } else if (stage == "challenge_value_source" && all_zero) {
            challenge_value_source_wiped.store(true, std::memory_order_release);
        } else if (stage == "candidate_line" || stage == "candidate_json") {
            candidate_copy_seen.store(true, std::memory_order_release);
        }
    };
    const TestDaemon daemon(
        [&](daemon::Dispatcher& dispatcher) {
            dispatcher.register_command(
                "wipe order",
                {daemon::Tier::Read, [&](const proto::Request&, daemon::RequestSession& session) {
                     auto outcome = session.challenge(code_challenge());
                     std::string value;
                     const bool answered = outcome.take_string(value);
                     secure::wipe(value);
                     session.result(
                         {{"answered", answered},
                          {"sources_wiped_before_resume",
                           parsed_line_wiped.load(std::memory_order_acquire) &&
                               answer_source_wiped.load(std::memory_order_acquire) &&
                               answer_move_source_wiped.load(std::memory_order_acquire) &&
                               answer_payload_wiped.load(std::memory_order_acquire) &&
                               challenge_value_source_wiped.load(std::memory_order_acquire) &&
                               !candidate_copy_seen.load(std::memory_order_acquire)}});
                 }});
        },
        false, observer);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"wipe", "order"}, 711));
    const auto challenge_frame = std::get<proto::Challenge>(read_frame(reader));
    send_frame(fd, answer_for(711, challenge_frame.challenge, "123456"));
    const auto result = std::get<proto::Result>(read_frame(reader));
    CHECK(result.data == json{{"answered", true}, {"sources_wiped_before_resume", true}});
    CHECK_FALSE(candidate_copy_seen.load(std::memory_order_acquire));
    ::close(fd);
}

TEST_CASE("socket and in-process dispatch have equivalent challenge results",
          "[server][challenge][dispatch]") {
    const proto::Request in_process_request = make_request({"challenge", "socket"}, 81);
    std::optional<json> in_process_result;
    daemon::CallbackSink in_process_sink(
        [](const json&) {}, [](const json&) {},
        [&in_process_result](json data) { in_process_result = std::move(data); },
        [](const std::string&, const std::string&, const json&, int) {
            FAIL("in-process request failed");
        },
        [](const json& emitted) -> std::optional<json> { return answer_for(81, emitted).answer; });
    daemon::Dispatcher in_process_dispatcher;
    install_challenge_command(in_process_dispatcher);
    daemon::RequestSession in_process_session(in_process_request, in_process_sink, 9001);
    in_process_dispatcher.dispatch(in_process_session);
    REQUIRE(in_process_result.has_value());

    const TestDaemon daemon(install_challenge_command);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"challenge", "socket"}, 81));
    const auto challenge_frame = std::get<proto::Challenge>(read_frame(reader));
    send_frame(fd, answer_for(81, challenge_frame.challenge));
    const auto socket_result = std::get<proto::Result>(read_frame(reader));
    CHECK(socket_result.data == *in_process_result);
    ::close(fd);
}

TEST_CASE("answer on another connection cannot affect the challenge owner", "[server][challenge]") {
    const TestDaemon daemon(install_challenge_command);
    const int owner_fd = connect_to(daemon.socket);
    proto::FrameReader owner_reader(owner_fd);
    read_frame(owner_reader);
    send_frame(owner_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(owner_fd, make_request({"challenge", "socket"}, 72));
    const auto owner_frame = read_frame(owner_reader);
    const auto* owner_challenge = std::get_if<proto::Challenge>(&owner_frame);
    REQUIRE(owner_challenge != nullptr);

    const int other_fd = connect_to(daemon.socket);
    proto::FrameReader other_reader(other_fd);
    read_frame(other_reader);
    send_frame(other_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(other_fd, answer_for(72, owner_challenge->challenge));
    const auto rejected = read_frame(other_reader);
    const auto* error = std::get_if<proto::Error>(&rejected);
    REQUIRE(error != nullptr);
    CHECK(error->code == "PROTOCOL_ANSWER_INVALID");
    CHECK(error->details == json{{"request_id", 72}, {"reason", "unknown_request"}});

    send_frame(owner_fd, answer_for(72, owner_challenge->challenge));
    CHECK(std::holds_alternative<proto::Result>(read_frame(owner_reader)));
    ::close(other_fd);
    ::close(owner_fd);
}

TEST_CASE("changed request id is unknown after its consumed challenge terminates",
          "[server][challenge]") {
    const TestDaemon daemon(install_challenge_command);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"challenge", "socket"}, 721));
    const auto emitted = std::get<proto::Challenge>(read_frame(reader));
    send_frame(fd, answer_for(721, emitted.challenge));
    CHECK(std::get<proto::Result>(read_frame(reader)).id == 721);

    send_frame(fd, answer_for(722, emitted.challenge));
    const auto rejected = std::get<proto::Error>(read_frame(reader));
    CHECK(rejected.id == 722);
    CHECK(rejected.code == "PROTOCOL_ANSWER_INVALID");
    CHECK(rejected.details == json{{"request_id", 722}, {"reason", "unknown_request"}});

    const int other_fd = connect_to(daemon.socket);
    proto::FrameReader other_reader(other_fd);
    read_frame(other_reader);
    send_frame(other_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(other_fd, make_request({"version"}, 723));
    CHECK(std::get<proto::Result>(read_frame(other_reader)).id == 723);
    ::close(other_fd);
    ::close(fd);
}

TEST_CASE("one active request per connection does not replace a challenge", "[server][challenge]") {
    const TestDaemon daemon(install_challenge_command);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"challenge", "socket"}, 73));
    const auto frame = read_frame(reader);
    const auto* challenge = std::get_if<proto::Challenge>(&frame);
    REQUIRE(challenge != nullptr);
    send_frame(fd, make_request({"version"}, 74));
    const auto rejected = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&rejected);
    REQUIRE(error != nullptr);
    CHECK(error->id == 74);
    CHECK(error->code == "USAGE");
    send_frame(fd, answer_for(73, challenge->challenge));
    const auto terminal = read_frame(reader);
    CHECK(std::get<proto::Result>(terminal).id == 73);
    ::close(fd);
}

TEST_CASE("malformed answer cancels only its owning request with the protocol error",
          "[server][challenge]") {
    const TestDaemon daemon(install_challenge_command);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"challenge", "socket"}, 75));
    const auto frame = read_frame(reader);
    REQUIRE(std::holds_alternative<proto::Challenge>(frame));
    const std::string malformed = R"({"type":"answer","id":75,"answer":{"nonce":"bad"}})";
    REQUIRE(::write(fd, malformed.data(), malformed.size()) ==
            static_cast<ssize_t>(malformed.size()));
    constexpr char newline = '\n';
    REQUIRE(::write(fd, &newline, 1) == 1);
    const auto terminal = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&terminal);
    REQUIRE(error != nullptr);
    CHECK(error->id == 75);
    CHECK(error->code == "PROTOCOL_ANSWER_INVALID");
    CHECK(error->details == json{{"request_id", 75}, {"reason", "malformed"}});
    ::close(fd);
}

TEST_CASE("malformed frame variants cannot terminate the detached daemon reader",
          "[server][process][proto]") {
    const TestDaemon daemon;
    for (
        const auto malformed : std::to_array<std::string_view>({
            R"({"type":null,"id":1,"answer":{}})",
            R"({"type":{},"id":1,"answer":{}})",
            R"({"type":[],"id":1,"answer":{}})",
            R"({"type":"hello","binary_version":"v","protocol_version":18446744073709551615})",
            R"({"type":"error","id":1,"error":{"code":"X","message":"m","details":{}},"exit_code":18446744073709551615})",
        })) {
        const int malformed_fd = connect_to(daemon.socket);
        proto::FrameReader malformed_reader(malformed_fd);
        read_frame(malformed_reader);
        send_frame(malformed_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
        const std::string line = std::string(malformed) + '\n';
        REQUIRE(::write(malformed_fd, line.data(), line.size()) ==
                static_cast<ssize_t>(line.size()));
        CHECK(std::holds_alternative<proto::Error>(read_frame(malformed_reader)));
        ::close(malformed_fd);

        const int health_fd = connect_to(daemon.socket);
        proto::FrameReader health_reader(health_fd);
        read_frame(health_reader);
        send_frame(health_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
        send_frame(health_fd, make_request({"version"}, 901));
        const auto health = std::get<proto::Result>(read_frame(health_reader));
        CHECK(health.id == 901);
        CHECK(health.data["version"] == "9.9.9");
        ::close(health_fd);
    }
}

TEST_CASE("terminal response releases same-connection admission before worker cleanup",
          "[server][lifecycle][race]") {
    TerminalThenBlockCommand blocked;
    const TestDaemon daemon(
        [&blocked](daemon::Dispatcher& dispatcher) { blocked.install(dispatcher); });
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"terminal", "then", "block"}, 910));
    const auto first = std::get<proto::Result>(read_frame(reader));
    CHECK(first.id == 910);
    blocked.wait_for_terminal();

    send_frame(fd, make_request({"version"}, 911));
    const auto second = read_frame(reader);
    REQUIRE(std::holds_alternative<proto::Result>(second));
    CHECK(std::get<proto::Result>(second).id == 911);

    blocked.release_and_wait();
    ::close(fd);
}

TEST_CASE("non-TTY challenge path emits structured input-required without a prompt",
          "[server][challenge]") {
    const TestDaemon daemon(install_challenge_command);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    auto no_tty = make_request({"challenge", "socket"}, 76);
    no_tty.context.tty = false;
    send_frame(fd, no_tty);
    const auto terminal = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&terminal);
    REQUIRE(error != nullptr);
    CHECK(error->code == "AUTH_INPUT_REQUIRED");
    CHECK(error->exit_code == kNotAuthed);
    ::close(fd);
}

TEST_CASE("request deadline terminates a socket challenge once", "[server][challenge]") {
    const TestDaemon daemon(install_challenge_command);
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    auto timed = make_request({"challenge", "socket"}, 77);
    timed.context.tty = true;
    timed.context.timeout_seconds = 0.02;
    send_frame(fd, timed);
    CHECK(std::holds_alternative<proto::Challenge>(read_frame(reader)));
    const auto terminal = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&terminal);
    REQUIRE(error != nullptr);
    CHECK(error->code == "TIMEOUT");
    CHECK(error->exit_code == kTimeout);
    ::close(fd);
}

TEST_CASE("socket and direct dispatch share finite and unlimited deadline policy",
          "[server][dispatch][deadline][unlimited]") {
    const auto install = [](daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "fetch", {daemon::Tier::Read,
                      [](const proto::Request&, daemon::RequestSession& session) {
                          session.result({{"finite", session.deadline().expires_at.has_value()}});
                      },
                      false, std::nullopt, DeadlineDefault::Unlimited});
    };
    const TestDaemon test_daemon(install, false);

    auto fetch = make_request({"fetch"}, 771);
    auto socket_result = std::get<proto::Result>(send_request(test_daemon, fetch));
    CHECK(socket_result.data == json{{"finite", false}});

    std::optional<json> direct_result;
    daemon::CallbackSink sink([](const json&) {}, [](const json&) {},
                              [&direct_result](json data) { direct_result = std::move(data); },
                              [](const std::string&, const std::string&, const json&, int) {});
    test_daemon.dispatcher.dispatch(fetch, sink);
    CHECK(direct_result == socket_result.data);

    fetch.id = 772;
    fetch.context.timeout_seconds = 0.25;
    socket_result = std::get<proto::Result>(send_request(test_daemon, fetch));
    CHECK(socket_result.data == json{{"finite", true}});
}

TEST_CASE("config admission and handlers observe one deadline tag",
          "[server][config-runtime][admission][deadline]") {
    using Rep = RequestClock::duration::rep;
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("30"));
    std::atomic<Rep> admitted_ticks{0};
    std::atomic<bool> admitted_unlimited{false};
    auto hooks = std::make_shared<daemon::testing::ConfigRuntimeHooks>();
    hooks->admission_deadline = [&](const RequestDeadline& deadline) {
        admitted_unlimited.store(!deadline.expires_at, std::memory_order_release);
        if (deadline.expires_at) {
            admitted_ticks.store(deadline.expires_at->time_since_epoch().count(),
                                 std::memory_order_release);
        }
    };
    daemon::ConfigRuntime runtime(config.file(), hooks);
    const auto install = [&](daemon::Dispatcher& dispatcher) {
        const auto handler = [&](const proto::Request&, daemon::RequestSession& session) {
            const bool same = session.deadline().expires_at
                                  ? !admitted_unlimited.load(std::memory_order_acquire) &&
                                        session.deadline().expires_at->time_since_epoch().count() ==
                                            admitted_ticks.load(std::memory_order_acquire)
                                  : admitted_unlimited.load(std::memory_order_acquire);
            session.result({{"same", same}});
        };
        dispatcher.register_command("ordinary deadline", {daemon::Tier::Read, handler});
        dispatcher.register_command("fetch", {daemon::Tier::Read, handler, false, std::nullopt,
                                              DeadlineDefault::Unlimited});
    };
    const TestDaemon test_daemon(install, false, {}, "main", {}, {}, &runtime);

    auto finite = make_request({"ordinary", "deadline"}, 773, "main");
    finite.context.timeout_seconds = 5.0;
    CHECK(std::get<proto::Result>(send_request(test_daemon, finite)).data == json{{"same", true}});

    auto unlimited = make_request({"fetch"}, 774, "main");
    CHECK(std::get<proto::Result>(send_request(test_daemon, unlimited)).data ==
          json{{"same", true}});
}

TEST_CASE("socket admission wall clock survives a logically delayed config refresh",
          "[server][config-runtime][admission][wall-clock][fetch]") {
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("30"));
    std::atomic<bool> config_finished = false;
    auto hooks = std::make_shared<daemon::testing::ConfigRuntimeHooks>();
    hooks->admission_finished = [&](daemon::ConfigRefreshStatus) {
        config_finished.store(true, std::memory_order_release);
    };
    daemon::ConfigRuntime runtime(config.file(), hooks);

    const auto admitted_at = std::chrono::system_clock::time_point{10'000s + 500ms};
    const auto after_config = admitted_at + 2h;
    std::atomic<unsigned> wall_clock_reads = 0;
    const auto wall_clock = [&] {
        wall_clock_reads.fetch_add(1, std::memory_order_relaxed);
        return config_finished.load(std::memory_order_acquire) ? after_config : admitted_at;
    };
    const auto install = [&](daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "fetch", {daemon::Tier::Read,
                      [&](const proto::Request&, daemon::RequestSession& session) {
                          const auto since =
                              daemon::parse_read_timestamp("1h", daemon::ReadTimestampBound::Since,
                                                           session.admission_wall_time());
                          session.result({{"since", since ? json(*since) : json(nullptr)},
                                          {"config_finished",
                                           config_finished.load(std::memory_order_acquire)}});
                      },
                      false, std::nullopt, DeadlineDefault::Unlimited});
    };
    const TestDaemon test_daemon(install, false, {}, "main", {}, {}, &runtime, {}, wall_clock);

    const auto terminal = send_request(test_daemon, make_request({"fetch"}, 775, "main"));
    REQUIRE(std::holds_alternative<proto::Result>(terminal));
    CHECK(std::get<proto::Result>(terminal).data ==
          json{{"since", 6'401}, {"config_finished", true}});
    CHECK(wall_clock_reads.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("daemon stop sends shutdown terminal to a challenge before EOF",
          "[server][challenge][process]") {
    TestDaemon daemon(install_challenge_command);
    const int challenged_fd = connect_to(daemon.socket);
    proto::FrameReader challenged_reader(challenged_fd);
    read_frame(challenged_reader);
    send_frame(challenged_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(challenged_fd, make_request({"challenge", "socket"}, 78));
    CHECK(std::holds_alternative<proto::Challenge>(read_frame(challenged_reader)));

    const int stop_fd = connect_to(daemon.socket);
    proto::FrameReader stop_reader(stop_fd);
    read_frame(stop_reader);
    send_frame(stop_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    std::thread shutdown([&daemon] {
        daemon.server.wait_for_stop();
        daemon.server.stop();
    });
    send_frame(stop_fd, make_request({"daemon", "stop"}, 79));
    CHECK(std::holds_alternative<proto::Result>(read_frame(stop_reader)));
    const auto terminal = read_frame(challenged_reader);
    const auto* error = std::get_if<proto::Error>(&terminal);
    REQUIRE(error != nullptr);
    CHECK(error->code == "DAEMON_SHUTDOWN");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string io_error;
    CHECK_FALSE(challenged_reader.read_line_until(deadline, io_error).has_value());
    shutdown.join();
    ::close(stop_fd);
    ::close(challenged_fd);
}

TEST_CASE("repeated QR progress updates remain display-only", "[server][challenge]") {
    const TestDaemon daemon([](daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "qr progress",
            {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& session) {
                 session.progress(
                     {{"kind", "auth_qr"}, {"auth_sequence", 3}, {"link", "tg://first"}});
                 session.progress(
                     {{"kind", "auth_qr"}, {"auth_sequence", 4}, {"link", "tg://second"}});
                 session.result({{"displayed", 2}});
             }});
    });
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"qr", "progress"}, 80));
    const auto first = std::get<proto::Progress>(read_frame(reader));
    const auto second = std::get<proto::Progress>(read_frame(reader));
    CHECK(first.data["link"] == "tg://first");
    CHECK(second.data["link"] == "tg://second");
    CHECK(std::holds_alternative<proto::Result>(read_frame(reader)));
    ::close(fd);
}

TEST_CASE("hello handshake then version round-trip over the socket", "[server]") {
    const TestDaemon daemon;
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);

    const auto hello_frame = read_frame(reader);
    const auto* hello = std::get_if<proto::Hello>(&hello_frame);
    REQUIRE(hello != nullptr);
    CHECK(hello->binary_version == "9.9.9");
    CHECK(hello->protocol_version == proto::kProtocolVersion);

    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"version"}, 42));

    const auto response = read_frame(reader);
    const auto* result = std::get_if<proto::Result>(&response);
    REQUIRE(result != nullptr);
    CHECK(result->id == 42);
    CHECK(result->data["version"] == "9.9.9");
    CHECK(result->data["tdlib"] == "1.2.3");
    ::close(fd);
}

TEST_CASE("same-v3 stale binary rejects every noncanonical request before observation",
          "[server][protocol-v3][binary-mismatch][safety]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("test", observations);
    const auto observer = observations.observer();
    const TestDaemon daemon({}, true, {}, "test", observer, probe.admission_probe());
    observations.reset();

    const auto request_document = [](std::vector<std::string> command) {
        return json::parse(proto::serialize(make_request(std::move(command), 1, "test")));
    };
    std::vector<std::pair<std::string, json>> cases{
        {"version", request_document({"version"})},
        {"me", request_document({"me"})},
        {"logout", request_document({"logout"})},
        {"account remove", request_document({"account", "remove"})},
        {"arbitrary M3", request_document({"send"})},
        {"stop alias", request_document({"daemon", "shutdown"})},
    };
    cases[4].second["context"]["idempotency_key"] = "opaque-m3-key";

    proto::Request canonical_stop("test");
    canonical_stop.id = 1;
    canonical_stop.command = {"daemon", "stop"};
    canonical_stop.context.cwd = "/";
    const auto stop_document = json::parse(proto::serialize(canonical_stop));
    auto invalid_stop_args = stop_document;
    invalid_stop_args["args"]["extra"] = true;
    cases.emplace_back("stop args", std::move(invalid_stop_args));
    auto invalid_stop_context = stop_document;
    invalid_stop_context["context"]["json"] = true;
    cases.emplace_back("stop context", std::move(invalid_stop_context));
    auto invalid_stop_id = stop_document;
    invalid_stop_id["id"] = 2;
    cases.emplace_back("stop id", std::move(invalid_stop_id));
    auto invalid_stop_key = stop_document;
    invalid_stop_key["context"]["idempotency_key"] = "raw-stop-key";
    cases.emplace_back("stop key", std::move(invalid_stop_key));

    for (const auto& [name, document] : cases) {
        DYNAMIC_SECTION(name) {
            const int fd = connect_to(daemon.socket);
            proto::FrameReader reader(fd);
            read_frame(reader);
            send_frame(fd, proto::Hello{"stale-client", proto::kProtocolVersion});
            send_line(fd, document.dump());

            const auto terminal = std::get<proto::Error>(read_frame(reader));
            CHECK(terminal.id == 0);
            CHECK(terminal.code == "USAGE");
            CHECK(terminal.details == json::object());
            CHECK(terminal.message.find("opaque-m3-key") == std::string::npos);
            CHECK(terminal.message.find("raw-stop-key") == std::string::npos);
            check_eof(reader);
            check_request_observations(observations, 0);
            ::close(fd);
        }
    }
}

TEST_CASE("same-v3 stale binary rejects answers and preserves routed account precedence",
          "[server][protocol-v3][binary-mismatch][routing][safety]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("main", observations);
    const auto observer = observations.observer();
    const TestDaemon daemon({}, true, {}, "main", observer, probe.admission_probe());
    observations.reset();

    SECTION("answer") {
        const int fd = connect_to(daemon.socket);
        proto::FrameReader reader(fd);
        read_frame(reader);
        send_frame(fd, proto::Hello{"stale-client", proto::kProtocolVersion});
        send_frame(fd, proto::Answer{1,
                                     {{"nonce", "00112233445566778899aabbccddeeff"},
                                      {"sequence", 1},
                                      {"client_generation", nullptr},
                                      {"auth_sequence", nullptr},
                                      {"value", true}}});

        const auto terminal = std::get<proto::Error>(read_frame(reader));
        CHECK(terminal.id == 0);
        CHECK(terminal.code == "USAGE");
        CHECK(terminal.details == json::object());
        check_eof(reader);
        check_request_observations(observations, 0);
        ::close(fd);
    }

    SECTION("wrong routed account") {
        const int fd = connect_to(daemon.socket);
        proto::FrameReader reader(fd);
        read_frame(reader);
        send_frame(fd, proto::Hello{"stale-client", proto::kProtocolVersion});
        send_frame(fd, make_request({"version"}, 73, "work"));

        const auto terminal = std::get<proto::Error>(read_frame(reader));
        CHECK(terminal.id == 73);
        CHECK(terminal.code == "ACCOUNT_MISMATCH");
        CHECK(terminal.details == json{{"requested_account", "work"}, {"daemon_account", "main"}});
        check_eof(reader);
        check_request_observations(observations, 0);
        ::close(fd);
    }
}

TEST_CASE("same-v3 stale binary admits only the canonical routed daemon stop",
          "[server][protocol-v3][binary-mismatch][lifecycle][safety]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("test", observations);
    const auto observer = observations.observer();
    TestDaemon daemon({}, true, {}, "test", observer, probe.admission_probe());
    observations.reset();

    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"stale-client", proto::kProtocolVersion});
    proto::Request stop("test");
    stop.id = 1;
    stop.command = {"daemon", "stop"};
    stop.context.cwd = "/";
    send_frame(fd, stop);

    const auto terminal = std::get<proto::Result>(read_frame(reader));
    CHECK(terminal.id == 1);
    CHECK(terminal.data == json{{"stopping", true}});
    CHECK(daemon.server.stop_requested());
    check_request_observations(observations, 1);
    daemon.server.stop();
    check_eof(reader);
    ::close(fd);
}

TEST_CASE("same-v3 matched binary retains normal request admission",
          "[server][protocol-v3][binary-match]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("test", observations);
    const auto observer = observations.observer();
    const TestDaemon daemon({}, true, {}, "test", observer, probe.admission_probe());
    observations.reset();

    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"version"}, 74, "test"));

    const auto terminal = std::get<proto::Result>(read_frame(reader));
    CHECK(terminal.id == 74);
    CHECK(terminal.data["version"] == "9.9.9");
    check_request_observations(observations, 1);
    ::close(fd);
}

TEST_CASE("multiple sequential requests on one connection", "[server]") {
    const TestDaemon daemon;
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader); // daemon hello
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});

    for (std::uint64_t id = 1; id <= 3; ++id) {
        send_frame(fd, make_request({"version"}, id));
        const auto response = read_frame(reader);
        const auto* result = std::get_if<proto::Result>(&response);
        REQUIRE(result != nullptr);
        CHECK(result->id == id);
    }
    ::close(fd);
}

TEST_CASE("protocol mismatch gets a terminal error", "[server]") {
    const TestDaemon daemon;
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader); // daemon hello
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion + 1});

    const auto response = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&response);
    REQUIRE(error != nullptr);
    CHECK(error->code == "PROTOCOL_MISMATCH");
    // Terminal: the daemon closes the connection afterwards.
    std::string io_error;
    CHECK_FALSE(reader.read_line(io_error).has_value());
    ::close(fd);
}

TEST_CASE("a request before hello is rejected", "[server]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("main", observations);
    const auto observer = observations.observer();
    const TestDaemon daemon([&probe](daemon::Dispatcher& dispatcher) { probe.install(dispatcher); },
                            false, {}, "main", observer, probe.admission_probe());
    observations.reset();
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader); // daemon hello
    send_frame(fd, make_request({"observe"}, 55, "work"));

    const auto response = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&response);
    REQUIRE(error != nullptr);
    CHECK(error->id == 0);
    CHECK(error->code == "USAGE");
    CHECK(error->details == json::object());
    check_eof(reader);
    check_request_observations(observations, 0);
    ::close(fd);
}

TEST_CASE("routed account mismatch is the sole terminal before request admission",
          "[server][account][routing][race]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("main", observations);
    const auto observer = observations.observer();
    const TestDaemon daemon([&probe](daemon::Dispatcher& dispatcher) { probe.install(dispatcher); },
                            false, {}, "main", observer, probe.admission_probe());
    observations.reset();

    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader);
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"observe"}, 56, "work"));

    const auto terminal = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&terminal);
    REQUIRE(error != nullptr);
    CHECK(error->id == 56);
    CHECK(error->code == "ACCOUNT_MISMATCH");
    CHECK(error->exit_code == kNotFound);
    CHECK(error->details == json{{"requested_account", "work"}, {"daemon_account", "main"}});
    check_eof(reader);
    check_request_observations(observations, 0);
    ::close(fd);
}

TEST_CASE("strict request mutations are connection-scoped malformed frames",
          "[server][proto][account][mutation]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("main", observations);
    const auto observer = observations.observer();
    const TestDaemon daemon([&probe](daemon::Dispatcher& dispatcher) { probe.install(dispatcher); },
                            false, {}, "main", observer, probe.admission_probe());
    observations.reset();
    const auto valid = json::parse(proto::serialize(make_request({"version"}, 57, "main")));
    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value.erase("account"); },
             [](json& value) { value["unknown"] = true; },
             [](json& value) { value["account"] = json::array(); },
             [](json& value) { value["account"] = ""; },
             [](json& value) { value["account"] = std::string(33, 'x'); },
             [](json& value) { value["account"] = "bad.name"; },
             [](json& value) { value["account"] = "w\xC3\xB6rk"; },
             [](json& value) { value["context"].erase("idempotency_key"); },
             [](json& value) { value["context"]["extra"] = true; },
             [](json& value) { value["context"]["timeout"] = 0; },
             [](json& value) { value["context"]["timeout"] = 1.7976931348623157e308; },
             [](json& value) { value["context"]["idempotency_key"] = "raw/key-sentinel"; },
             [](json& value) { value["context"]["idempotency_key"] = "valid-key"; },
             [](json& value) {
                 value["command"] = json::array({"send"});
                 value["context"]["idempotency_key"] = "valid-key";
                 value["context"]["dry_run"] = true;
             }}) {
        auto invalid = valid;
        mutate(invalid);
        const int fd = connect_to(daemon.socket);
        proto::FrameReader reader(fd);
        read_frame(reader);
        send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
        send_line(fd, invalid.dump());

        const auto terminal = read_frame(reader);
        const auto* error = std::get_if<proto::Error>(&terminal);
        REQUIRE(error != nullptr);
        INFO(invalid.dump());
        CHECK(error->id == 0);
        CHECK(error->code == "USAGE");
        CHECK(error->details == json::object());
        CHECK(error->message.find("raw/key-sentinel") == std::string::npos);
        check_eof(reader);
        check_request_observations(observations, 0);
        ::close(fd);
    }
}

TEST_CASE("strict Hello mutations are connection-scoped before every request observation",
          "[server][proto][protocol-v3][mutation]") {
    RequestObservationCounters observations;
    RoutedAccountProbe probe("main", observations);
    const auto observer = observations.observer();
    const TestDaemon daemon([&probe](daemon::Dispatcher& dispatcher) { probe.install(dispatcher); },
                            false, {}, "main", observer, probe.admission_probe());
    observations.reset();
    const auto valid =
        json::parse(proto::serialize(proto::Hello{"9.9.9", proto::kProtocolVersion}));
    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value.erase("binary_version"); },
             [](json& value) { value.erase("protocol_version"); },
             [](json& value) { value["extra"] = nullptr; }}) {
        auto invalid = valid;
        mutate(invalid);
        const int fd = connect_to(daemon.socket);
        proto::FrameReader reader(fd);
        read_frame(reader);
        send_line(fd, invalid.dump());

        const auto terminal = std::get<proto::Error>(read_frame(reader));
        CHECK(terminal.id == 0);
        CHECK(terminal.code == "USAGE");
        CHECK(terminal.details == json::object());
        check_eof(reader);
        check_request_observations(observations, 0);
        ::close(fd);
    }
}

TEST_CASE("crossed account requests leave both daemons isolated and healthy",
          "[server][account][routing][process]") {
    RequestObservationCounters main_observations;
    RequestObservationCounters work_observations;
    RoutedAccountProbe main_probe("main", main_observations);
    RoutedAccountProbe work_probe("work", work_observations);
    const TestDaemon main_daemon(
        [&main_probe](daemon::Dispatcher& dispatcher) { main_probe.install(dispatcher); }, false,
        {}, "main", main_observations.observer(), main_probe.admission_probe());
    const TestDaemon work_daemon(
        [&work_probe](daemon::Dispatcher& dispatcher) { work_probe.install(dispatcher); }, false,
        {}, "work", work_observations.observer(), work_probe.admission_probe());
    main_observations.reset();
    work_observations.reset();

    for (const auto& [target, requested] : {std::pair{&main_daemon, std::string("work")},
                                            std::pair{&work_daemon, std::string("main")}}) {
        const int fd = connect_to(target->socket);
        proto::FrameReader reader(fd);
        read_frame(reader);
        send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
        send_frame(fd, make_request({"observe"}, 58, requested));
        const auto terminal = std::get<proto::Error>(read_frame(reader));
        CHECK(terminal.code == "ACCOUNT_MISMATCH");
        CHECK(terminal.details["requested_account"] == requested);
        CHECK(terminal.details["daemon_account"] == target->account);
        check_eof(reader);
        ::close(fd);
    }
    check_request_observations(main_observations, 0);
    check_request_observations(work_observations, 0);

    const auto send_correct_request = [](const TestDaemon& target) {
        const int fd = connect_to(target.socket);
        proto::FrameReader reader(fd);
        read_frame(reader);
        send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
        send_frame(fd, make_request({"observe"}, 59, target.account));
        const auto terminal = std::get<proto::Result>(read_frame(reader));
        CHECK(terminal.data["account"] == target.account);
        ::close(fd);
    };

    send_correct_request(main_daemon);
    check_request_observations(main_observations, 1);
    check_request_observations(work_observations, 0);

    send_correct_request(work_daemon);
    check_request_observations(main_observations, 1);
    check_request_observations(work_observations, 1);
}

TEST_CASE("real server admission retains last-good settings and rejects only standing grants",
          "[server][config-runtime][admission][destructive]") {
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("30", true, "old-password-hook"));
    daemon::ConfigRuntime runtime(config.file());
    const TestDaemon daemon(
        [](daemon::Dispatcher& dispatcher) { AdmissionInspector::install(dispatcher); }, false, {},
        "main", {}, {}, &runtime);

    auto request = make_request({"inspect", "admission"}, 80, "main");
    auto first = std::get<proto::Result>(send_request(daemon, request));
    CHECK(first.data["authority"] == "config");
    CHECK(first.data["standing_grant_valid"] == true);
    CHECK(first.data["password_cmd"] == "old-password-hook");
    CHECK(first.data["account_count"] == 1);

    config.replace("[accounts.main\n");
    request.id = 81;
    auto invalid_standing = std::get<proto::Result>(send_request(daemon, request));
    CHECK(invalid_standing.data["authority"] == "invalid_config_grant");
    CHECK(invalid_standing.data["allow_write"] == true);
    CHECK(invalid_standing.data["standing_grant_valid"] == false);
    CHECK(invalid_standing.data["password_cmd"] == "old-password-hook");
    CHECK(invalid_standing.data["account_count"] == 1);
    CHECK(invalid_standing.data["reload_reason"] == "parse_error");

    request.id = 82;
    request.context.write_authority = proto::WriteAuthority::Grant;
    auto explicit_grant = std::get<proto::Result>(send_request(daemon, request));
    CHECK(explicit_grant.data["authority"] == "request");
    CHECK(explicit_grant.data["standing_grant_valid"] == false);

    request.id = 83;
    request.context.write_authority = proto::WriteAuthority::Deny;
    auto explicit_deny = std::get<proto::Result>(send_request(daemon, request));
    CHECK(explicit_deny.data["authority"] == "explicit_deny");
}

TEST_CASE("config and activity rejection boundaries precede session construction",
          "[server][config-runtime][admission][ordering]") {
    const RuntimeConfig config;
    config.write_initial("[accounts.main\n");
    daemon::ConfigRuntime runtime(config.file());
    RequestObservationCounters observations;
    const TestDaemon daemon({}, false, {}, "main", observations.observer(), {}, &runtime);
    observations.reset();

    const auto terminal =
        std::get<proto::Error>(send_request(daemon, make_request({"never"}, 84, "main")));
    CHECK(terminal.code == "CONFIG_INVALID");
    CHECK(observations.get(daemon::testing::RequestObservationStage::ConfigRead) == 1);
    CHECK(observations.get(daemon::testing::RequestObservationStage::ActivityAdmission) == 0);
    CHECK(observations.get(daemon::testing::RequestObservationStage::SessionConstruction) == 0);
    CHECK(observations.get(daemon::testing::RequestObservationStage::DispatcherLookup) == 0);
}

TEST_CASE("active request keeps its immutable snapshot and prevents shortened idle expiry",
          "[server][config-runtime][activity][idle][race]") {
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("30"));
    daemon::ConfigRuntime runtime(config.file());
    const auto initial = runtime.current("main");
    BlockingAdmissionCommand blocking;
    TestDaemon daemon([&blocking](daemon::Dispatcher& dispatcher) { blocking.install(dispatcher); },
                      false, {}, "main", {}, {}, &runtime);

    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    static_cast<void>(read_frame(reader));
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"block", "admission"}, 85, "main"));
    blocking.wait_until_entered();

    const auto replaced_at = std::chrono::steady_clock::now();
    config.replace(runtime_account_config("1"));
    REQUIRE(wait_for_condition(1900ms, [&] {
        const auto current = runtime.current("main");
        return current.generation > initial.generation && current.idle_exit == 1s;
    }));
    CHECK(std::chrono::steady_clock::now() - replaced_at < 2s);
    std::this_thread::sleep_for(1100ms);
    CHECK_FALSE(daemon.server.stop_requested());

    blocking.unblock();
    const auto old_terminal = std::get<proto::Result>(read_frame(reader));
    CHECK(old_terminal.data["idle_exit"] == 30);
    ::close(fd);
    REQUIRE(wait_for_condition(1500ms, [&] { return daemon.server.stop_requested(); }));
}

TEST_CASE("idle config reload shortens the original zero transition and stops without traffic",
          "[server][config-runtime][activity][idle][integration]") {
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("30"));
    daemon::ConfigRuntime runtime(config.file());
    TestDaemon daemon({}, false, {}, "main", {}, {}, &runtime);

    std::this_thread::sleep_for(1100ms);
    const auto replaced_at = std::chrono::steady_clock::now();
    config.replace(runtime_account_config("1"));
    REQUIRE(wait_for_condition(1900ms, [&] { return daemon.server.stop_requested(); }));
    CHECK(std::chrono::steady_clock::now() - replaced_at < 2s);
}

TEST_CASE("an open connection without an admitted request is not idle activity",
          "[server][config-runtime][activity][idle]") {
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("1"));
    daemon::ConfigRuntime runtime(config.file());
    TestDaemon daemon({}, false, {}, "main", {}, {}, &runtime);

    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    static_cast<void>(read_frame(reader));
    REQUIRE(wait_for_condition(1500ms, [&] { return daemon.server.stop_requested(); }));
    daemon.server.stop();
    check_eof(reader);
    ::close(fd);
}

TEST_CASE("daemon stop cancels a forced config admission without waiting for reload",
          "[server][config-runtime][activity][cancel][race]") {
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("30"));
    ForcedReloadGate gate;
    auto hooks = std::make_shared<daemon::testing::ConfigRuntimeHooks>();
    hooks->before_reload = [&gate](bool forced) { gate.before_reload(forced); };
    daemon::ConfigRuntime runtime(config.file(), hooks);
    TestDaemon daemon({}, false, {}, "main", {}, {}, &runtime);

    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    static_cast<void>(read_frame(reader));
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"never"}, 86, "main"));
    gate.wait_until_entered();

    const auto stopped_at = std::chrono::steady_clock::now();
    daemon.server.request_stop();
    const auto terminal = std::get<proto::Error>(read_frame(reader));
    CHECK(terminal.code == "DAEMON_SHUTDOWN");
    CHECK(std::chrono::steady_clock::now() - stopped_at < 500ms);
    daemon.server.stop();
    CHECK(std::chrono::steady_clock::now() - stopped_at < 500ms);
    gate.release();
    ::close(fd);
}

TEST_CASE("socket EOF cancels Unlimited config admission before session construction",
          "[server][config-runtime][deadline][unlimited][disconnect]") {
    const RuntimeConfig config;
    config.write_initial(runtime_account_config("30"));
    ForcedReloadGate gate;
    AdmissionFinishProbe finished;
    auto hooks = std::make_shared<daemon::testing::ConfigRuntimeHooks>();
    hooks->before_reload = [&gate](bool forced) { gate.before_reload(forced); };
    hooks->admission_finished = [&finished](daemon::ConfigRefreshStatus status) {
        finished.notify(status);
    };
    daemon::ConfigRuntime runtime(config.file(), hooks);
    RequestObservationCounters observations;
    std::atomic<int> handlers = 0;
    const auto install = [&handlers](daemon::Dispatcher& dispatcher) {
        dispatcher.register_command(
            "fetch", {daemon::Tier::Read,
                      [&handlers](const proto::Request&, daemon::RequestSession& session) {
                          handlers.fetch_add(1, std::memory_order_relaxed);
                          session.result({{"unexpected", true}});
                      },
                      false, std::nullopt, DeadlineDefault::Unlimited});
    };
    const TestDaemon daemon(install, false, {}, "main", observations.observer(), {}, &runtime);

    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    static_cast<void>(read_frame(reader));
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"fetch"}, 87, "main"));
    gate.wait_until_entered();

    REQUIRE(::shutdown(fd, SHUT_RDWR) == 0);
    ::close(fd);
    const bool cancelled = finished.wait_for(daemon::ConfigRefreshStatus::Cancelled, 100ms);
    CHECK(cancelled);
    CHECK(observations.get(daemon::testing::RequestObservationStage::SessionConstruction) == 0);
    CHECK(handlers.load(std::memory_order_relaxed) == 0);

    gate.release();
    if (!cancelled) {
        static_cast<void>(finished.wait_for(daemon::ConfigRefreshStatus::Completed, 2s));
    }
}

TEST_CASE("malformed frame gets a USAGE error and a closed connection", "[server]") {
    const TestDaemon daemon;
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader); // daemon hello

    constexpr std::string_view garbage = "this is not json\n";
    REQUIRE(::write(fd, garbage.data(), garbage.size()) > 0);

    const auto response = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&response);
    REQUIRE(error != nullptr);
    CHECK(error->code == "USAGE");
    std::string io_error;
    CHECK_FALSE(reader.read_line(io_error).has_value());
    ::close(fd);
}

TEST_CASE("daemon stop over the socket flags shutdown", "[server]") {
    TestDaemon daemon;
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader); // daemon hello
    send_frame(fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(fd, make_request({"daemon", "stop"}));

    const auto response = read_frame(reader);
    const auto* result = std::get_if<proto::Result>(&response);
    REQUIRE(result != nullptr);
    CHECK(result->data["stopping"] == true);
    CHECK(daemon.server.stop_requested());
    ::close(fd);
    daemon.server.stop();
    // The socket file is gone after a graceful stop.
    CHECK(::access(daemon.socket.c_str(), F_OK) != 0);
}

TEST_CASE("daemon stop gives every active request exactly one terminal frame",
          "[server][process]") {
    BlockingCommand state;
    TestDaemon daemon([&state](daemon::Dispatcher& dispatcher) { state.install(dispatcher); });

    const int blocked_fd = connect_to(daemon.socket);
    proto::FrameReader blocked_reader(blocked_fd);
    read_frame(blocked_reader);
    send_frame(blocked_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(blocked_fd, make_request({"block"}, 11));

    state.wait_until_entered();

    const int stopping_fd = connect_to(daemon.socket);
    proto::FrameReader stopping_reader(stopping_fd);
    read_frame(stopping_reader);
    send_frame(stopping_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});

    std::thread shutdown_thread([&daemon] {
        daemon.server.wait_for_stop();
        daemon.server.stop();
    });
    send_frame(stopping_fd, make_request({"daemon", "stop"}, 22));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto stopping_terminal = read_frame_until(stopping_reader, deadline);
    const auto* stopping_result = std::get_if<proto::Result>(&stopping_terminal);
    REQUIRE(stopping_result != nullptr);
    CHECK(stopping_result->id == 22);
    CHECK(stopping_result->data == json{{"stopping", true}});

    const auto blocked_terminal = read_frame_until(blocked_reader, deadline);
    const auto* shutdown_error = std::get_if<proto::Error>(&blocked_terminal);
    REQUIRE(shutdown_error != nullptr);
    CHECK(shutdown_error->id == 11);
    CHECK(shutdown_error->code == "DAEMON_SHUTDOWN");
    CHECK(shutdown_error->message == "daemon is shutting down");
    CHECK(shutdown_error->details == json{{"reason", "daemon_shutdown"}});
    CHECK(shutdown_error->exit_code == kGeneric);

    for (auto* reader : {&stopping_reader, &blocked_reader}) {
        std::string io_error;
        CHECK_FALSE(reader->read_line_until(deadline, io_error).has_value());
        CHECK(io_error.empty());
    }
    state.release_and_wait();
    shutdown_thread.join();
    ::close(stopping_fd);
    ::close(blocked_fd);
}

TEST_CASE("disconnect racing daemon stop cannot block the stop requester", "[server]") {
    BlockingCommand state;
    TestDaemon daemon([&state](daemon::Dispatcher& dispatcher) { state.install(dispatcher); });

    const int blocked_fd = connect_to(daemon.socket);
    proto::FrameReader blocked_reader(blocked_fd);
    read_frame(blocked_reader);
    send_frame(blocked_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(blocked_fd, make_request({"block"}, 31));
    state.wait_until_entered();
    ::shutdown(blocked_fd, SHUT_RDWR);
    ::close(blocked_fd);

    const int stopping_fd = connect_to(daemon.socket);
    proto::FrameReader stopping_reader(stopping_fd);
    read_frame(stopping_reader);
    send_frame(stopping_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(stopping_fd, make_request({"daemon", "stop"}, 32));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto stopping_terminal = read_frame_until(stopping_reader, deadline);
    const auto* stopping_result = std::get_if<proto::Result>(&stopping_terminal);
    REQUIRE(stopping_result != nullptr);
    CHECK(stopping_result->id == 32);
    CHECK(stopping_result->data == json{{"stopping", true}});

    state.release_and_wait();
    daemon.server.stop();

    std::string io_error;
    CHECK_FALSE(stopping_reader.read_line_until(deadline, io_error).has_value());
    CHECK(io_error.empty());
    ::close(stopping_fd);
}

TEST_CASE("admitted daemon stop keeps its success when external shutdown races",
          "[server][lifecycle]") {
    BlockingCommand blocked;
    BlockingStopCommand stopping;
    TestDaemon daemon(
        [&blocked, &stopping](daemon::Dispatcher& dispatcher) {
            blocked.install(dispatcher);
            stopping.install(dispatcher);
        },
        false);

    const int blocked_fd = connect_to(daemon.socket);
    proto::FrameReader blocked_reader(blocked_fd);
    read_frame(blocked_reader);
    send_frame(blocked_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(blocked_fd, make_request({"block"}, 41));
    blocked.wait_until_entered();

    const int stopping_fd = connect_to(daemon.socket);
    proto::FrameReader stopping_reader(stopping_fd);
    read_frame(stopping_reader);
    send_frame(stopping_fd, proto::Hello{"9.9.9", proto::kProtocolVersion});
    send_frame(stopping_fd, make_request({"daemon", "stop"}, 42));
    stopping.wait_until_entered();

    std::latch shutdown_sweep_complete(1);
    std::thread shutdown_thread([&daemon, &shutdown_sweep_complete] {
        daemon.server.request_stop();
        shutdown_sweep_complete.count_down();
        daemon.server.stop();
    });
    shutdown_sweep_complete.wait();

    stopping.release_and_wait();
    blocked.release_and_wait();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto stopping_terminal = read_frame_until(stopping_reader, deadline);
    const auto blocked_terminal = read_frame_until(blocked_reader, deadline);
    shutdown_thread.join();

    const auto* stopping_result = std::get_if<proto::Result>(&stopping_terminal);
    REQUIRE(stopping_result != nullptr);
    CHECK(stopping_result->id == 42);
    CHECK(stopping_result->data == json{{"stopping", true}});

    const auto* shutdown_error = std::get_if<proto::Error>(&blocked_terminal);
    REQUIRE(shutdown_error != nullptr);
    CHECK(shutdown_error->id == 41);
    CHECK(shutdown_error->code == "DAEMON_SHUTDOWN");
    CHECK(shutdown_error->details == json{{"reason", "daemon_shutdown"}});

    for (auto* reader : {&stopping_reader, &blocked_reader}) {
        std::string io_error;
        CHECK_FALSE(reader->read_line_until(deadline, io_error).has_value());
        CHECK(io_error.empty());
    }
    ::close(stopping_fd);
    ::close(blocked_fd);
}
