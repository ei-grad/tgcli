#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/logout_commands.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using namespace tgcli;
using nlohmann::json;

namespace {

class LogoutTree final {
  public:
    explicit LogoutTree(bool allow_write = true, bool credentials = false) {
        std::string pattern = "/tmp/tgcli-logout-command-XXXXXX";
        pattern.push_back('\0');
        const char* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        for (const auto& directory :
             {config_root(), state_root(), data_root(), runtime_root(), config_root() + "/tgcli",
              state_root() + "/tgcli", state_root() + "/tgcli/accounts", account_state()}) {
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        }
        std::ofstream output(config_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << "default_account = \"main\"\n\n[accounts.main]\nallow_write = "
               << (allow_write ? "true\n" : "false\n");
        if (credentials) {
            output << "api_id = 12345\napi_hash = \"0123456789abcdef0123456789abcdef\"\n";
        }
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

    ~LogoutTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    LogoutTree(const LogoutTree&) = delete;
    LogoutTree& operator=(const LogoutTree&) = delete;
    LogoutTree(LogoutTree&&) = delete;
    LogoutTree& operator=(LogoutTree&&) = delete;

    [[nodiscard]] std::string config_root() const {
        return root_ + "/config";
    }
    [[nodiscard]] std::string state_root() const {
        return root_ + "/state";
    }
    [[nodiscard]] std::string data_root() const {
        return root_ + "/data";
    }
    [[nodiscard]] std::string runtime_root() const {
        return root_ + "/runtime";
    }
    [[nodiscard]] std::string config_path() const {
        return config_root() + "/tgcli/config.toml";
    }
    [[nodiscard]] std::string account_state() const {
        return state_root() + "/tgcli/accounts/main";
    }
    [[nodiscard]] std::string audit_path() const {
        return account_state() + "/audit.log";
    }
    [[nodiscard]] paths::Environment environment() const {
        paths::Environment result;
        result.xdg_config_home = config_root();
        result.xdg_state_home = state_root();
        result.xdg_data_home = data_root();
        result.xdg_runtime_dir = runtime_root();
        result.home = root_;
        result.uid = ::getuid();
        return result;
    }

    void write_config(std::string_view contents) const {
        std::ofstream output(config_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << contents;
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

  private:
    std::string root_;
};

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    std::vector<json> challenges;
    int exit_code = -1;
    int terminal_count = 0;
};

struct ControlledLogout {
    std::shared_ptr<daemon::RequestSession> session;
    std::future<Outcome> outcome;
};

std::shared_ptr<daemon::testing::LogoutHooks>
fixed_hooks(std::shared_ptr<const daemon::testing::LogoutAuditHooks> audit = {}) {
    auto hooks = std::make_shared<daemon::testing::LogoutHooks>();
    hooks->invocation_id = [] { return "0123456789abcdef0123456789abcdef"; };
    hooks->timestamp = [] { return "2026-08-04T12:00:00Z"; };
    hooks->audit = std::move(audit);
    return hooks;
}

class FakeLogout final {
  public:
    FakeLogout(const LogoutTree& tree,
               std::shared_ptr<const daemon::testing::LogoutHooks> hooks = {},
               std::function<void()> audit_fatal = {},
               core::AuthState initial_state = core::AuthState::Ready)
        : config_(tree.config_path(), {}, tree.environment().uid) {
        auto runtime = std::make_unique<test::ScriptedTdRuntime>(true);
        runtime_ = runtime.get();
        client_ = std::make_unique<core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        first_ = runtime_->clients().front();
        runtime_->push_response(first_, 1, {}, core::AuthStateData{initial_state});
        REQUIRE(wait_state(initial_state));
        coordinator_ = std::make_unique<daemon::LogoutCoordinator>(
            *client_, config_, tree.environment(), "main", tree.config_path(),
            std::move(audit_fatal), hooks ? std::move(hooks) : fixed_hooks());
        daemon::register_logout_command(dispatcher_, *coordinator_);
        dispatcher_.set_request_preflight(
            [this](const std::string& command, daemon::RequestSession& session) {
                if (command == "logout" && session.request().context.dry_run) {
                    return true;
                }
                return coordinator_->preflight(session);
            });
        dispatcher_.register_command(
            "doctor",
            {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& session) {
                 session.result({{"checked", true}});
             }});
    }

    ~FakeLogout() {
        const auto current = client_->auth_state();
        if (current && current->auth_sequence == 0) {
            const auto clients = runtime_->clients();
            if (!clients.empty()) {
                runtime_->push_response(clients.back(), 1, {},
                                        core::AuthStateData{core::AuthState::WaitPhoneNumber});
                static_cast<void>(wait_state(core::AuthState::WaitPhoneNumber));
            }
        }
        client_->close();
    }

    FakeLogout(const FakeLogout&) = delete;
    FakeLogout& operator=(const FakeLogout&) = delete;
    FakeLogout(FakeLogout&&) = delete;
    FakeLogout& operator=(FakeLogout&&) = delete;

    [[nodiscard]] bool wait_state(core::AuthState state) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = client_->auth_state();
            if (snapshot && snapshot->data.state == state) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    [[nodiscard]] bool wait_occurrence(std::uint64_t generation,
                                       std::uint64_t minimum_sequence) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = client_->auth_state();
            if (snapshot && snapshot->client_generation >= generation &&
                snapshot->auth_sequence >= minimum_sequence) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    std::future<Outcome> dispatch(proto::WriteAuthority authority = proto::WriteAuthority::Unset,
                                  bool yes = true, bool tty = false,
                                  std::optional<double> timeout = 2.0, bool dry_run = false,
                                  std::optional<bool> confirmation = std::nullopt,
                                  std::vector<std::string> command = {"logout"},
                                  std::function<void(std::size_t)> challenge_hook = {}) {
        return dispatch_controlled(authority, yes, tty, timeout, dry_run, confirmation,
                                   std::move(command), std::move(challenge_hook))
            .outcome;
    }

    ControlledLogout
    dispatch_controlled(proto::WriteAuthority authority = proto::WriteAuthority::Unset,
                        bool yes = true, bool tty = false, std::optional<double> timeout = 2.0,
                        bool dry_run = false, std::optional<bool> confirmation = std::nullopt,
                        std::vector<std::string> command = {"logout"},
                        std::function<void(std::size_t)> challenge_hook = {}) {
        auto outcome = std::make_shared<Outcome>();
        auto sink = std::make_shared<daemon::CallbackSink>(
            [](const json&) {}, [](const json&) {},
            [outcome](json result) {
                outcome->result = std::move(result);
                outcome->exit_code = kOk;
                ++outcome->terminal_count;
            },
            [outcome](std::string code, std::string message, json details, int exit_code) {
                outcome->error = json{{"error",
                                       {{"code", std::move(code)},
                                        {"message", std::move(message)},
                                        {"details", std::move(details)}}}};
                outcome->exit_code = exit_code;
                ++outcome->terminal_count;
            },
            [outcome, confirmation,
             challenge_hook = std::move(challenge_hook)](json challenge) -> std::optional<json> {
                outcome->challenges.push_back(challenge);
                if (challenge_hook) {
                    challenge_hook(outcome->challenges.size());
                }
                if (!confirmation) {
                    return std::nullopt;
                }
                return json{{"nonce", challenge["nonce"]},
                            {"sequence", challenge["sequence"]},
                            {"client_generation", challenge["client_generation"]},
                            {"auth_sequence", challenge["auth_sequence"]},
                            {"value", *confirmation}};
            });
        proto::Request request("main");
        request.id = 41;
        request.command = std::move(command);
        request.context.write_authority = authority;
        request.context.yes = yes;
        request.context.tty = tty;
        request.context.timeout_seconds = timeout;
        request.context.dry_run = dry_run;
        auto session = std::make_shared<daemon::RequestSession>(std::move(request), sink);
        auto future = std::async(std::launch::async, [this, outcome, session] {
            dispatcher_.dispatch(*session);
            return *outcome;
        });
        return {std::move(session), std::move(future)};
    }

    test::ScriptedTdRuntime& runtime() {
        return *runtime_;
    }
    [[nodiscard]] test::ScriptedClient first() const {
        return first_;
    }

  private:
    daemon::ConfigRuntime config_;
    test::ScriptedTdRuntime* runtime_ = nullptr;
    test::ScriptedClient first_{};
    std::unique_ptr<core::TdClient> client_;
    std::unique_ptr<daemon::LogoutCoordinator> coordinator_;
    daemon::Dispatcher dispatcher_;
};

std::vector<json> jsonl_records(const std::string& filename) {
    std::ifstream input(filename);
    std::vector<json> result;
    std::string line;
    while (std::getline(input, line)) {
        result.push_back(json::parse(line));
    }
    return result;
}

std::string file_bytes(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<json> audit_records(const LogoutTree& tree) {
    return jsonl_records(tree.audit_path());
}

void append_prior_logout(const LogoutTree& tree,
                         const std::vector<daemon::AuditStage>& checkpoints) {
    const config::Store store(tree.config_path());
    const auto loaded = store.load();
    REQUIRE(loaded.snapshot);
    std::string error;
    const auto plan = proto::make_logout_plan("main", error);
    REQUIRE(plan);
    const daemon::LogoutAuditLog audit(tree.account_state(), "main", tree.environment().uid);
    const daemon::AuditRecordIdentity identity{"fedcba9876543210fedcba9876543210",
                                               "2026-08-04T11:00:00Z"};
    auto intent = daemon::make_logout_audit_intent(identity, *plan, loaded.snapshot->identity,
                                                   daemon::AuthoritySource::Config,
                                                   daemon::ConfirmationSource::Yes, error);
    REQUIRE(intent);
    daemon::LogoutAuditFailure failure;
    REQUIRE(audit.append(daemon::serialize(*intent), failure, true));
    for (const auto stage : checkpoints) {
        auto checkpoint = daemon::make_logout_audit_checkpoint(identity, *plan, stage, error);
        REQUIRE(checkpoint);
        REQUIRE(audit.append(daemon::serialize(*checkpoint), failure));
    }
}

void append_complete_logout(daemon::LogoutAuditLog& audit, const std::string& config_snapshot,
                            const std::string& invocation) {
    std::string error;
    const auto plan = proto::make_logout_plan("main", error);
    REQUIRE(plan);
    const daemon::AuditRecordIdentity identity{invocation, "2026-08-04T11:00:00Z"};
    daemon::LogoutAuditFailure failure;
    auto intent = daemon::make_logout_audit_intent(identity, *plan, config_snapshot,
                                                   daemon::AuthoritySource::Config,
                                                   daemon::ConfirmationSource::Yes, error);
    REQUIRE(intent);
    REQUIRE(audit.append(daemon::serialize(*intent), failure, true));
    std::vector<daemon::AuditStage> completed{daemon::AuditStage::IntentSynced};
    for (const auto stage :
         {daemon::AuditStage::LogoutSendStarted, daemon::AuditStage::LogoutClosedConfirmed}) {
        auto checkpoint = daemon::make_logout_audit_checkpoint(identity, *plan, stage, error);
        REQUIRE(checkpoint);
        REQUIRE(audit.append(daemon::serialize(*checkpoint), failure));
        completed.push_back(stage);
    }
    auto outcome = daemon::make_logout_success_audit_outcome(identity, *plan, completed, error);
    REQUIRE(outcome);
    REQUIRE(audit.append(daemon::serialize(*outcome), failure));
}

} // namespace

TEST_CASE("logout waits for correlated Closed after its ok response",
          "[logout][dispatch][audit][lifecycle]") {
    const LogoutTree tree;
    FakeLogout logout(tree);
    auto outcome = logout.dispatch();
    REQUIRE(logout.runtime().wait_for_sent(2));
    const auto sent = logout.runtime().sent_functions();
    CHECK(sent[1].function.has_type("logOut"));
    logout.runtime().push_response(logout.first(), sent[1].query_id,
                                   core::TdValue::from(core::TdOk{}));
    logout.runtime().push_update(logout.first(), {},
                                 core::AuthStateData{core::AuthState::LoggingOut});
    logout.runtime().push_update(logout.first(), {}, core::AuthStateData{core::AuthState::Closed});

    const auto result = outcome.get();
    CHECK(result.result == json{{"account", "main"}, {"logged_out", true}});
    CHECK(result.terminal_count == 1);
    const auto records = audit_records(tree);
    REQUIRE(records.size() == 4);
    CHECK(records[0]["phase"] == "intent");
    CHECK(records[1]["stage"] == "logout_send_started");
    CHECK(records[2]["stage"] == "logout_closed_confirmed");
    CHECK(records[3]["phase"] == "outcome");
    CHECK(records[3]["mutation_state"] == "confirmed");
}

TEST_CASE("logout accepts Closed before its response and ignores the old late response",
          "[logout][dispatch][lifecycle]") {
    const LogoutTree tree;
    FakeLogout logout(tree);
    auto outcome = logout.dispatch();
    REQUIRE(logout.runtime().wait_for_sent(2));
    const auto sent = logout.runtime().sent_functions();
    logout.runtime().push_update(logout.first(), {}, core::AuthStateData{core::AuthState::Closed});
    const auto result = outcome.get();
    CHECK(result.result == json{{"account", "main"}, {"logged_out", true}});
    logout.runtime().push_response(logout.first(), sent[1].query_id,
                                   core::TdValue::from(core::TdError{500, "late"}));
}

TEST_CASE("logout response and Closed races settle exactly once under stress",
          "[logout][dispatch][lifecycle][race][stress]") {
    for (int iteration = 0; iteration < 24; ++iteration) {
        CAPTURE(iteration);
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto sent = logout.runtime().sent_functions();

        std::thread response([&] {
            logout.runtime().push_response(logout.first(), sent[1].query_id,
                                           core::TdValue::from(core::TdOk{}));
        });
        std::thread closed([&] {
            logout.runtime().push_update(logout.first(), {},
                                         core::AuthStateData{core::AuthState::Closed});
        });
        response.join();
        closed.join();

        const auto result = outcome.get();
        CHECK(result.result == json{{"account", "main"}, {"logged_out", true}});
        CHECK(result.terminal_count == 1);
        CHECK(audit_records(tree).size() == 4);
    }
}

TEST_CASE("same-daemon audit preflights serialize behind an active logout invocation",
          "[logout][dispatch][audit][concurrency]") {
    const LogoutTree tree;
    FakeLogout logout(tree);
    auto logout_outcome = logout.dispatch();
    REQUIRE(logout.runtime().wait_for_sent(2));
    auto doctor_outcome = logout.dispatch(proto::WriteAuthority::Unset, true, false, 2.0, false,
                                          std::nullopt, {"doctor"});
    CHECK(doctor_outcome.wait_for(20ms) == std::future_status::timeout);

    logout.runtime().push_update(logout.first(), {}, core::AuthStateData{core::AuthState::Closed});
    REQUIRE(logout_outcome.get().result);
    const auto doctor = doctor_outcome.get();
    CHECK(doctor.result == json{{"checked", true}});
    CHECK(doctor.terminal_count == 1);
    CHECK(audit_records(tree).size() == 4);
}

TEST_CASE("blocked audit preflight obeys its own deadline without a second send",
          "[logout][dispatch][audit][concurrency][deadline]") {
    const LogoutTree tree;
    FakeLogout logout(tree);
    auto active = logout.dispatch();
    REQUIRE(logout.runtime().wait_for_sent(2));

    const auto started = std::chrono::steady_clock::now();
    const auto blocked = logout
                             .dispatch(proto::WriteAuthority::Unset, true, false, 0.03, false,
                                       std::nullopt, {"doctor"})
                             .get();
    CHECK(std::chrono::steady_clock::now() - started < 500ms);
    REQUIRE(blocked.error);
    CHECK((*blocked.error)["error"]["code"] == "TIMEOUT");
    CHECK((*blocked.error)["error"]["details"] == json{{"operation", "audit"}, {"state", nullptr}});
    CHECK(logout.runtime().sent_functions().size() == 2);

    logout.runtime().push_update(logout.first(), {}, core::AuthStateData{core::AuthState::Closed});
    REQUIRE(active.get().result);
}

TEST_CASE("blocked audit preflight stops on disconnect before entering the audit",
          "[logout][dispatch][audit][concurrency][disconnect]") {
    const LogoutTree tree;
    FakeLogout logout(tree);
    auto active = logout.dispatch();
    REQUIRE(logout.runtime().wait_for_sent(2));
    const auto records_before = audit_records(tree);

    auto blocked = logout.dispatch_controlled(proto::WriteAuthority::Unset, true, false, 2.0, false,
                                              std::nullopt, {"doctor"});
    CHECK(blocked.outcome.wait_for(20ms) == std::future_status::timeout);
    blocked.session->disconnect();
    REQUIRE(blocked.outcome.wait_for(500ms) == std::future_status::ready);
    const auto cancelled = blocked.outcome.get();
    CHECK_FALSE(cancelled.result);
    CHECK_FALSE(cancelled.error);
    CHECK(cancelled.terminal_count == 0);
    CHECK(audit_records(tree) == records_before);
    CHECK(logout.runtime().sent_functions().size() == 2);

    logout.runtime().push_update(logout.first(), {}, core::AuthStateData{core::AuthState::Closed});
    REQUIRE(active.get().result);
}

TEST_CASE("logout durably records post-send shutdown and disconnect before terminating",
          "[logout][dispatch][audit][lifecycle]") {
    SECTION("daemon shutdown") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto controlled = logout.dispatch_controlled();
        REQUIRE(logout.runtime().wait_for_sent(2));
        controlled.session->shutdown();

        const auto result = controlled.outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "DAEMON_SHUTDOWN");
        CHECK(result.terminal_count == 1);
        const auto records = audit_records(tree);
        REQUIRE(records.size() == 3);
        CHECK(records.back()["phase"] == "outcome");
        CHECK(records.back()["error"]["code"] == "DAEMON_SHUTDOWN");
        CHECK(records.back()["mutation_state"] == "possible");
    }

