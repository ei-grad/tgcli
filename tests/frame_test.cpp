#include "proto/frame.hpp"
#include "proto/frame_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

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
    return std::move(*parsed);
}

Request make_request() {
    Request req("main");
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

json request_document_with_timeout(json timeout) {
    auto document = json::parse(serialize(make_request()));
    document["context"]["timeout"] = std::move(timeout);
    return document;
}

json destructive_challenge(std::string action, json target) {
    return {{"kind", "destructive_confirmation"},
            {"nonce", "00112233445566778899aabbccddeeff"},
            {"sequence", 1},
            {"client_generation", nullptr},
            {"auth_sequence", nullptr},
            {"secret", false},
            {"prompt", "Confirm? [y/N] "},
            {"details", {{"action", std::move(action)}, {"target", std::move(target)}}}};
}

json logout_target() {
    return {{"operation", "logout"},
            {"account", "main"},
            {"remote_logout", true},
            {"tdlib_request", "logOut"}};
}

json account_remove_target() {
    return {{"operation", "account_remove"},
            {"account", "work"},
            {"remote_logout", true},
            {"keep_session", false},
            {"delete_paths", json::array({"/data/work", "/state/work"})},
            {"config_path", "/config/tgcli/config.toml"},
            {"config_snapshot",
             "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
             "ino:2;size:3;ctime_ns:4"},
            {"data_root",
             {{"path", "/data/work"},
              {"device", std::uint64_t{1}},
              {"inode", std::uint64_t{2}},
              {"owner", std::uint64_t{1000}}}},
            {"state_root",
             {{"path", "/state/work"},
              {"device", std::uint64_t{1}},
              {"inode", std::uint64_t{3}},
              {"owner", std::uint64_t{1000}}}},
            {"reassign_default", "main"}};
}

} // namespace

TEST_CASE("serialized frames are single-line JSON", "[proto]") {
    for (const Frame& frame : {Frame{Hello{"0.1.0", kProtocolVersion}}, Frame{make_request()},
                               Frame{Result{1, json{{"ok", true}}}},
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
    const auto document = json::parse(serialize(make_request()));
    CHECK(document.size() == 6);
    for (const auto* field : {"type", "id", "account", "command", "args", "context"}) {
        CHECK(document.contains(field));
    }
    CHECK(document["account"] == "main");

    auto frame = round_trip(make_request());
    auto& req = std::get<Request>(frame);
    CHECK(req.id == 42);
    CHECK(req.account == "main");
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

TEST_CASE("request account is mandatory and uses the canonical account-name grammar",
          "[proto][account][mutation]") {
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<Request>);
    const auto valid = json::parse(serialize(make_request()));
    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value.erase("account"); },
             [](json& value) { value["extra"] = true; }, [](json& value) { value["account"] = 1; },
             [](json& value) { value["account"] = ""; },
             [](json& value) { value["account"] = std::string(33, 'a'); },
             [](json& value) { value["account"] = "bad.name"; },
             [](json& value) { value["account"] = "m\xC3\xA4in"; }}) {
        auto invalid = valid;
        mutate(invalid);
        std::string error;
        INFO(invalid.dump());
        CHECK_FALSE(parse(invalid.dump(), error));
        CHECK_FALSE(error.empty());
    }

    for (const auto& invalid : {std::string{}, std::string(33, 'a'), std::string("bad.name"),
                                std::string("m\xC3\xA4in")}) {
        CHECK_THROWS_AS(Request(invalid), std::invalid_argument);
        auto request = make_request();
        request.account = invalid;
        CHECK_THROWS_AS(serialize(request), std::invalid_argument);
    }
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
        const json challenge{{"kind", "password"},
                             {"nonce", "00112233445566778899aabbccddeeff"},
                             {"sequence", 1},
                             {"client_generation", 3},
                             {"auth_sequence", 8},
                             {"secret", true},
                             {"prompt", "Password: "},
                             {"details",
                              {{"hint", "pet"},
                               {"has_recovery_email", true},
                               {"has_passport_data", false},
                               {"recovery_email_pattern", "a***@example.com"}}}};
        auto c = std::get<Challenge>(round_trip(Challenge{9, challenge}));
        CHECK(c.challenge == challenge);
        const json answer{{"nonce", challenge["nonce"]},
                          {"sequence", 1},
                          {"client_generation", 3},
                          {"auth_sequence", 8},
                          {"value", "sentinel"}};
        auto a = std::get<Answer>(round_trip(Answer{9, json(answer)}));
        CHECK(a.answer == answer);
    }
}

