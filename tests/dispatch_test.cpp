// Contract-style tests: requests driven through the real Dispatcher against
// the M0 command set; assertions on observable frames and exit codes only.

#include "common/exit_codes.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

struct Outcome {
    std::optional<json> result;
    std::optional<std::string> error_code;
    std::optional<json> error_details;
    int exit_code = -1;
};

Outcome dispatch_request(const daemon::Dispatcher& dispatcher, const proto::Request& request) {
    Outcome outcome;
    daemon::CallbackSink sink(
        [](const json&) {}, [](const json&) {},
        [&outcome](json data) {
            outcome.result = std::move(data);
            outcome.exit_code = kOk;
        },
        [&outcome](std::string code, const std::string&, json details, int exit_code) {
            outcome.error_code = std::move(code);
            outcome.error_details = std::move(details);
            outcome.exit_code = exit_code;
        });
    dispatcher.dispatch(request, sink);
    return outcome;
}

Outcome dispatch_frozen_request(const daemon::Dispatcher& dispatcher,
                                const proto::Request& request) {
    Outcome outcome;
    daemon::CallbackSink sink(
        [](const json&) {}, [](const json&) {},
        [&outcome](json data) {
            outcome.result = std::move(data);
            outcome.exit_code = kOk;
        },
        [&outcome](std::string code, const std::string&, json details, int exit_code) {
            outcome.error_code = std::move(code);
            outcome.error_details = std::move(details);
            outcome.exit_code = exit_code;
        });
    daemon::RequestSession session(request, sink, 0, daemon::RequestSession::NonceGenerator{},
                                   daemon::ActivityTracker::Token{}, nullptr, std::nullopt,
                                   daemon::ConfigAdmissionMode::FrozenRuntime);
    dispatcher.dispatch(session);
    return outcome;
}