    SECTION("client disconnect") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto controlled = logout.dispatch_controlled();
        REQUIRE(logout.runtime().wait_for_sent(2));
        controlled.session->disconnect();

        const auto result = controlled.outcome.get();
        CHECK_FALSE(result.result);
        CHECK_FALSE(result.error);
        CHECK(result.terminal_count == 0);
        const auto records = audit_records(tree);
        REQUIRE(records.size() == 3);
        CHECK(records.back()["phase"] == "outcome");
        CHECK(records.back()["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");
        CHECK(records.back()["error"]["details"]["reason"] == "transport_lost");
        CHECK(records.back()["mutation_state"] == "possible");
    }
}

TEST_CASE("logout authority and confirmation failures precede audit and TDLib",
          "[logout][dispatch][safety]") {
    SECTION("no grant") {
        const LogoutTree tree(false);
        FakeLogout logout(tree);
        const auto outcome = logout.dispatch().get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "WRITE_DENIED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "no_grant");
        CHECK(logout.runtime().sent_functions().size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }
    SECTION("explicit deny overrides standing grant") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        const auto outcome = logout.dispatch(proto::WriteAuthority::Deny).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["details"]["reason"] == "explicit_deny");
        CHECK(logout.runtime().sent_functions().size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }
    SECTION("non-TTY without yes") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        const auto outcome = logout.dispatch(proto::WriteAuthority::Unset, false, false).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
        CHECK(outcome.challenges.empty());
        CHECK(logout.runtime().sent_functions().size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }
    SECTION("negative TTY answer") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        const auto outcome =
            logout.dispatch(proto::WriteAuthority::Unset, false, true, 2.0, false, false).get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "CONFIRMATION_REQUIRED");
        REQUIRE(outcome.challenges.size() == 1);
        CHECK(outcome.challenges[0]["details"]["action"] == "logout");
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }
    SECTION("TTY confirmation deadline") {
        const LogoutTree tree;
        auto hooks = fixed_hooks();
        std::optional<daemon::ChallengeSpec> requested_challenge;
        hooks->challenge_provider = [&](daemon::ChallengeSpec spec) {
            requested_challenge = std::move(spec);
            return daemon::ChallengeOutcome{daemon::ChallengeStatus::TimedOut, std::monostate{}};
        };
        FakeLogout logout(tree, hooks);
        const auto outcome = logout.dispatch(proto::WriteAuthority::Unset, false, true).get();
        REQUIRE(outcome.error);
        REQUIRE(requested_challenge);
        CHECK(requested_challenge->kind == proto::ChallengeKind::DestructiveConfirmation);
        CHECK(requested_challenge->client_generation == 1);
        CHECK(requested_challenge->auth_sequence == 1);
        CHECK(requested_challenge->details["action"] == "logout");
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "logout"}, {"state", "ready"}});
        CHECK(logout.runtime().sent_functions().size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }
}

