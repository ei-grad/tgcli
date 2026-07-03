// Socket-level round-trip: a real Server on a real unix socket, a raw client
// speaking the frame protocol. td-free (fake context), so TSan covers it.

#include "common/exit_codes.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/server.hpp"
#include "proto/frame_io.hpp"

#include <cstring>
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
    return "/tmp/tgcli-test-" + std::to_string(getpid()) + ".sock";
}

int connect_to(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

proto::Frame read_frame(proto::FrameReader& reader) {
    std::string io_error;
    const auto line = reader.read_line(io_error);
    INFO("io error: " << io_error);
    REQUIRE(line.has_value());
    std::string parse_error;
    auto frame = proto::parse(*line, parse_error);
    INFO("parse error: " << parse_error);
    REQUIRE(frame.has_value());
    return std::move(*frame);
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

    TestDaemon() : server({socket, "9.9.9", proto::kProtocolVersion}, dispatcher) {
        context.account = "test";
        context.binary_version = "9.9.9";
        context.protocol_version = proto::kProtocolVersion;
        context.tdlib_version = "1.2.3";
        context.socket_path = socket;
        context.request_shutdown = [this] { server.request_stop(); };
        daemon::register_commands(dispatcher, context);
        std::string error;
        INFO("server start error: " << error);
        REQUIRE(server.start(error));
    }

    ~TestDaemon() {
        server.stop();
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
