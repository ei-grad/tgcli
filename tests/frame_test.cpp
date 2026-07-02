#include "proto/frame.hpp"

#include <array>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli::proto;
using nlohmann::json;

namespace {

Frame round_trip(const Frame& frame) {
    std::string error;
    auto parsed = parse(serialize(frame), error);
    INFO("parse error: " << error);
    REQUIRE(parsed.has_value());
    return *parsed;
}

Request make_request() {
    Request req;
    req.id = 42;
    req.command = {"msg", "delete"};
    req.args = json{{"chat", "@dev"}, {"ids", json::array({1, 2})}};
    req.context.tty = true;
    req.context.json = true;
    req.context.yes = false;
    req.context.dry_run = false;
    req.context.timeout_seconds = 30.5;
    req.context.cwd = "/home/user";
    req.context.media_dir = "/data/media";
    req.context.write_authority = WriteAuthority::Deny;
    return req;
}

} // namespace

TEST_CASE("serialized frames are single-line JSON", "[proto]") {
    for (const Frame& frame :
         {Frame{Hello{"0.1.0", 1}}, Frame{make_request()}, Frame{Result{1, json{{"ok", true}}}},
          Frame{Error{2, "DENIED", "no grant", json::object(), 6}}}) {
        auto line = serialize(frame);
        CHECK(line.find('\n') == std::string::npos);
        CHECK(json::parse(line).is_object());
    }
}

TEST_CASE("hello round-trip", "[proto]") {
    auto frame = round_trip(Hello{"0.1.0", kProtocolVersion});
    auto& hello = std::get<Hello>(frame);
    CHECK(hello.binary_version == "0.1.0");
    CHECK(hello.protocol_version == kProtocolVersion);
}

TEST_CASE("request round-trip preserves context", "[proto]") {
    auto frame = round_trip(make_request());
    auto& req = std::get<Request>(frame);
    CHECK(req.id == 42);
    CHECK(req.command == std::vector<std::string>{"msg", "delete"});
    CHECK(req.args["chat"] == "@dev");
    CHECK(req.context.tty);
    CHECK(req.context.json);
    CHECK_FALSE(req.context.yes);
    CHECK_FALSE(req.context.dry_run);
    CHECK(req.context.timeout_seconds == 30.5);
    CHECK(req.context.cwd == "/home/user");
    CHECK(req.context.media_dir == "/data/media");
    CHECK(req.context.write_authority == WriteAuthority::Deny);
}

TEST_CASE("request context nullables round-trip as null", "[proto]") {
    auto req = make_request();
    req.context.timeout_seconds.reset();
    req.context.media_dir.reset();
    req.context.write_authority = WriteAuthority::Unset;

    auto doc = json::parse(serialize(req));
    CHECK(doc["context"]["timeout"].is_null());
    CHECK(doc["context"]["media_dir"].is_null());
    CHECK(doc["context"]["write_authority"] == "unset");

    auto parsed = std::get<Request>(round_trip(req));
    CHECK_FALSE(parsed.context.timeout_seconds.has_value());
    CHECK_FALSE(parsed.context.media_dir.has_value());
    CHECK(parsed.context.write_authority == WriteAuthority::Unset);
}

TEST_CASE("write authority values cover the §6 tri-state", "[proto]") {
    for (auto [authority, wire] :
         {std::pair{WriteAuthority::Grant, "grant"}, std::pair{WriteAuthority::Deny, "deny"},
          std::pair{WriteAuthority::Unset, "unset"}}) {
        auto req = make_request();
        req.context.write_authority = authority;
        auto doc = json::parse(serialize(req));
        CHECK(doc["context"]["write_authority"] == wire);
        auto parsed = std::get<Request>(round_trip(req));
        CHECK(parsed.context.write_authority == authority);
    }
}

TEST_CASE("response frames round-trip", "[proto]") {
    SECTION("result") {
        auto f = std::get<Result>(round_trip(Result{7, json{{"id", 123}}}));
        CHECK(f.id == 7);
        CHECK(f.data["id"] == 123);
    }
    SECTION("item") {
        auto f = std::get<Item>(round_trip(Item{7, json{{"text", "hi"}}}));
        CHECK(f.data["text"] == "hi");
    }
    SECTION("progress") {
        auto f = std::get<Progress>(round_trip(Progress{7, json{{"done", 10}, {"total", 100}}}));
        CHECK(f.data["done"] == 10);
    }
    SECTION("error carries code, message, details, exit code") {
        auto f = std::get<Error>(
            round_trip(Error{7, "RATE_LIMITED", "flood wait", json{{"retry_after", 42}}, 5}));
        CHECK(f.code == "RATE_LIMITED");
        CHECK(f.message == "flood wait");
        CHECK(f.details["retry_after"] == 42);
        CHECK(f.exit_code == 5);
    }
    SECTION("challenge and answer") {
        auto c = std::get<Challenge>(round_trip(Challenge{9, json{{"kind", "confirm"}}}));
        CHECK(c.challenge["kind"] == "confirm");
        auto a = std::get<Answer>(round_trip(Answer{9, json{{"confirmed", true}}}));
        CHECK(a.answer["confirmed"] == true);
    }
}

TEST_CASE("malformed input is rejected with a reason", "[proto]") {
    const auto cases = std::to_array<std::string_view>({
        "not json at all",
        "[1,2,3]",
        R"({"id":1})",
        R"({"type":"warp","id":1})",
        R"({"type":"result","data":{}})",
        R"({"type":"result","id":-1,"data":{}})",
        R"({"type":"request","id":1,"command":[],"args":{},"context":{}})",
        R"({"type":"request","id":1,"command":["me"],"args":{}})",
        R"({"type":"error","id":1,"error":{"code":"X"},"exit_code":1})",
        R"({"type":"hello","binary_version":1,"protocol_version":1})",
    });
    for (auto line : cases) {
        std::string error;
        INFO("input: " << line);
        CHECK_FALSE(parse(line, error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("write_authority outside the tri-state is rejected", "[proto]") {
    auto doc = json::parse(serialize(make_request()));
    doc["context"]["write_authority"] = "root";
    std::string error;
    CHECK_FALSE(parse(doc.dump(), error).has_value());
    CHECK(error.find("write_authority") != std::string::npos);
}