TEST_CASE("logout confirmation follows the current auth occurrence",
          "[logout][dispatch][challenge][auth][safety]") {
    SECTION("a repeated Ready update replaces the stale challenge") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        bool advanced = false;
        auto outcome = logout.dispatch(proto::WriteAuthority::Unset, false, true, 2.0, false, true,
                                       {"logout"}, [&](std::size_t challenge_count) {
                                           if (challenge_count == 1) {
                                               logout.runtime().push_update(
                                                   logout.first(), {},
                                                   core::AuthStateData{core::AuthState::Ready});
                                               advanced = logout.wait_occurrence(1, 2);
                                           }
                                       });
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});

        const auto result = outcome.get();
        REQUIRE(advanced);
        REQUIRE(result.result);
        REQUIRE(result.challenges.size() == 2);
        CHECK(result.challenges[0]["auth_sequence"] == 1);
        CHECK(result.challenges[1]["auth_sequence"] == 2);
        CHECK(result.challenges[0]["nonce"] != result.challenges[1]["nonce"]);
        CHECK(audit_records(tree).front()["confirmation_source"] == "tty");
    }

    SECTION("a generation change terminates without intent or logOut") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        bool replaced = false;
        const auto result =
            logout
                .dispatch(proto::WriteAuthority::Unset, false, true, 2.0, false, true, {"logout"},
                          [&](std::size_t challenge_count) {
                              if (challenge_count == 1) {
                                  logout.runtime().push_update(
                                      logout.first(), {},
                                      core::AuthStateData{core::AuthState::Closed});
                                  replaced = logout.wait_occurrence(2, 0);
                              }
                          })
                .get();
        REQUIRE(replaced);
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "NOT_AUTHED");
        CHECK(result.challenges.size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
        const auto sent = logout.runtime().sent_functions();
        CHECK(std::none_of(sent.begin(), sent.end(), [](const auto& function) {
            return function.function.has_type("logOut");
        }));
    }
}