Outcome dispatch(const daemon::Dispatcher& dispatcher, const std::vector<std::string>& command) {
    proto::Request request("testacct");
    request.id = 1;
    request.command = command;
    return dispatch_request(dispatcher, request);
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

std::vector<std::string> command_parts(std::string_view path) {
    std::vector<std::string> parts;
    while (!path.empty()) {
        const auto separator = path.find(' ');
        parts.emplace_back(path.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        path.remove_prefix(separator + 1);
    }
    return parts;
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

TEST_CASE("M1 destructive bypass is bound to its two exact command paths",
          "[dispatch][m1][safety]") {
    constexpr std::array<std::string_view, 2> allowed_paths{"logout", "account remove"};
    daemon::Dispatcher dispatcher;
    std::set<std::string_view> handled_paths;
    for (const auto path : allowed_paths) {
        dispatcher.register_command(
            std::string(path),
            {daemon::Tier::Destructive,
             [path, &handled_paths](const proto::Request&, daemon::RequestSession& sink) {
                 handled_paths.emplace(path);
                 sink.result({{"ok", true}});
             },
             true});
    }
    for (const auto path : allowed_paths) {
        const auto outcome = dispatch(dispatcher, command_parts(path));
        REQUIRE(outcome.result.has_value());
        CHECK(outcome.exit_code == kOk);
        CHECK(handled_paths.contains(path));
    }
    CHECK(handled_paths.size() == allowed_paths.size());

    const auto handler = [](const proto::Request&, daemon::RequestSession& sink) {
        sink.result({{"unsafe", true}});
    };
    daemon::Dispatcher rejected;
    CHECK_THROWS_AS(rejected.register_command("nuke", {daemon::Tier::Destructive, handler, true}),
                    std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command("loguot", {daemon::Tier::Destructive, handler, true}),
                    std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command("logout", {daemon::Tier::Write, handler, true}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        rejected.register_command("account remove", {daemon::Tier::Read, handler, true}),
        std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command(
                        "send", {daemon::Tier::Write, handler, true, daemon::M3Operation::Send}),
                    std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command("chat leave", {daemon::Tier::Destructive, handler,
                                                             true, daemon::M3Operation::ChatLeave}),
                    std::invalid_argument);

    daemon::Dispatcher unmarked;
    bool unmarked_ran = false;
    unmarked.register_command(
        "logout", {daemon::Tier::Destructive,
                   [&unmarked_ran](const proto::Request&, daemon::RequestSession& sink) {
                       unmarked_ran = true;
                       sink.result({{"unsafe", true}});
                   }});
    const auto denied = dispatch(unmarked, {"logout"});
    CHECK(denied.error_code == "DENIED");
    CHECK(denied.exit_code == kDenied);
    CHECK_FALSE(unmarked_ran);
}

TEST_CASE("M3 operation registry is closed and has exact tier and bot policy",
          "[dispatch][m3][safety]") {
    struct ExpectedPolicy {
        daemon::M3Operation operation;
        std::string_view canonical_name;
        std::string_view command_path;
        daemon::Tier tier;
        daemon::M3BotPolicy bot_policy;
    };
    constexpr std::array expected{
        ExpectedPolicy{daemon::M3Operation::Send, "send", "send", daemon::Tier::Write,
                       daemon::M3BotPolicy::ImmediateOnly},
        ExpectedPolicy{daemon::M3Operation::MsgEdit, "msg_edit", "msg edit", daemon::Tier::Write,
                       daemon::M3BotPolicy::Allowed},
        ExpectedPolicy{daemon::M3Operation::MsgDelete, "msg_delete", "msg delete",
                       daemon::Tier::Destructive, daemon::M3BotPolicy::Allowed},
        ExpectedPolicy{daemon::M3Operation::MsgForward, "msg_forward", "msg forward",
                       daemon::Tier::Write, daemon::M3BotPolicy::Allowed},
        ExpectedPolicy{daemon::M3Operation::MsgReact, "msg_react", "msg react", daemon::Tier::Write,
                       daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::MsgPin, "msg_pin", "msg pin", daemon::Tier::Write,
                       daemon::M3BotPolicy::Allowed},
        ExpectedPolicy{daemon::M3Operation::MsgUnpin, "msg_unpin", "msg unpin", daemon::Tier::Write,
                       daemon::M3BotPolicy::Allowed},
        ExpectedPolicy{daemon::M3Operation::ChatMarkRead, "chat_mark_read", "chat mark-read",
                       daemon::Tier::Write, daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatMute, "chat_mute", "chat mute", daemon::Tier::Write,
                       daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatUnmute, "chat_unmute", "chat unmute",
                       daemon::Tier::Write, daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatPin, "chat_pin", "chat pin", daemon::Tier::Write,
                       daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatUnpin, "chat_unpin", "chat unpin",
                       daemon::Tier::Write, daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatArchive, "chat_archive", "chat archive",
                       daemon::Tier::Write, daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatUnarchive, "chat_unarchive", "chat unarchive",
                       daemon::Tier::Write, daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatJoin, "chat_join", "chat join", daemon::Tier::Write,
                       daemon::M3BotPolicy::UserOnly},
        ExpectedPolicy{daemon::M3Operation::ChatLeave, "chat_leave", "chat leave",
                       daemon::Tier::Destructive, daemon::M3BotPolicy::Allowed},
        ExpectedPolicy{daemon::M3Operation::SavedAttach, "saved_attach", "saved attach",
                       daemon::Tier::Write, daemon::M3BotPolicy::UserOnly},
    };

    const auto policies = daemon::m3_operation_policies();
    const auto identities = proto::m3_operation_identities();
    REQUIRE(policies.size() == expected.size());
    REQUIRE(identities.size() == expected.size());
    std::set<std::string_view> canonical_names;
    std::set<std::string_view> command_paths;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& wanted = expected.at(index);
        const auto& actual = policies[index];
        CHECK(actual.operation == wanted.operation);
        CHECK(actual.canonical_name == wanted.canonical_name);
        CHECK(actual.command_path == wanted.command_path);
        CHECK(actual.tier == wanted.tier);
        CHECK(actual.bot_policy == wanted.bot_policy);
        CHECK(identities[index].operation == actual.operation);
        CHECK(identities[index].canonical_name == actual.canonical_name);
        CHECK(identities[index].command_path == actual.command_path);
        CHECK(canonical_names.emplace(actual.canonical_name).second);
        CHECK(command_paths.emplace(actual.command_path).second);

        REQUIRE(daemon::parse_m3_operation(wanted.canonical_name).has_value());
        CHECK(*daemon::parse_m3_operation(wanted.canonical_name) == wanted.operation);
        REQUIRE(daemon::m3_operation_for_command(wanted.command_path).has_value());
        CHECK(*daemon::m3_operation_for_command(wanted.command_path) == wanted.operation);
        REQUIRE(daemon::m3_operation_policy(wanted.operation) != nullptr);
        CHECK(daemon::m3_operation_policy(wanted.operation)->canonical_name ==
              wanted.canonical_name);

        CHECK(daemon::evaluate_m3_bot_admission(wanted.operation, false,
                                                daemon::M3ScheduleKind::None) ==
              daemon::M3BotAdmission::Allowed);
        const auto expected_bot = wanted.bot_policy == daemon::M3BotPolicy::UserOnly
                                      ? daemon::M3BotAdmission::Unsupported
                                      : daemon::M3BotAdmission::Allowed;
        CHECK(daemon::evaluate_m3_bot_admission(wanted.operation, true,
                                                daemon::M3ScheduleKind::None) == expected_bot);
    }

    CHECK(daemon::evaluate_m3_bot_admission(daemon::M3Operation::Send, false,
                                            daemon::M3ScheduleKind::At) ==
          daemon::M3BotAdmission::Allowed);
    CHECK(daemon::evaluate_m3_bot_admission(daemon::M3Operation::Send, false,
                                            daemon::M3ScheduleKind::Online) ==
          daemon::M3BotAdmission::Allowed);
    CHECK(daemon::evaluate_m3_bot_admission(daemon::M3Operation::Send, true,
                                            daemon::M3ScheduleKind::At) ==
          daemon::M3BotAdmission::Unsupported);
    CHECK(daemon::evaluate_m3_bot_admission(daemon::M3Operation::Send, true,
                                            daemon::M3ScheduleKind::Online) ==
          daemon::M3BotAdmission::Unsupported);

    const auto invalid = static_cast<daemon::M3Operation>(255);
    CHECK(daemon::m3_operation_policy(invalid) == nullptr);
    CHECK_FALSE(daemon::parse_m3_operation("msg-edit").has_value());
    CHECK_FALSE(daemon::m3_operation_for_command("msg_edit").has_value());
    CHECK(daemon::evaluate_m3_bot_admission(invalid, false, daemon::M3ScheduleKind::None) ==
          daemon::M3BotAdmission::Unsupported);

    const auto invalid_schedule = static_cast<daemon::M3ScheduleKind>(255);
    for (const auto& policy : policies) {
        CHECK(daemon::evaluate_m3_bot_admission(policy.operation, false, invalid_schedule) ==
              daemon::M3BotAdmission::Unsupported);
        if (policy.bot_policy != daemon::M3BotPolicy::UserOnly) {
            CHECK(daemon::evaluate_m3_bot_admission(policy.operation, true, invalid_schedule) ==
                  daemon::M3BotAdmission::Unsupported);
        }
    }
}

TEST_CASE("dormant M6 session functions do not extend or activate the M3 command registry",
          "[dispatch][m3][m6][session][safety]") {
    CHECK(daemon::m3_operation_policies().size() == 17);
    CHECK_FALSE(daemon::m3_operation_for_command("session list").has_value());
    CHECK_FALSE(daemon::m3_operation_for_command("session terminate").has_value());
    CHECK_FALSE(daemon::parse_m3_operation("session_list").has_value());
    CHECK_FALSE(daemon::parse_m3_operation("session_terminate").has_value());

    auto context = test_context();
    daemon::Dispatcher dispatcher;
    daemon::register_commands(dispatcher, context);
    for (const auto& command : {std::vector<std::string>{"session", "list"},
                                std::vector<std::string>{"session", "terminate"}}) {
        const auto outcome = dispatch(dispatcher, command);
        CHECK(outcome.error_code == "USAGE");
        CHECK(outcome.exit_code == kUsage);
        CHECK_FALSE(outcome.result.has_value());
    }
}

TEST_CASE("dispatcher activates unlimited defaults only for registered eligible commands",
          "[dispatch][deadline][unlimited]") {
    const auto report_deadline = [](const proto::Request&, daemon::RequestSession& session) {
        session.result({{"finite", session.deadline().expires_at.has_value()}});
    };

    daemon::Dispatcher dispatcher;
    dispatcher.register_command("ordinary", {daemon::Tier::Read, report_deadline});
    dispatcher.register_command("fetch", {daemon::Tier::Read, report_deadline, false, std::nullopt,
                                          DeadlineDefault::Unlimited});

    proto::Request ordinary("testacct");
    ordinary.command = {"ordinary"};
    auto outcome = dispatch_request(dispatcher, ordinary);
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["finite"] == true);

    proto::Request fetch("testacct");
    fetch.command = {"fetch"};
    outcome = dispatch_request(dispatcher, fetch);
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["finite"] == false);

    fetch.context.timeout_seconds = 0.25;
    outcome = dispatch_request(dispatcher, fetch);
    REQUIRE(outcome.result);
    CHECK((*outcome.result)["finite"] == true);

    proto::Request unregistered("testacct");
    unregistered.command = {"listen"};
    CHECK(dispatcher.deadline_default(unregistered) == DeadlineDefault::Default60);
    CHECK_THROWS_AS(dispatcher.register_command("fetch", {daemon::Tier::Read, report_deadline}),
                    std::invalid_argument);
    CHECK_THROWS_AS(dispatcher.register_command("ordinary unlimited",
                                                {daemon::Tier::Read, report_deadline, false,
                                                 std::nullopt, DeadlineDefault::Unlimited}),
                    std::invalid_argument);
}

TEST_CASE("dormant M3 descriptors require frozen config admission in direct paths",
          "[dispatch][m3][config-admission][no-daemon]") {
    daemon::Dispatcher dispatcher;
    dispatcher.register_command("send", {daemon::Tier::Write,
                                         [](const proto::Request&, daemon::RequestSession&) {},
                                         false, daemon::M3Operation::Send});
    proto::Request send("testacct");
    send.command = {"send"};
    CHECK(dispatcher.requires_frozen_config_admission(send));
    CHECK_THROWS_AS(dispatch_request(dispatcher, send), std::invalid_argument);

    dispatcher.register_command(
        "ordinary", {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession&) {}});
    proto::Request ordinary("testacct");
    ordinary.command = {"ordinary"};
    CHECK_FALSE(dispatcher.requires_frozen_config_admission(ordinary));
}

