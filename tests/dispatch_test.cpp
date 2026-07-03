// Contract-style tests: requests driven through the real Dispatcher against
// the M0 command set; assertions on observable frames and exit codes only.

#include "common/exit_codes.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"

#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
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
    proto::Request request;
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
        {daemon::Tier::Write, [&handler_ran](const proto::Request&, daemon::ResponseSink& sink) {
             handler_ran = true;
             sink.result(json::object());
         }});
    dispatcher.register_command("nuke",
                                {daemon::Tier::Destructive,
                                 [&handler_ran](const proto::Request&, daemon::ResponseSink& sink) {
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
