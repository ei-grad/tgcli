// Socket-level round-trip: a real Server on a real unix socket, a raw client
// speaking the frame protocol. td-free (fake context), so TSan covers it.

#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/server.hpp"
#include "proto/frame_io.hpp"

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
            {daemon::Tier::Read, [this](const proto::Request&, daemon::ResponseSink& sink) {
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
            {daemon::Tier::Read, [this](const proto::Request&, daemon::ResponseSink& sink) {
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

} // namespace

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