TEST_CASE("challenge payloads enforce closed kinds, details, and secrecy", "[proto][challenge]") {
    const json base{{"kind", "phone_number"}, {"nonce", "00112233445566778899aabbccddeeff"},
                    {"sequence", 1},          {"client_generation", 4},
                    {"auth_sequence", 9},     {"secret", false},
                    {"prompt", "Phone: "},    {"details", json::object()}};

    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value["kind"] = "future_kind"; },
             [](json& value) { value["nonce"] = "ABCDEF"; },
             [](json& value) { value["sequence"] = 0; },
             [](json& value) { value["client_generation"] = nullptr; },
             [](json& value) { value["secret"] = true; },
             [](json& value) { value["details"]["extra"] = true; },
             [](json& value) { value["extra"] = true; }}) {
        auto invalid = base;
        mutate(invalid);
        std::string error;
        INFO(invalid.dump());
        CHECK_FALSE(parse(serialize(Challenge{7, invalid}), error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("destructive confirmation target is the exact closed plan", "[proto][challenge]") {
    for (const auto& [action, target] :
         {std::pair{std::string("logout"), logout_target()},
          std::pair{std::string("account_remove"), account_remove_target()}}) {
        std::string error;
        CHECK(validate_challenge_payload(destructive_challenge(action, target), error));
    }

    SECTION("logout rejects shape, identity, operation, and secret-bearing fields") {
        const auto base = logout_target();
        for (const auto& mutate : std::vector<std::function<void(json&)>>{
                 [](json& value) { value.erase("account"); },
                 [](json& value) { value["extra"] = true; },
                 [](json& value) { value["operation"] = "account_remove"; },
                 [](json& value) { value["account"] = "../main"; },
                 [](json& value) { value["remote_logout"] = false; },
                 [](json& value) { value["tdlib_request"] = "log_out"; },
                 [](json& value) { value["password"] = "secret"; }}) {
            auto invalid = base;
            mutate(invalid);
            std::string error;
            INFO(invalid.dump());
            CHECK_FALSE(
                validate_challenge_payload(destructive_challenge("logout", invalid), error));
        }
        std::string error;
        CHECK_FALSE(validate_challenge_payload(
            destructive_challenge("account_remove", logout_target()), error));
    }

    SECTION("account removal rejects every target and nested identity deviation") {
        const auto base = account_remove_target();
        for (const auto& mutate : std::vector<std::function<void(json&)>>{
                 [](json& value) { value.erase("config_path"); },
                 [](json& value) { value["extra"] = true; },
                 [](json& value) { value["operation"] = "logout"; },
                 [](json& value) { value["account"] = ""; },
                 [](json& value) { value["remote_logout"] = false; },
                 [](json& value) { value["keep_session"] = true; },
                 [](json& value) { value["delete_paths"] = json::array({"/data/work"}); },
                 [](json& value) { value["delete_paths"][0] = "relative"; },
                 [](json& value) { value["config_path"] = false; },
                 [](json& value) { value["config_snapshot"] = "sha256:bad"; },
                 [](json& value) {
                     value["config_snapshot"] =
                         "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
                         "dev:18446744073709551616;ino:2;size:3;ctime_ns:4";
                 },
                 [](json& value) { value["data_root"]["extra"] = true; },
                 [](json& value) { value["data_root"]["path"] = "/other"; },
                 [](json& value) { value["data_root"]["device"] = -1; },
                 [](json& value) { value["state_root"] = json::array(); },
                 [](json& value) { value["reassign_default"] = "../main"; },
                 [](json& value) { value["bot_token"] = "secret"; }}) {
            auto invalid = base;
            mutate(invalid);
            std::string error;
            INFO(invalid.dump());
            CHECK_FALSE(validate_challenge_payload(destructive_challenge("account_remove", invalid),
                                                   error));
        }
        std::string error;
        CHECK_FALSE(validate_challenge_payload(
            destructive_challenge("logout", account_remove_target()), error));
    }

    SECTION("absent roots and null reassignment retain the exact removal shape") {
        auto target = account_remove_target();
        target["data_root"] = nullptr;
        target["state_root"] = nullptr;
        target["reassign_default"] = nullptr;
        std::string error;
        CHECK(validate_challenge_payload(destructive_challenge("account_remove", target), error));
    }

    SECTION("keep-session removal is the valid opposite boolean policy") {
        auto target = account_remove_target();
        target["remote_logout"] = false;
        target["keep_session"] = true;
        std::string error;
        CHECK(validate_challenge_payload(destructive_challenge("account_remove", target), error));
    }
}

TEST_CASE("answer payloads have one exact value or cancellation shape", "[proto][challenge]") {
    const json base{{"nonce", "00112233445566778899aabbccddeeff"},
                    {"sequence", 1},
                    {"client_generation", 4},
                    {"auth_sequence", 9},
                    {"value", "12345"}};
    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value["nonce"] = "short"; },
             [](json& value) { value["sequence"] = -1; },
             [](json& value) { value["cancelled"] = true; },
             [](json& value) { value.erase("value"); },
             [](json& value) {
                 value.erase("value");
                 value["cancelled"] = false;
             },
             [](json& value) { value["value"] = json::object(); },
             [](json& value) { value["extra"] = true; }}) {
        auto invalid = base;
        mutate(invalid);
        std::string error;
        INFO(invalid.dump());
        auto encoded = serialize(Answer{7, std::move(invalid)});
        const auto parsed = parse(std::move(encoded), error);
        CHECK_FALSE(parsed.has_value());
        CHECK_FALSE(error.empty());
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
        CHECK_FALSE(parse(std::string(line), error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("strict and answer-recovery parsers never throw on malformed field types",
          "[proto][challenge]") {
    const auto cases = std::to_array<std::string_view>({
        R"({"type":null,"id":1,"answer":{}})",
        R"({"type":{},"id":1,"answer":{}})",
        R"({"type":[],"id":1,"answer":{}})",
        R"({"type":true,"id":1,"answer":{}})",
        R"({"type":"answer","id":{},"answer":{}})",
        R"({"type":"answer","id":1,"answer":null})",
        R"({"type":"answer","id":1,"answer":[]})",
        R"({"type":"hello","binary_version":"v","protocol_version":18446744073709551615})",
        R"({"type":"error","id":1,"error":{"code":"X","message":"m","details":{}},"exit_code":18446744073709551615})",
    });
    for (const auto line : cases) {
        INFO(line);
        std::string error;
        CHECK_NOTHROW(parse(std::string(line), error));
        CHECK_FALSE(parse(std::string(line), error).has_value());
        CHECK_NOTHROW(parse_answer_candidate(std::string(line)));
    }
}

TEST_CASE("request timeout distinguishes default from invalid present values", "[proto][timeout]") {
    SECTION("null uses the admission default") {
        std::string error;
        const auto parsed = parse(request_document_with_timeout(nullptr).dump(), error);
        REQUIRE(parsed.has_value());
        CHECK_FALSE(std::get<Request>(*parsed).context.timeout_seconds.has_value());
    }

    SECTION("positive fractions remain valid") {
        std::string error;
        const auto parsed = parse(request_document_with_timeout(0.000001).dump(), error);
        REQUIRE(parsed.has_value());
        CHECK(std::get<Request>(*parsed).context.timeout_seconds == 0.000001);
    }

    SECTION("zero, negative, non-finite, and representation-extreme values fail") {
        for (
            const auto& line : std::to_array<std::string>({
                request_document_with_timeout(0).dump(),
                request_document_with_timeout(-1).dump(),
                request_document_with_timeout(18446744073709551615ULL).dump(),
                std::string(
                    R"({"type":"request","id":42,"account":"main","command":["version"],"args":{},"context":{"tty":true,"json":false,"yes":false,"dry_run":false,"timeout":1.7976931348623157e308,"cwd":"/","media_dir":null,"write_authority":"unset"}})"),
            })) {
            std::string error;
            INFO(line);
            CHECK_FALSE(parse(line, error).has_value());
            CHECK(error.find("timeout") != std::string::npos);
        }
    }
}

TEST_CASE("write_authority outside the tri-state is rejected", "[proto]") {
    auto doc = json::parse(serialize(make_request()));
    doc["context"]["write_authority"] = "root";
    std::string error;
    CHECK_FALSE(parse(doc.dump(), error).has_value());
    CHECK(error.find("write_authority") != std::string::npos);
}

TEST_CASE("secret frame buffers are zeroed across serialization transport and parsing",
          "[proto][secret][wipe]") {
    struct Observation {
        std::string stage;
        std::size_t size = 0;
        bool all_zero = false;
    };
    std::vector<Observation> observations;
    const auto observer = [&observations](std::string_view stage, const char* bytes,
                                          std::size_t size) {
        const bool all_zero =
            size == 0 || std::all_of(bytes, bytes + static_cast<std::ptrdiff_t>(size),
                                     [](char value) { return value == '\0'; });
        observations.push_back({std::string(stage), size, all_zero});
    };
    constexpr std::string_view sentinel = "1:token";
    const json payload{{"nonce", "00112233445566778899aabbccddeeff"},
                       {"sequence", 1},
                       {"client_generation", 4},
                       {"auth_sequence", 9},
                       {"value", sentinel}};
    std::array<int, 2> descriptors{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    std::string error;
    {
        const Frame outbound{Answer{7, json(payload), observer}};
        REQUIRE(write_frame(descriptors[0], outbound, error, observer));
    }
    {
        FrameReader reader(descriptors[1], observer);
        auto line = reader.read_line(error);
        REQUIRE(line);
        auto parsed = parse(std::move(*line), error, observer);
        REQUIRE(parsed);
        const auto& answer = std::get<Answer>(*parsed);
        CHECK(answer.answer["value"] == sentinel);
    }
    ::close(descriptors[0]);
    ::close(descriptors[1]);

    for (const auto* const stage :
         {"serialized_json", "write_line", "answer_source", "answer_move_source", "answer_payload",
          "frame_reader_chunk", "frame_reader_buffer", "parsed_line", "parsed_json"}) {
        const auto matching = [&stage](const Observation& item) { return item.stage == stage; };
        INFO(stage);
        REQUIRE(std::ranges::any_of(observations, matching));
        CHECK(std::ranges::all_of(observations, [&](const Observation& item) {
            return !matching(item) || item.all_zero;
        }));
    }
    CHECK(std::ranges::count_if(observations, [sentinel](const Observation& item) {
              return item.stage == "answer_payload" && item.size == sentinel.size() &&
                     item.all_zero;
          }) == 2);

    observations.clear();
    auto malformed_document = json{{"type", "answer"}, {"id", 7}, {"answer", payload}};
    malformed_document["answer"]["unexpected"] = true;
    const auto malformed = malformed_document.dump();
    CHECK_FALSE(parse(malformed, error, observer));
    CHECK(error.find(sentinel) == std::string::npos);
    CHECK(std::ranges::any_of(observations, [sentinel](const Observation& item) {
        return item.stage == "parsed_json" && item.size == sentinel.size() && item.all_zero;
    }));

    observations.clear();
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    ::close(descriptors[1]);
    {
        const Frame outbound{Answer{7, json(payload), observer}};
        CHECK_FALSE(write_frame(descriptors[0], outbound, error, observer));
    }
    ::close(descriptors[0]);
    CHECK(error.find(sentinel) == std::string::npos);
    CHECK(std::ranges::any_of(observations, [](const Observation& item) {
        return item.stage == "write_line" && item.all_zero;
    }));
    CHECK(std::ranges::any_of(observations, [sentinel](const Observation& item) {
        return item.stage == "answer_payload" && item.size == sentinel.size() && item.all_zero;
    }));

    observations.clear();
    {
        const Frame cancelled{Answer{7,
                                     {{"nonce", "00112233445566778899aabbccddeeff"},
                                      {"sequence", 1},
                                      {"client_generation", 4},
                                      {"auth_sequence", 9},
                                      {"cancelled", true}},
                                     observer}};
    }
    CHECK(std::ranges::any_of(observations, [](const Observation& item) {
        return item.stage == "answer_payload" && item.all_zero;
    }));
}