TEST_CASE("fetch retains removal before logout recovery without broadening read history",
          "[dispatch][deadline][preflight][ordering]") {
    const auto fetch = daemon::recovery_preflight_order("fetch");
    REQUIRE(fetch.size() == 2);
    CHECK(fetch[0] == daemon::RecoveryPreflight::Removal);
    CHECK(fetch[1] == daemon::RecoveryPreflight::Logout);

    CHECK(daemon::recovery_preflight_order("read").empty());
    CHECK(daemon::recovery_preflight_order("history").empty());
    const auto daemon_status = daemon::recovery_preflight_order("daemon status");
    REQUIRE(daemon_status.size() == 1);
    CHECK(daemon_status.front() == daemon::RecoveryPreflight::Removal);

    for (const auto* command :
         {"send", "msg edit", "msg delete", "msg forward", "msg react", "msg pin", "msg unpin"}) {
        const auto write = daemon::recovery_preflight_order(command);
        REQUIRE(write.size() == 2);
        CHECK(write[0] == daemon::RecoveryPreflight::Removal);
        CHECK(write[1] == daemon::RecoveryPreflight::Logout);
    }
}

TEST_CASE("dispatcher independently rejects invalid or out-of-scope raw idempotency keys",
          "[dispatch][m3][idempotency][safety]") {
    daemon::Dispatcher dispatcher;
    dispatcher.register_command(
        "version", {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& session) {
                        session.result({{"ok", true}});
                    }});

    proto::Request invalid("testacct");
    invalid.command = {"version"};
    invalid.context.idempotency_key = "raw/key";
    auto outcome = dispatch_request(dispatcher, invalid);
    CHECK(outcome.error_code == "USAGE");
    CHECK(outcome.error_details ==
          json{{"argument", "--idempotency-key"}, {"reason", "invalid_argument"}});

    invalid.context.idempotency_key = "valid-key";
    outcome = dispatch_request(dispatcher, invalid);
    CHECK(outcome.error_code == "USAGE");
    CHECK(outcome.error_details ==
          json{{"argument", "--idempotency-key"}, {"reason", "unsupported_mode"}});

    daemon::Dispatcher m3;
    bool handler_ran = false;
    m3.register_command("send",
                        {daemon::Tier::Write,
                         [&handler_ran](const proto::Request&, daemon::RequestSession& session) {
                             handler_ran = true;
                             session.result(json::object());
                         },
                         false, daemon::M3Operation::Send});
    proto::Request dry_run("testacct");
    dry_run.command = {"send"};
    dry_run.context.idempotency_key = "valid-key";
    dry_run.context.dry_run = true;
    outcome = dispatch_frozen_request(m3, dry_run);
    CHECK(outcome.error_code == "USAGE");
    CHECK(outcome.error_details ==
          json{{"argument", "--idempotency-key"}, {"reason", "mutually_exclusive"}});

    dry_run.context.dry_run = false;
    outcome = dispatch_frozen_request(m3, dry_run);
    CHECK(outcome.result == json::object());
    CHECK(handler_ran);
}

