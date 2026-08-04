// Contract-style tests: requests driven through the real Dispatcher against
// the M0 command set; assertions on observable frames and exit codes only.

#include "common/exit_codes.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"

#include <atomic>
#include <barrier>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

struct Outcome {
    std::optional<json> result;
    std::optional<std::string> error_code;
    int exit_code = -1;
};

Outcome dispatch(const daemon::Dispatcher& dispatcher, const std::vector<std::string>& command) {
    Outcome outcome;
    daemon::CallbackSink sink(
        [](const json&) {}, [](const json&) {},
        [&outcome](json data) {
            outcome.result = std::move(data);
            outcome.exit_code = kOk;
        },
        [&outcome](std::string code, const std::string&, const json&, int exit_code) {
            outcome.error_code = std::move(code);
            outcome.exit_code = exit_code;
        });
    proto::Request request("testacct");
    request.id = 1;
    request.command = command;
    dispatcher.dispatch(request, sink);
    return outcome;
}

daemon::DaemonContext test_context() {
    daemon::DaemonContext context;
    context.account = "testacct";
    context.binary_version = "9.9.9";
    context.protocol_version = 7;
    context.tdlib_version = "1.2.3";
    context.socket_path = "/tmp/test.sock";
    return context;
}

} // namespace

TEST_CASE("version reports binary, protocol, and tdlib versions", "[dispatch]") {
    const auto context = test_context();
    daemon::Dispatcher dispatcher;
    daemon::register_commands(dispatcher, context);

    const auto outcome = dispatch(dispatcher, {"version"});
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.exit_code == kOk);
    CHECK((*outcome.result)["version"] == "9.9.9");
    CHECK((*outcome.result)["protocol"] == 7);
    CHECK((*outcome.result)["tdlib"] == "1.2.3");
    CHECK_THAT(*outcome.result, test::matches_json_schema("version.result.schema.json"));
}

TEST_CASE("doctor reports account, daemon, tdlib, and auth state", "[dispatch]") {
    const auto context = test_context();
    daemon::Dispatcher dispatcher;
    daemon::register_commands(dispatcher, context);

    const auto outcome = dispatch(dispatcher, {"doctor"});
    REQUIRE(outcome.result.has_value());
    const auto& data = *outcome.result;
    CHECK(data["account"] == "testacct");
    CHECK(data["daemon"]["running"] == true);
    CHECK(data["daemon"]["socket"] == "/tmp/test.sock");
    CHECK(data["tdlib"]["version"] == "1.2.3");
    CHECK(data["auth"]["state"] == "unknown");
    CHECK_THAT(data, test::matches_json_schema("doctor.result.schema.json"));
}

TEST_CASE("doctor reports the in-process daemon bypass variant", "[dispatch][schema]") {
    auto context = test_context();
    context.socket_path.clear();
    context.in_process = true;
    daemon::Dispatcher dispatcher;
    daemon::register_commands(dispatcher, context);

    const auto outcome = dispatch(dispatcher, {"doctor"});
    REQUIRE(outcome.result.has_value());
    CHECK_THAT(*outcome.result, test::matches_json_schema("doctor.result.schema.json"));
}

TEST_CASE("unknown command exits 2 with a USAGE error", "[dispatch]") {
    const auto context = test_context();
    daemon::Dispatcher dispatcher;
    daemon::register_commands(dispatcher, context);

    const auto outcome = dispatch(dispatcher, {"frobnicate"});
    CHECK_FALSE(outcome.result.has_value());
    CHECK(outcome.error_code == "USAGE");
    CHECK(outcome.exit_code == kUsage);
}

TEST_CASE("non-Read tiers fail closed pending the M3 gate", "[dispatch]") {
    daemon::Dispatcher dispatcher;
    bool handler_ran = false;
    dispatcher.register_command(
        "poke",
        {daemon::Tier::Write, [&handler_ran](const proto::Request&, daemon::RequestSession& sink) {
             handler_ran = true;
             sink.result(json::object());
         }});
    dispatcher.register_command(
        "nuke", {daemon::Tier::Destructive,
                 [&handler_ran](const proto::Request&, daemon::RequestSession& sink) {
                     handler_ran = true;
                     sink.result(json::object());
                 }});

    for (const auto* name : {"poke", "nuke"}) {
        const auto outcome = dispatch(dispatcher, {name});
        CHECK(outcome.error_code == "DENIED");
        CHECK(outcome.exit_code == kDenied);
    }
    CHECK_FALSE(handler_ran);
}