TEST_CASE("logout dry-run returns the exact plan without authority audit or TDLib",
          "[logout][dispatch][dry-run]") {
    const LogoutTree tree(false);
    FakeLogout logout(tree);
    const auto outcome =
        logout.dispatch(proto::WriteAuthority::Deny, false, false, 2.0, true).get();
    CHECK(outcome.result == json{{"dry_run", true},
                                 {"plan",
                                  {{"operation", "logout"},
                                   {"account", "main"},
                                   {"remote_logout", true},
                                   {"tdlib_request", "logOut"}}}});
    CHECK(logout.runtime().sent_functions().size() == 1);
    CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
}

TEST_CASE("direct logout dry-run never reconciles a prior destructive audit",
          "[logout][dispatch][dry-run][audit][safety]") {
    const LogoutTree tree(false);
    append_prior_logout(tree, {});
    const auto before = file_bytes(tree.audit_path());
    FakeLogout logout(tree);

    const auto outcome =
        logout.dispatch(proto::WriteAuthority::Deny, false, false, 2.0, true).get();

    CHECK(outcome.result == json{{"dry_run", true},
                                 {"plan",
                                  {{"operation", "logout"},
                                   {"account", "main"},
                                   {"remote_logout", true},
                                   {"tdlib_request", "logOut"}}}});
    CHECK(file_bytes(tree.audit_path()) == before);
    CHECK(logout.runtime().sent_functions().size() == 1);
}

TEST_CASE("logout rejects implicit main at daemon-side config admission",
          "[logout][dispatch][config][safety]") {
    SECTION("valid empty config is an absent account") {
        const LogoutTree tree;
        tree.write_config("");
        FakeLogout logout(tree);

        const auto outcome = logout.dispatch(proto::WriteAuthority::Grant, true, false).get();

        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "ACCOUNT_NOT_FOUND");
        CHECK((*outcome.error)["error"]["details"] == json{{"account", "main"}});
        CHECK(logout.runtime().sent_functions().size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }

    SECTION("invalid reload over empty last-good remains config invalid") {
        const LogoutTree tree;
        tree.write_config("");
        FakeLogout logout(tree);
        tree.write_config("[accounts.main\n");

        const auto outcome = logout.dispatch(proto::WriteAuthority::Grant, true, false).get();

        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "CONFIG_INVALID");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"path", tree.config_path()}, {"reason", "parse_error"}});
        CHECK(logout.runtime().sent_functions().size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }
}

TEST_CASE("logout preflight reconciles every definite prior audit prefix before a new send",
          "[logout][dispatch][audit][reconciliation]") {
    SECTION("intent without dispatch") {
        const LogoutTree tree;
        append_prior_logout(tree, {});
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(outcome.get().result);

        const auto records = audit_records(tree);
        REQUIRE(records.size() == 6);
        CHECK(records[1]["phase"] == "outcome");
        CHECK(records[1]["success"] == false);
        CHECK(records[1]["error"]["code"] == "INTERNAL");
        CHECK(records[1]["mutation_state"] == "none");
    }

    SECTION("send without Closed") {
        const LogoutTree tree;
        append_prior_logout(tree, {daemon::AuditStage::LogoutSendStarted});
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(outcome.get().result);

        const auto records = audit_records(tree);
        REQUIRE(records.size() == 7);
        CHECK(records[2]["phase"] == "outcome");
        CHECK(records[2]["success"] == false);
        CHECK(records[2]["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");
        CHECK(records[2]["error"]["details"]["state"] == "ready");
        CHECK(records[2]["mutation_state"] == "possible");
        CHECK(logout.runtime().sent_functions().size() >= 2);
    }

    SECTION("correlated Closed without outcome") {
        const LogoutTree tree;
        append_prior_logout(tree, {daemon::AuditStage::LogoutSendStarted,
                                   daemon::AuditStage::LogoutClosedConfirmed});
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(outcome.get().result);

        const auto records = audit_records(tree);
        REQUIRE(records.size() == 8);
        CHECK(records[3]["phase"] == "outcome");
        CHECK(records[3]["success"] == true);
        CHECK(records[3]["mutation_state"] == "confirmed");
    }
}

TEST_CASE("logout recovery opens the configured TDLib database only to observe auth state",
          "[logout][audit][reconciliation][bootstrap]") {
    SECTION("bootstrap reaches an observed state and syncs a possible outcome") {
        const LogoutTree tree(true, true);
        append_prior_logout(tree, {daemon::AuditStage::LogoutSendStarted});
        FakeLogout logout(tree, {}, {}, core::AuthState::WaitTdlibParameters);

        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto sent = logout.runtime().sent_functions();
        REQUIRE(sent[1].function.has_type("setTdlibParameters"));
        logout.runtime().push_response(logout.first(), sent[1].query_id,
                                       core::TdValue::from(core::TdOk{}));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::WaitPhoneNumber});

        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "NOT_AUTHED");
        const auto records = audit_records(tree);
        REQUIRE(records.size() == 3);
        CHECK(records.back()["phase"] == "outcome");
        CHECK(records.back()["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");
        CHECK(records.back()["error"]["details"]["state"] == "wait_phone_number");
        CHECK(records.back()["mutation_state"] == "possible");
        CHECK(logout.runtime().sent_functions().size() == 2);
    }

    SECTION("deadline leaves the intent unmatched and reports its synced prefix") {
        const LogoutTree tree(true, true);
        append_prior_logout(tree, {daemon::AuditStage::LogoutSendStarted});
        FakeLogout logout(tree, {}, {}, core::AuthState::WaitTdlibParameters);

        auto outcome = logout.dispatch(proto::WriteAuthority::Unset, true, false, 0.03);
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "AUDIT_INCOMPLETE");
        CHECK((*result.error)["error"]["details"]["mutation_state"] == "possible");
        CHECK((*result.error)["error"]["details"]["completed_stages"] ==
              json::array({"intent_synced", "logout_send_started"}));
        CHECK(audit_records(tree).size() == 2);
    }
}

