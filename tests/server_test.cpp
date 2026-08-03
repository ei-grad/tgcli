// Socket-level round-trip: a real Server on a real unix socket, a raw client
// speaking the frame protocol. td-free (fake context), so TSan covers it.

#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "daemon/server.hpp"
#include "proto/frame_io.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <latch>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <variant>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

std::string test_socket_path() {
    // Short and per-pid: sun_path is ~104 bytes and tests may run parallel.
    return "/tmp/tgcli-test-" + std::to_string(getpid()) + "/main.sock";
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

proto::Request make_request(std::vector<std::string> command, std::uint64_t id = 1) {
    proto::Request request;
    request.id = id;
    request.command = std::move(command);
    request.context.tty = true;
    request.context.cwd = "/";
    return request;
}

struct TestDaemon {
    daemon::DaemonContext context;
    daemon::Dispatcher dispatcher;
    std::string socket = test_socket_path();
    daemon::Server server;

    TestDaemon(const TestDaemon&) = delete;
    TestDaemon& operator=(const TestDaemon&) = delete;
    TestDaemon(TestDaemon&&) = delete;
    TestDaemon& operator=(TestDaemon&&) = delete;

    explicit TestDaemon(const std::function<void(daemon::Dispatcher&)>& configure = {},
                        bool register_default_commands = true)
        : server({socket, "9.9.9", proto::kProtocolVersion, {}, {}}, dispatcher) {
        std::string error;
        const auto separator = socket.rfind('/');
        REQUIRE(separator != std::string::npos);
        REQUIRE(paths::ensure_private_dir(socket.substr(0, separator), getuid(), error));
        context.account = "test";
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
             const auto outcome = session.challenge(code_challenge());
             switch (outcome.status) {
             case daemon::ChallengeStatus::Answered:
                 if (!session.reserve_in_flight()) {
                     return;
                 }
                 session.settle_in_flight();
                 session.result({{"value", std::get<std::string>(outcome.value)}});
                 return;
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
    TestDaemon daemon(install_challenge_command);
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

TEST_CASE("socket and in-process dispatch have equivalent challenge results",
          "[server][challenge][dispatch]") {
    proto::Request in_process_request = make_request({"challenge", "socket"}, 81);
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

    TestDaemon daemon(install_challenge_command);
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
    TestDaemon daemon(install_challenge_command);
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
    TestDaemon daemon(install_challenge_command);
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
    TestDaemon daemon(install_challenge_command);
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
    TestDaemon daemon(install_challenge_command);
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
    TestDaemon daemon;
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
    TestDaemon daemon([&blocked](daemon::Dispatcher& dispatcher) { blocked.install(dispatcher); });
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
    TestDaemon daemon(install_challenge_command);
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
    TestDaemon daemon(install_challenge_command);
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
    TestDaemon daemon([](daemon::Dispatcher& dispatcher) {
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
    const TestDaemon daemon;
    const int fd = connect_to(daemon.socket);
    proto::FrameReader reader(fd);
    read_frame(reader); // daemon hello
    send_frame(fd, make_request({"version"}));

    const auto response = read_frame(reader);
    const auto* error = std::get_if<proto::Error>(&response);
    REQUIRE(error != nullptr);
    CHECK(error->code == "USAGE");
    ::close(fd);
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