TEST_CASE("daemon stop triggers the shutdown hook and confirms", "[dispatch]") {
    auto context = test_context();
    bool shutdown_requested = false;
    context.request_shutdown = [&shutdown_requested] { shutdown_requested = true; };
    daemon::Dispatcher dispatcher;
    daemon::register_commands(dispatcher, context);

    const auto outcome = dispatch(dispatcher, {"daemon", "stop"});
    REQUIRE(outcome.result.has_value());
    CHECK((*outcome.result)["stopping"] == true);
    CHECK_THAT(*outcome.result, test::matches_json_schema("daemon-stop.result.schema.json"));
    CHECK(shutdown_requested);
}

TEST_CASE("daemon stop under --no-daemon is a usage error", "[dispatch]") {
    auto context = test_context();
    context.in_process = true;
    daemon::Dispatcher dispatcher;
    daemon::register_commands(dispatcher, context);

    const auto outcome = dispatch(dispatcher, {"daemon", "stop"});
    CHECK(outcome.error_code == "USAGE");
    CHECK(outcome.exit_code == kUsage);
}

TEST_CASE("only the first terminal response is emitted", "[dispatch]") {
    daemon::Dispatcher dispatcher;
    dispatcher.register_command(
        "duplicate terminal",
        {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& sink) {
             sink.item({{"sequence", 1}});
             sink.result({{"winner", "result"}});
             sink.error("GENERIC", "late error", json::object(), kGeneric);
             sink.result({{"winner", "late result"}});
             sink.progress({{"sequence", 2}});
         }});

    int item_count = 0;
    int progress_count = 0;
    int result_count = 0;
    int error_count = 0;
    daemon::CallbackSink sink([&item_count](const json&) { ++item_count; },
                              [&progress_count](const json&) { ++progress_count; },
                              [&result_count](const json&) { ++result_count; },
                              [&error_count](const std::string&, const std::string&, const json&,
                                             int) { ++error_count; });
    proto::Request request("testacct");
    request.id = 1;
    request.command = {"duplicate", "terminal"};

    dispatcher.dispatch(request, sink);

    CHECK(item_count == 1);
    CHECK(progress_count == 0);
    CHECK(result_count == 1);
    CHECK(error_count == 0);
}

TEST_CASE("handler return and exception both receive a terminal error", "[dispatch]") {
    daemon::Dispatcher dispatcher;
    dispatcher.register_command(
        "missing terminal",
        {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession&) {}});
    dispatcher.register_command(
        "throwing handler",
        {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession&) {
             throw std::runtime_error("handler failed");
         }});

    for (const auto& command : {std::vector<std::string>{"missing", "terminal"},
                                std::vector<std::string>{"throwing", "handler"}}) {
        const auto outcome = dispatch(dispatcher, command);
        CHECK_FALSE(outcome.result.has_value());
        CHECK(outcome.error_code == "GENERIC");
        CHECK(outcome.exit_code == kGeneric);
    }
}

TEST_CASE("concurrent result and error race to one terminal response", "[dispatch]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        std::atomic<int> result_count{0};
        std::atomic<int> error_count{0};
        daemon::CallbackSink sink(
            [](const json&) {}, [](const json&) {},
            [&result_count](const json&) { result_count.fetch_add(1, std::memory_order_relaxed); },
            [&error_count](const std::string&, const std::string&, const json&, int) {
                error_count.fetch_add(1, std::memory_order_relaxed);
            });
        std::barrier start(3);
        std::thread result_thread([&sink, &start] {
            start.arrive_and_wait();
            sink.result({{"winner", "result"}});
        });
        std::thread error_thread([&sink, &start] {
            start.arrive_and_wait();
            sink.error("DAEMON_SHUTDOWN", "daemon is shutting down",
                       {{"reason", "daemon_shutdown"}}, kGeneric);
        });

        start.arrive_and_wait();
        result_thread.join();
        error_thread.join();

        CHECK(result_count.load(std::memory_order_relaxed) +
                  error_count.load(std::memory_order_relaxed) ==
              1);
        CHECK(sink.has_terminal());
    }
}