TEST_CASE("logout treats a standing grant from an invalid reload as invalid authority",
          "[logout][dispatch][config][safety]") {
    SECTION("unset request authority is denied") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        REQUIRE(logout.dispatch(proto::WriteAuthority::Deny, false, false, 2.0, true).get().result);
        tree.write_config("[accounts.main\n");

        const auto outcome = logout.dispatch().get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "WRITE_DENIED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "invalid_config_grant");
        CHECK(logout.runtime().sent_functions().size() == 1);
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
    }

    SECTION("an explicit request grant remains independently valid") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        REQUIRE(logout.dispatch(proto::WriteAuthority::Deny, false, false, 2.0, true).get().result);
        tree.write_config("[accounts.main\n");

        auto outcome = logout.dispatch(proto::WriteAuthority::Grant);
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(outcome.get().result);
        CHECK(audit_records(tree).front()["authority_source"] == "request");
    }
}

TEST_CASE("logout checks Ready before confirmation or audit", "[logout][dispatch][auth][safety]") {
    const LogoutTree tree;
    FakeLogout logout(tree);
    logout.runtime().push_update(logout.first(), {},
                                 core::AuthStateData{core::AuthState::WaitPhoneNumber});
    REQUIRE(logout.wait_state(core::AuthState::WaitPhoneNumber));

    const auto outcome =
        logout.dispatch(proto::WriteAuthority::Unset, false, true, 2.0, false, true).get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"account", "main"}, {"state", "wait_phone_number"}, {"reason", "login_required"}});
    CHECK(outcome.challenges.empty());
    CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
}

TEST_CASE("logout audit faults fail closed at intent checkpoint and outcome",
          "[logout][dispatch][audit][safety]") {
    SECTION("intent write failure") {
        const LogoutTree tree;
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->should_fail = [](daemon::LogoutAuditFault fault) {
            return fault == daemon::LogoutAuditFault::Write;
        };
        FakeLogout logout(tree, fixed_hooks(audit));
        const auto outcome = logout.dispatch().get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "AUDIT_UNAVAILABLE");
        CHECK(logout.runtime().sent_functions().size() == 1);
    }
    SECTION("send checkpoint sync failure") {
        const LogoutTree tree;
        int syncs = 0;
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->should_fail = [&syncs](daemon::LogoutAuditFault fault) {
            return fault == daemon::LogoutAuditFault::Sync && ++syncs == 2;
        };
        bool fatal = false;
        FakeLogout logout(tree, fixed_hooks(audit), [&fatal] { fatal = true; });
        const auto outcome = logout.dispatch().get();
        CHECK_FALSE(outcome.result);
        CHECK_FALSE(outcome.error);
        CHECK(outcome.terminal_count == 0);
        CHECK(fatal);
        CHECK(logout.runtime().sent_functions().size() == 1);
    }
    SECTION("outcome sync failure") {
        const LogoutTree tree;
        int syncs = 0;
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->should_fail = [&syncs](daemon::LogoutAuditFault fault) {
            return fault == daemon::LogoutAuditFault::Sync && ++syncs == 4;
        };
        bool fatal = false;
        FakeLogout logout(tree, fixed_hooks(audit), [&fatal] { fatal = true; });
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        const auto result = outcome.get();
        CHECK_FALSE(result.result);
        CHECK_FALSE(result.error);
        CHECK(result.terminal_count == 0);
        CHECK(fatal);
    }
}

TEST_CASE("logout rechecks deadline and shutdown after syncing the send checkpoint",
          "[logout][dispatch][audit][deadline][safety]") {
    SECTION("deadline expires during checkpoint persistence") {
        const LogoutTree tree;
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->after_sync = [](std::string_view phase) {
            if (phase == "checkpoint") {
                std::this_thread::sleep_for(40ms);
            }
        };
        FakeLogout logout(tree, fixed_hooks(audit));
        const auto result = logout.dispatch(proto::WriteAuthority::Unset, true, false, 0.01).get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");
        CHECK((*result.error)["error"]["details"]["reason"] == "timeout");
        CHECK(logout.runtime().sent_functions().size() == 1);
        const auto records = audit_records(tree);
        REQUIRE(records.size() == 3);
        CHECK(records.back()["mutation_state"] == "possible");
    }

    SECTION("shutdown arrives while checkpoint persistence is blocked") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool checkpoint_synced = false;
        bool release = false;
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->after_sync = [&](std::string_view phase) {
            if (phase != "checkpoint") {
                return;
            }
            std::unique_lock lock(mutex);
            checkpoint_synced = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, fixed_hooks(audit));
        auto controlled = logout.dispatch_controlled();
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return checkpoint_synced; }));
        }
        controlled.session->shutdown();
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        const auto result = controlled.outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "DAEMON_SHUTDOWN");
        CHECK(logout.runtime().sent_functions().size() == 1);
        const auto records = audit_records(tree);
        REQUIRE(records.size() == 3);
        CHECK(records.back()["error"]["code"] == "DAEMON_SHUTDOWN");
    }
}

TEST_CASE("logout atomically designates audited ownership before intent",
          "[logout][dispatch][audit][lifecycle][safety]") {
    SECTION("deadline expires before designation") {
        const LogoutTree tree;
        auto hooks = fixed_hooks();
        hooks->before_intent = [] { std::this_thread::sleep_for(30ms); };
        FakeLogout logout(tree, hooks);

        const auto outcome = logout.dispatch(proto::WriteAuthority::Unset, true, false, 0.01).get();

        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
        CHECK(logout.runtime().sent_functions().size() == 1);
    }

    for (const bool shutdown : {false, true}) {
        DYNAMIC_SECTION(
            (shutdown ? "shutdown before designation" : "disconnect before designation")) {
            const LogoutTree tree;
            std::mutex mutex;
            std::condition_variable condition;
            bool reached = false;
            bool release = false;
            auto hooks = fixed_hooks();
            hooks->before_intent = [&] {
                std::unique_lock lock(mutex);
                reached = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release; });
            };
            FakeLogout logout(tree, hooks);
            auto controlled = logout.dispatch_controlled();
            {
                std::unique_lock lock(mutex);
                REQUIRE(condition.wait_for(lock, 2s, [&] { return reached; }));
            }
            if (shutdown) {
                controlled.session->shutdown();
                controlled.session->shutdown();
            } else {
                controlled.session->disconnect();
            }
            {
                const std::lock_guard lock(mutex);
                release = true;
            }
            condition.notify_all();

            const auto outcome = controlled.outcome.get();
            if (shutdown) {
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] == "DAEMON_SHUTDOWN");
            } else {
                CHECK_FALSE(outcome.error);
                CHECK_FALSE(outcome.result);
            }
            CHECK_FALSE(std::filesystem::exists(tree.audit_path()));
            CHECK(logout.runtime().sent_functions().size() == 1);
        }
    }
}