TEST_CASE("M3 descriptors must match the closed registry and remain fail closed",
          "[dispatch][m3][safety]") {
    const auto handler = [](const proto::Request&, daemon::RequestSession& sink) {
        sink.result(json::object());
    };

    daemon::Dispatcher rejected;
    CHECK_THROWS_AS(
        rejected.register_command("send", {daemon::Tier::Write, handler, false, std::nullopt}),
        std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command("wrong path", {daemon::Tier::Write, handler, false,
                                                             daemon::M3Operation::Send}),
                    std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command(
                        "send", {daemon::Tier::Read, handler, false, daemon::M3Operation::Send}),
                    std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command(
                        "send", {daemon::Tier::Write, handler, true, daemon::M3Operation::Send}),
                    std::invalid_argument);
    CHECK_THROWS_AS(rejected.register_command("send", {daemon::Tier::Write, handler, false,
                                                       static_cast<daemon::M3Operation>(255)}),
                    std::invalid_argument);

    daemon::Dispatcher dispatcher;
    std::size_t handler_runs = 0;
    for (const auto& policy : daemon::m3_operation_policies()) {
        dispatcher.register_command(
            std::string(policy.command_path),
            {policy.tier,
             [&handler_runs](const proto::Request&, daemon::RequestSession& sink) {
                 ++handler_runs;
                 sink.result(json::object());
             },
             false, policy.operation});
    }
    for (const auto& policy : daemon::m3_operation_policies()) {
        proto::Request request("testacct");
        request.id = 1;
        request.command = command_parts(policy.command_path);
        const auto outcome = dispatch_frozen_request(dispatcher, request);
        CHECK(outcome.result == json::object());
    }
    CHECK(handler_runs == 17);
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