TEST_CASE("logout audit rotation keeps complete invocation groups and strict file metadata",
          "[logout][audit][rotation][safety]") {
    SECTION("rotation occurs only before the next intent") {
        const LogoutTree tree;
        const config::Store store(tree.config_path());
        const auto loaded = store.load();
        REQUIRE(loaded.snapshot);
        auto audit_hooks = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit_hooks->rotation_bytes = 1;
        daemon::LogoutAuditLog audit(tree.account_state(), "main", tree.environment().uid,
                                     audit_hooks);

        append_complete_logout(audit, loaded.snapshot->identity,
                               "11111111111111111111111111111111");
        append_complete_logout(audit, loaded.snapshot->identity,
                               "22222222222222222222222222222222");

        const auto rotated = jsonl_records(tree.audit_path() + ".1");
        const auto active = audit_records(tree);
        REQUIRE(rotated.size() == 4);
        REQUIRE(active.size() == 4);
        for (const auto& record : rotated) {
            CHECK(record["invocation_id"] == "11111111111111111111111111111111");
        }
        for (const auto& record : active) {
            CHECK(record["invocation_id"] == "22222222222222222222222222222222");
        }
        CHECK(audit.inspect().status == daemon::LogoutAuditInspectionStatus::Clean);
    }

    SECTION("unsafe audit file metadata is rejected") {
        const LogoutTree tree;
        const config::Store store(tree.config_path());
        const auto loaded = store.load();
        REQUIRE(loaded.snapshot);
        daemon::LogoutAuditLog audit(tree.account_state(), "main", tree.environment().uid);
        append_complete_logout(audit, loaded.snapshot->identity,
                               "33333333333333333333333333333333");
        REQUIRE(::chmod(tree.audit_path().c_str(), 0644) == 0);

        CHECK(audit.inspect().status == daemon::LogoutAuditInspectionStatus::Invalid);
        daemon::LogoutAuditFailure failure;
        CHECK_FALSE(audit.append(json::object(), failure));
        CHECK(failure.reason == "path_invalid");
    }
}

TEST_CASE("logout audit inspector rejects malformed or noncanonical records",
          "[logout][audit][safety]") {
    const LogoutTree tree;
    {
        std::ofstream output(tree.audit_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output
            << R"({"schema_version":1,"phase":"intent","invocation_id":"0123456789abcdef0123456789abcdef","timestamp":"not-a-time","account":"main","command":"logout","arguments":{},"plan":{"operation":"logout","account":"main","remote_logout":true,"tdlib_request":"logOut"},"config_snapshot":"missing","authority_source":"config","confirmation_source":"yes"})"
            << '\n';
    }
    REQUIRE(::chmod(tree.audit_path().c_str(), 0600) == 0);
    const daemon::LogoutAuditLog audit(tree.account_state(), "main", tree.environment().uid);
    CHECK(audit.inspect().status == daemon::LogoutAuditInspectionStatus::Invalid);

    FakeLogout logout(tree);
    const auto outcome = logout
                             .dispatch(proto::WriteAuthority::Unset, true, false, 2.0, false,
                                       std::nullopt, {"doctor"})
                             .get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "AUDIT_UNAVAILABLE");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"account", "main"}, {"path", tree.audit_path()}, {"reason", "path_invalid"}});
}

TEST_CASE("logout audit preserves a synced prefix before a trailing partial record",
          "[logout][audit][reconciliation][safety]") {
    const LogoutTree tree;
    append_prior_logout(tree, {daemon::AuditStage::LogoutSendStarted});
    {
        std::ofstream output(tree.audit_path(), std::ios::binary | std::ios::app);
        REQUIRE(output.good());
        output << R"({"schema_version":1,"phase":"outcome")";
    }

    FakeLogout logout(tree);
    const auto result = logout
                            .dispatch(proto::WriteAuthority::Unset, true, false, 2.0, false,
                                      std::nullopt, {"doctor"})
                            .get();
    REQUIRE(result.error);
    CHECK((*result.error)["error"]["code"] == "AUDIT_INCOMPLETE");
    CHECK((*result.error)["error"]["details"]["mutation_state"] == "possible");
    CHECK((*result.error)["error"]["details"]["completed_stages"] ==
          json::array({"intent_synced", "logout_send_started"}));
}

TEST_CASE("fresh inspection reestablishes outcome durability after an audit-fatal sync failure",
          "[logout][audit][reconciliation][safety]") {
    const LogoutTree tree;
    {
        int syncs = 0;
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->should_fail = [&syncs](daemon::LogoutAuditFault fault) {
            return fault == daemon::LogoutAuditFault::Sync && ++syncs == 4;
        };
        bool fatal = false;
        FakeLogout logout(tree, fixed_hooks(audit), [&] { fatal = true; });
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        const auto result = outcome.get();
        CHECK_FALSE(result.result);
        CHECK_FALSE(result.error);
        CHECK(fatal);
    }

    {
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->should_fail = [](daemon::LogoutAuditFault fault) {
            return fault == daemon::LogoutAuditFault::InspectSync;
        };
        FakeLogout logout(tree, fixed_hooks(audit));
        const auto result = logout
                                .dispatch(proto::WriteAuthority::Unset, true, false, 2.0, false,
                                          std::nullopt, {"doctor"})
                                .get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "AUDIT_UNAVAILABLE");
        CHECK((*result.error)["error"]["details"]["reason"] == "sync_failed");
    }

    {
        FakeLogout logout(tree);
        const auto result = logout
                                .dispatch(proto::WriteAuthority::Unset, true, false, 2.0, false,
                                          std::nullopt, {"doctor"})
                                .get();
        CHECK(result.result == json{{"checked", true}});
        CHECK(audit_records(tree).size() == 4);
    }
}

TEST_CASE("logout maps pre-transition TD errors and post-send timeout without claiming success",
          "[logout][dispatch][lifecycle][audit]") {
    SECTION("TDLib error") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto sent = logout.runtime().sent_functions();
        logout.runtime().push_response(logout.first(), sent[1].query_id,
                                       core::TdValue::from(core::TdError{400, "bad request"}));
        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "TDLIB_ERROR");
        CHECK(audit_records(tree).back()["mutation_state"] == "possible");
    }
    SECTION("rate limit") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto sent = logout.runtime().sent_functions();
        logout.runtime().push_response(
            logout.first(), sent[1].query_id,
            core::TdValue::from(core::TdError{429, "retry after 17 seconds"}));
        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK(result.exit_code == kRateLimited);
        CHECK((*result.error)["error"]["code"] == "RATE_LIMITED");
        CHECK((*result.error)["error"]["details"] ==
              json{{"operation", "logout"}, {"tdlib_code", 429}, {"retry_after", 17}});
    }
    SECTION("rate limit clamps oversized retry values without signed overflow") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto sent = logout.runtime().sent_functions();
        logout.runtime().push_response(
            logout.first(), sent[1].query_id,
            core::TdValue::from(core::TdError{429, "retry after 21474836499999999999 seconds"}));
        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "RATE_LIMITED");
        CHECK((*result.error)["error"]["details"]["retry_after"] ==
              std::numeric_limits<std::int32_t>::max());
    }
    SECTION("expected transition wins over a late TDLib error") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto sent = logout.runtime().sent_functions();
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closing});
        REQUIRE(logout.wait_state(core::AuthState::Closing));
        logout.runtime().push_response(logout.first(), sent[1].query_id,
                                       core::TdValue::from(core::TdError{500, "late"}));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        const auto result = outcome.get();
        CHECK(result.result == json{{"account", "main"}, {"logged_out", true}});
        CHECK(result.terminal_count == 1);
    }
    SECTION("timeout") {
        const LogoutTree tree;
        FakeLogout logout(tree);
        auto outcome = logout.dispatch(proto::WriteAuthority::Unset, true, false, 0.2);
        REQUIRE(logout.runtime().wait_for_sent(2));
        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");
        CHECK((*result.error)["error"]["details"]["reason"] == "timeout");
        CHECK(audit_records(tree).back()["mutation_state"] == "possible");
    }
}

TEST_CASE("logout resolves TD errors by receive commit order rather than polling order",
          "[logout][dispatch][lifecycle][ordering]") {
    SECTION("error committed before a direct Closed update wins") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool sent = false;
        bool release = false;
        auto hooks = fixed_hooks();
        hooks->after_send = [&] {
            std::unique_lock lock(mutex);
            sent = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, hooks);
        auto outcome = logout.dispatch();
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
        }
        const auto functions = logout.runtime().sent_functions();
        logout.runtime().push_response(logout.first(), functions[1].query_id,
                                       core::TdValue::from(core::TdError{400, "first"}));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(logout.wait_state(core::AuthState::Closed));
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "TDLIB_ERROR");
    }

    SECTION("direct Closed update committed before an error wins") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool sent = false;
        bool release = false;
        auto hooks = fixed_hooks();
        hooks->after_send = [&] {
            std::unique_lock lock(mutex);
            sent = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, hooks);
        auto outcome = logout.dispatch();
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
        }
        const auto functions = logout.runtime().sent_functions();
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        logout.runtime().push_response(logout.first(), functions[1].query_id,
                                       core::TdValue::from(core::TdError{400, "late"}));
        REQUIRE(logout.wait_state(core::AuthState::Closed));
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        const auto result = outcome.get();
        CHECK(result.result == json{{"account", "main"}, {"logged_out", true}});
        CHECK(result.terminal_count == 1);
    }

    SECTION("error committed before LoggingOut wins") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool sent = false;
        bool release = false;
        auto hooks = fixed_hooks();
        hooks->after_send = [&] {
            std::unique_lock lock(mutex);
            sent = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, hooks);
        auto outcome = logout.dispatch();
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
        }
        const auto functions = logout.runtime().sent_functions();
        logout.runtime().push_response(logout.first(), functions[1].query_id,
                                       core::TdValue::from(core::TdError{400, "first"}));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::LoggingOut});
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(logout.wait_state(core::AuthState::Closed));
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        const auto result = outcome.get();
        REQUIRE(result.error);
        CHECK((*result.error)["error"]["code"] == "TDLIB_ERROR");
    }

    SECTION("LoggingOut committed before an error wins") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool sent = false;
        bool release = false;
        auto hooks = fixed_hooks();
        hooks->after_send = [&] {
            std::unique_lock lock(mutex);
            sent = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, hooks);
        auto outcome = logout.dispatch();
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
        }
        const auto functions = logout.runtime().sent_functions();
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::LoggingOut});
        logout.runtime().push_response(logout.first(), functions[1].query_id,
                                       core::TdValue::from(core::TdError{400, "late"}));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(logout.wait_state(core::AuthState::Closed));
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        const auto result = outcome.get();
        CHECK(result.result == json{{"account", "main"}, {"logged_out", true}});
        CHECK(result.terminal_count == 1);
    }
}

TEST_CASE("logout lifecycle gate orders terminal events and replacement",
          "[logout][dispatch][lifecycle][ordering][gate]") {
    SECTION("close before send is generation loss and holds replacement through outcome") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool close_reserved = false;
        auto hooks = fixed_hooks();
        FakeLogout logout(tree, hooks);
        hooks->during_terminal_claim = [&] {
            const std::lock_guard lock(mutex);
            close_reserved = true;
            condition.notify_all();
        };
        hooks->before_send = [&] {
            logout.runtime().push_update(logout.first(), {},
                                         core::AuthStateData{core::AuthState::Closed});
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return close_reserved; }));
        };

        const auto outcome = logout.dispatch().get();

        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");
        CHECK((*outcome.error)["error"]["details"]["reason"] == "generation_lost");
        const auto sent = logout.runtime().sent_functions();
        CHECK(std::none_of(sent.begin(), sent.end(),
                           [](const auto& entry) { return entry.function.has_type("logOut"); }));
        const auto records = audit_records(tree);
        REQUIRE(records.size() == 3);
        CHECK(records.back()["mutation_state"] == "possible");
        REQUIRE(logout.runtime().wait_for_clients(2));
    }

    SECTION("replacement waits for the synced Closed decision") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        int checkpoints = 0;
        bool closed_synced = false;
        bool release = false;
        auto audit = std::make_shared<daemon::testing::LogoutAuditHooks>();
        audit->after_sync = [&](std::string_view phase) {
            if (phase != "checkpoint" || ++checkpoints != 2) {
                return;
            }
            std::unique_lock lock(mutex);
            closed_synced = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, fixed_hooks(audit));
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return closed_synced; }));
        }
        CHECK(logout.runtime().clients().size() == 1);
        CHECK(logout.runtime().sent_functions().size() == 2);
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        REQUIRE(outcome.get().result);
        REQUIRE(logout.runtime().wait_for_clients(2));
    }

    SECTION("Closed committed before deadline shutdown and disconnect wins") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool sent = false;
        bool release = false;
        auto hooks = fixed_hooks();
        hooks->after_send = [&] {
            std::unique_lock lock(mutex);
            sent = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, hooks);
        auto controlled =
            logout.dispatch_controlled(proto::WriteAuthority::Unset, true, false, 0.2);
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
        }
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        REQUIRE(logout.wait_state(core::AuthState::Closed));
        std::this_thread::sleep_for(220ms);
        controlled.session->shutdown();
        controlled.session->shutdown();
        controlled.session->disconnect();
        CHECK(logout.runtime().clients().size() == 1);
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        const auto outcome = controlled.outcome.get();
        CHECK_FALSE(outcome.error);
        CHECK_FALSE(outcome.result);
        CHECK(audit_records(tree).back()["success"] == true);
        REQUIRE(logout.runtime().wait_for_clients(2));
    }

    for (const std::string terminal : {"deadline", "shutdown", "disconnect"}) {
        DYNAMIC_SECTION(terminal << " committed before Closed wins") {
            const LogoutTree tree;
            std::mutex mutex;
            std::condition_variable condition;
            bool sent = false;
            bool release = false;
            auto hooks = fixed_hooks();
            hooks->after_send = [&] {
                std::unique_lock lock(mutex);
                sent = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release; });
            };
            FakeLogout logout(tree, hooks);
            auto controlled = logout.dispatch_controlled(proto::WriteAuthority::Unset, true, false,
                                                         terminal == "deadline" ? 0.2 : 2.0);
            {
                std::unique_lock lock(mutex);
                REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
            }
            if (terminal == "deadline") {
                std::this_thread::sleep_for(220ms);
            } else if (terminal == "shutdown") {
                controlled.session->shutdown();
                controlled.session->shutdown();
            } else {
                controlled.session->disconnect();
            }
            logout.runtime().push_update(logout.first(), {},
                                         core::AuthStateData{core::AuthState::Closed});
            REQUIRE(logout.wait_state(core::AuthState::Closed));
            CHECK(logout.runtime().clients().size() == 1);
            {
                const std::lock_guard lock(mutex);
                release = true;
            }
            condition.notify_all();

            const auto outcome = controlled.outcome.get();
            if (terminal == "deadline") {
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["details"]["reason"] == "timeout");
            } else if (terminal == "shutdown") {
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] == "DAEMON_SHUTDOWN");
            } else {
                CHECK_FALSE(outcome.error);
                CHECK_FALSE(outcome.result);
            }
            CHECK(audit_records(tree).back()["success"] == false);
            REQUIRE(logout.runtime().wait_for_clients(2));
        }
    }

    SECTION("Closed callback reserved before deadline survives delayed claim and owner release") {
        const LogoutTree tree;
        std::mutex mutex;
        std::condition_variable condition;
        bool claim_started = false;
        bool release = false;
        auto hooks = fixed_hooks();
        hooks->during_terminal_claim = [&] {
            std::unique_lock lock(mutex);
            claim_started = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        };
        FakeLogout logout(tree, hooks);
        auto outcome = logout.dispatch(proto::WriteAuthority::Unset, true, false, 0.3);
        REQUIRE(logout.runtime().wait_for_sent(2));
        logout.runtime().push_update(logout.first(), {},
                                     core::AuthStateData{core::AuthState::Closed});
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return claim_started; }));
        }
        std::this_thread::sleep_for(320ms);
        CHECK(outcome.wait_for(20ms) == std::future_status::timeout);
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();

        CHECK(outcome.get().result == json{{"account", "main"}, {"logged_out", true}});
        REQUIRE(logout.runtime().wait_for_clients(2));
    }
}

TEST_CASE("logout terminal gate orders TD errors and progress against deadline",
          "[logout][dispatch][lifecycle][ordering][deadline][gate]") {
    for (const bool error_before_deadline : {false, true}) {
        DYNAMIC_SECTION((error_before_deadline ? "TD error before deadline wins"
                                               : "deadline before TD error wins")) {
            const LogoutTree tree;
            std::mutex mutex;
            std::condition_variable condition;
            bool sent = false;
            bool claimed = false;
            bool release = false;
            auto hooks = fixed_hooks();
            hooks->after_send = [&] {
                std::unique_lock lock(mutex);
                sent = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release; });
            };
            hooks->during_terminal_claim = [&] {
                const std::lock_guard lock(mutex);
                claimed = true;
                condition.notify_all();
            };
            FakeLogout logout(tree, hooks);
            auto outcome = logout.dispatch(proto::WriteAuthority::Unset, true, false, 0.2);
            {
                std::unique_lock lock(mutex);
                REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
            }
            const auto sent_functions = logout.runtime().sent_functions();
            if (!error_before_deadline) {
                std::this_thread::sleep_for(220ms);
            }
            logout.runtime().push_response(logout.first(), sent_functions[1].query_id,
                                           core::TdValue::from(core::TdError{400, "ordered"}));
            {
                std::unique_lock lock(mutex);
                REQUIRE(condition.wait_for(lock, 2s, [&] { return claimed; }));
            }
            if (error_before_deadline) {
                std::this_thread::sleep_for(220ms);
            }
            {
                const std::lock_guard lock(mutex);
                release = true;
            }
            condition.notify_all();

            const auto result = outcome.get();
            REQUIRE(result.error);
            CHECK((*result.error)["error"]["code"] ==
                  (error_before_deadline ? "TDLIB_ERROR" : "REMOTE_LOGOUT_UNCONFIRMED"));
            if (!error_before_deadline) {
                CHECK((*result.error)["error"]["details"]["reason"] == "timeout");
            }
        }
    }

    for (const bool progress_before_deadline : {false, true}) {
        DYNAMIC_SECTION((progress_before_deadline ? "progress before deadline suppresses late error"
                                                  : "deadline before progress remains timeout")) {
            const LogoutTree tree;
            std::mutex mutex;
            std::condition_variable condition;
            bool sent = false;
            int claims = 0;
            bool release = false;
            auto hooks = fixed_hooks();
            hooks->after_send = [&] {
                std::unique_lock lock(mutex);
                sent = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release; });
            };
            hooks->during_terminal_claim = [&] {
                const std::lock_guard lock(mutex);
                ++claims;
                condition.notify_all();
            };
            FakeLogout logout(tree, hooks);
            auto outcome = logout.dispatch(proto::WriteAuthority::Unset, true, false, 0.2);
            {
                std::unique_lock lock(mutex);
                REQUIRE(condition.wait_for(lock, 2s, [&] { return sent; }));
            }
            if (!progress_before_deadline) {
                std::this_thread::sleep_for(220ms);
            }
            logout.runtime().push_update(logout.first(), {},
                                         core::AuthStateData{core::AuthState::LoggingOut});
            {
                std::unique_lock lock(mutex);
                REQUIRE(condition.wait_for(lock, 2s, [&] { return claims >= 1; }));
            }
            if (progress_before_deadline) {
                std::this_thread::sleep_for(220ms);
            }
            const auto sent_functions = logout.runtime().sent_functions();
            logout.runtime().push_response(logout.first(), sent_functions[1].query_id,
                                           core::TdValue::from(core::TdError{400, "late"}));
            {
                const std::lock_guard lock(mutex);
                release = true;
            }
            condition.notify_all();

            const auto result = outcome.get();
            REQUIRE(result.error);
            CHECK((*result.error)["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");
            CHECK((*result.error)["error"]["details"]["reason"] == "timeout");
        }
    }
}

TEST_CASE("logout prunes completed lifecycle waiters on a long-lived generation",
          "[logout][dispatch][lifecycle][gate][stress]") {
    const LogoutTree tree;
    std::uint64_t identity = 0;
    auto hooks = fixed_hooks();
    hooks->invocation_id = [&] {
        std::array<char, 33> rendered{};
        std::snprintf(rendered.data(), rendered.size(), "%032llx",
                      static_cast<unsigned long long>(++identity));
        return std::string(rendered.data());
    };
    FakeLogout logout(tree, hooks);

    for (std::size_t iteration = 0; iteration < 24; ++iteration) {
        CAPTURE(iteration);
        auto outcome = logout.dispatch();
        REQUIRE(logout.runtime().wait_for_sent(iteration + 2));
        const auto sent = logout.runtime().sent_functions();
        logout.runtime().push_response(
            logout.first(), sent.back().query_id,
            core::TdValue::from(core::TdError{400, "retryable test failure"}));
        REQUIRE(outcome.get().error);
        CHECK(logout.runtime().clients().size() == 1);
    }

    auto outcome = logout.dispatch();
    REQUIRE(logout.runtime().wait_for_sent(26));
    logout.runtime().push_update(logout.first(), {}, core::AuthStateData{core::AuthState::Closed});
    REQUIRE(outcome.get().result);
    REQUIRE(logout.runtime().wait_for_clients(2));
}
