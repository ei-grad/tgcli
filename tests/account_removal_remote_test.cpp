#include "daemon/account_removal_remote.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;
using namespace tgcli::daemon;

namespace {

constexpr std::string_view kSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
    "ino:2;size:3;ctime_ns:4";

class TempRemoteTree final {
  public:
    TempRemoteTree() {
        std::string pattern = "/tmp/tgcli-removal-remote-XXXXXX";
        pattern.push_back('\0');
        root_ = ::mkdtemp(pattern.data());
        REQUIRE_FALSE(root_.empty());
        environment_.home = root_;
        environment_.xdg_config_home = root_ + "/config";
        environment_.xdg_data_home = root_ + "/data";
        environment_.xdg_state_home = root_ + "/state";
        environment_.xdg_runtime_dir = root_ + "/run";
        environment_.uid = ::getuid();
    }

    ~TempRemoteTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempRemoteTree(const TempRemoteTree&) = delete;
    TempRemoteTree& operator=(const TempRemoteTree&) = delete;
    TempRemoteTree(TempRemoteTree&&) = delete;
    TempRemoteTree& operator=(TempRemoteTree&&) = delete;

    [[nodiscard]] config::Store store() const {
        return config::Store(paths::config_file(environment_), environment_.uid);
    }

    [[nodiscard]] const paths::Environment& environment() const {
        return environment_;
    }

  private:
    std::string root_;
    paths::Environment environment_;
};

std::shared_ptr<const config::ConfigSnapshot> snapshot() {
    auto value = std::make_shared<config::ConfigSnapshot>();
    value->identity = std::string(kSnapshot);
    value->default_account = "work";
    config::AccountConfig account;
    account.api_id = 12345;
    account.api_hash = "hash";
    value->accounts.emplace("work", std::move(account));
    return value;
}

proto::AccountRemovePlan plan(bool data_present = true) {
    std::string error;
    auto value = proto::make_account_remove_plan(
        {.account = "work",
         .keep_session = false,
         .delete_paths = {"/data/tgcli/accounts/work", "/state/tgcli/accounts/work"},
         .config_path = "/config/tgcli/config.toml",
         .config_snapshot = std::string(kSnapshot),
         .data_root = data_present ? std::optional<proto::RootIdentity>{proto::RootIdentity{
                                         "/data/tgcli/accounts/work", 1, 2, 1000}}
                                   : std::nullopt,
         .state_root = std::nullopt,
         .reassign_default = std::nullopt},
        error);
    if (!value) {
        throw std::runtime_error("failed to construct account removal plan: " + error);
    }
    return *value;
}

struct SessionFixture {
    explicit SessionFixture(double timeout = 1.0)
        : sink([](const nlohmann::json&) {}, [](const nlohmann::json&) {},
               [](const nlohmann::json&) {},
               [](const std::string&, const std::string&, const nlohmann::json&, int) {}),
          session(make_request(timeout), sink) {
        if (session.begin_audited_terminal() != AuditedTerminalStatus::Designated) {
            throw std::runtime_error("failed to designate audited removal terminal");
        }
    }

    static proto::Request make_request(double timeout) {
        proto::Request value("work");
        value.id = 1;
        value.command = {"account", "remove"};
        value.context.timeout_seconds = timeout;
        return value;
    }

    CallbackSink sink;
    RequestSession session;
};

void close_replacement(test::ScriptedTdRuntime& scripted, core::TdClient& client,
                       std::size_t expected_sent) {
    REQUIRE(scripted.wait_for_clients(2));
    REQUIRE(scripted.wait_for_sent(expected_sent));
    const auto replacement = scripted.clients().at(1);
    scripted.push_response(replacement, 1, {},
                           core::AuthStateData{core::AuthState::WaitTdlibParameters});
    CHECK(client.close_until(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

} // namespace

TEST_CASE("TD removal proves absent storage without sending parameters or logout",
          "[removal][remote]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {},
                            core::AuthStateData{core::AuthState::WaitTdlibParameters});
    SessionFixture request;
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
    std::vector<AuditStage> stages;
    const auto proof = remote.prove_remote_logout(plan(false), snapshot(), false, request.session,
                                                  [&](AuditStage stage) {
                                                      stages.push_back(stage);
                                                      return true;
                                                  });
    REQUIRE(std::holds_alternative<AccountRemoveRemoteResult>(proof));
    CHECK(std::get<AccountRemoveRemoteResult>(proof) == AccountRemoveRemoteResult::NotPresent);
    CHECK(stages == std::vector{AuditStage::RemoteNotPresent});
    CHECK(scripted->sent_functions().size() == 1);
}

TEST_CASE("TD removal syncs send-start before dispatching logOut", "[removal][remote][audit]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
    SessionFixture request;
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
    std::mutex mutex;
    std::condition_variable condition;
    bool send_checkpoint_started = false;
    bool release_checkpoint = false;
    std::vector<AuditStage> stages;
    auto proof = std::async(std::launch::async, [&] {
        return remote.prove_remote_logout(
            plan(), snapshot(), false, request.session, [&](AuditStage stage) {
                stages.push_back(stage);
                if (stage == AuditStage::RemoteLogoutSendStarted) {
                    std::unique_lock lock(mutex);
                    send_checkpoint_started = true;
                    condition.notify_all();
                    condition.wait(lock, [&] { return release_checkpoint; });
                }
                return true;
            });
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return send_checkpoint_started; }));
    }
    CHECK(scripted->sent_functions().size() == 1);
    {
        const std::lock_guard lock(mutex);
        release_checkpoint = true;
    }
    condition.notify_all();
    REQUIRE(scripted->wait_for_sent(2));
    const auto logout = scripted->sent_functions().at(1);
    REQUIRE(logout.function.has_type("logOut"));
    scripted->push_response(generation, logout.query_id,
                            core::TdValue::from(core::TdError{500, "stop"}));
    REQUIRE(std::holds_alternative<RemovalOperationError>(proof.get()));
    CHECK(stages == std::vector{AuditStage::RemoteLogoutSendStarted});
}

TEST_CASE("TD removal rejects a disconnected pre-send without checkpoint or logOut",
          "[removal][remote][lifecycle]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
    SessionFixture request;
    request.session.disconnect();
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
    std::vector<AuditStage> stages;
    const auto proof = remote.prove_remote_logout(plan(), snapshot(), false, request.session,
                                                  [&](AuditStage stage) {
                                                      stages.push_back(stage);
                                                      return true;
                                                  });
    REQUIRE(std::holds_alternative<RemovalOperationError>(proof));
    const auto& error = std::get<RemovalOperationError>(proof);
    CHECK(error.code == "REMOTE_LOGOUT_UNCONFIRMED");
    CHECK(error.details["reason"] == "transport_lost");
    CHECK(stages.empty());
    const auto sent = scripted->sent_functions();
    CHECK(std::none_of(sent.begin(), sent.end(),
                       [](const auto& entry) { return entry.function.has_type("logOut"); }));
}

TEST_CASE("TD removal rejects a generation closed after send-start without logOut or proof",
          "[removal][remote][lifecycle][ordering]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
    SessionFixture request;
    std::mutex mutex;
    std::condition_variable condition;
    bool closed_claim_started = false;
    auto hooks = std::make_shared<testing::AccountRemovalRemoteHooks>();
    hooks->during_terminal_claim = [&] {
        const std::lock_guard lock(mutex);
        closed_claim_started = true;
        condition.notify_all();
    };
    hooks->before_send = [&] {
        scripted->push_update(generation, {}, core::AuthStateData{core::AuthState::Closed});
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(2),
                                [&] { return closed_claim_started; })) {
            throw std::runtime_error("Closed lifecycle claim did not start");
        }
    };
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0", {}, {},
                                  secret_hook::run, hooks);
    std::vector<AuditStage> stages;
    const auto proof = remote.prove_remote_logout(plan(), snapshot(), false, request.session,
                                                  [&](AuditStage stage) {
                                                      stages.push_back(stage);
                                                      return true;
                                                  });
    REQUIRE(std::holds_alternative<RemovalOperationError>(proof));
    const auto& error = std::get<RemovalOperationError>(proof);
    CHECK(error.code == "REMOTE_LOGOUT_UNCONFIRMED");
    CHECK(error.details["reason"] == "generation_lost");
    CHECK(stages == std::vector{AuditStage::RemoteLogoutSendStarted});
    const auto sent = scripted->sent_functions();
    CHECK(std::none_of(sent.begin(), sent.end(),
                       [](const auto& entry) { return entry.function.has_type("logOut"); }));
    close_replacement(*scripted, client, 2);
}

TEST_CASE("TD removal requires same-generation Closed after logOut and then quiesces replacement",
          "[removal][remote][lifecycle]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
    SessionFixture request;
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0", {}, {});
    std::vector<AuditStage> stages;
    std::mutex mutex;
    std::condition_variable condition;
    bool remote_checkpoint_started = false;
    bool release_checkpoint = false;
    auto proof = std::async(std::launch::async, [&] {
        return remote.prove_remote_logout(
            plan(), snapshot(), false, request.session, [&](AuditStage stage) {
                stages.push_back(stage);
                if (stage == AuditStage::RemoteConfirmed) {
                    std::unique_lock lock(mutex);
                    remote_checkpoint_started = true;
                    condition.notify_all();
                    condition.wait(lock, [&] { return release_checkpoint; });
                }
                return true;
            });
    });
    REQUIRE(scripted->wait_for_sent(2));
    const auto sent = scripted->sent_functions();
    CHECK(sent[1].function.has_type("logOut"));
    scripted->push_update(generation, {}, core::AuthStateData{core::AuthState::Closed});
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return remote_checkpoint_started; }));
    }
    CHECK(scripted->clients().size() == 1);
    {
        const std::lock_guard lock(mutex);
        release_checkpoint = true;
    }
    condition.notify_all();
    const auto result = proof.get();
    scripted->push_response(generation, sent[1].query_id, core::TdValue::from(core::TdOk{}));
    if (const auto* error = std::get_if<RemovalOperationError>(&result)) {
        INFO(error->code << " " << error->details.dump());
    }
    REQUIRE(std::holds_alternative<AccountRemoveRemoteResult>(result));
    CHECK(std::get<AccountRemoveRemoteResult>(result) == AccountRemoveRemoteResult::Confirmed);
    CHECK(stages == std::vector{AuditStage::RemoteLogoutSendStarted, AuditStage::RemoteConfirmed});

    REQUIRE(scripted->wait_for_clients(2));
    const auto replacement = scripted->clients()[1];
    REQUIRE(scripted->wait_for_sent(3));
    scripted->push_response(replacement, 1, {},
                            core::AuthStateData{core::AuthState::WaitTdlibParameters});
    CHECK_FALSE(remote.quiesce(request.session));
}

TEST_CASE("TD removal recovery resends from ready without duplicating the durable send checkpoint",
          "[removal][remote][recovery]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
    SessionFixture request;
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
    std::vector<AuditStage> stages;
    auto proof = std::async(std::launch::async, [&] {
        return remote.prove_remote_logout(plan(), snapshot(), true, request.session,
                                          [&](AuditStage stage) {
                                              stages.push_back(stage);
                                              return true;
                                          });
    });
    REQUIRE(scripted->wait_for_sent(2));
    scripted->push_update(generation, {}, core::AuthStateData{core::AuthState::Closed});
    const auto result = proof.get();
    if (const auto* error = std::get_if<RemovalOperationError>(&result)) {
        INFO(error->code << " " << error->details.dump());
    }
    REQUIRE(scripted->wait_for_clients(2));
    const auto replacement = scripted->clients()[1];
    REQUIRE(scripted->wait_for_sent(3));
    scripted->push_response(replacement, 1, {},
                            core::AuthStateData{core::AuthState::WaitTdlibParameters});
    CHECK_FALSE(remote.quiesce(request.session));
    REQUIRE(std::holds_alternative<AccountRemoveRemoteResult>(result));
    CHECK(std::get<AccountRemoveRemoteResult>(result) == AccountRemoveRemoteResult::Confirmed);
    CHECK(stages == std::vector{AuditStage::RemoteConfirmed});
}

TEST_CASE("TD removal rejects partially authenticated states without a logout send",
          "[removal][remote]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::WaitCode});
    SessionFixture request;
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
    const auto proof = remote.prove_remote_logout(plan(), snapshot(), false, request.session,
                                                  [](AuditStage) { return true; });
    REQUIRE(std::holds_alternative<RemovalOperationError>(proof));
    CHECK(std::get<RemovalOperationError>(proof).code == "REMOTE_LOGOUT_UNCONFIRMED");
    CHECK(std::get<RemovalOperationError>(proof).details["state"] == "wait_code");
    CHECK(scripted->sent_functions().size() == 1);
}

TEST_CASE("TD removal reports an unconfirmed timeout when initial auth state is unavailable",
          "[removal][remote][timeout]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    SessionFixture request(0.02);
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
    const auto proof = remote.prove_remote_logout(plan(), snapshot(), false, request.session,
                                                  [](AuditStage) { return true; });
    REQUIRE(std::holds_alternative<RemovalOperationError>(proof));
    const auto& error = std::get<RemovalOperationError>(proof);
    CHECK(error.code == "REMOTE_LOGOUT_UNCONFIRMED");
    CHECK(error.details ==
          nlohmann::json{{"account", "work"}, {"state", "unknown"}, {"reason", "timeout"}});
    REQUIRE(scripted->wait_for_clients(1));
    scripted->push_response(scripted->clients().front(), 1, {},
                            core::AuthStateData{core::AuthState::WaitTdlibParameters});
    CHECK(client.close_until(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST_CASE("TD removal maps logout errors without accepting them as remote proof",
          "[removal][remote][error]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
    SessionFixture request;
    std::mutex mutex;
    std::condition_variable condition;
    bool claim_started = false;
    bool release_claim = false;
    auto hooks = std::make_shared<testing::AccountRemovalRemoteHooks>();
    hooks->during_terminal_claim = [&] {
        std::unique_lock lock(mutex);
        claim_started = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release_claim; });
    };
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0", {}, {},
                                  secret_hook::run, hooks);
    auto proof = std::async(std::launch::async, [&] {
        return remote.prove_remote_logout(plan(), snapshot(), false, request.session,
                                          [](AuditStage) { return true; });
    });
    REQUIRE(scripted->wait_for_sent(2));
    const auto logout = scripted->sent_functions().at(1);
    scripted->push_response(generation, logout.query_id,
                            core::TdValue::from(core::TdError{500, "server failure"}));
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return claim_started; }));
    }
    scripted->push_update(generation, {}, core::AuthStateData{core::AuthState::Closed});
    {
        const std::lock_guard lock(mutex);
        release_claim = true;
    }
    condition.notify_all();
    const auto result = proof.get();
    REQUIRE(std::holds_alternative<RemovalOperationError>(result));
    const auto& error = std::get<RemovalOperationError>(result);
    CHECK(error.code == "TDLIB_ERROR");
    CHECK(error.details == nlohmann::json{{"operation", "account_remove"}, {"tdlib_code", 500}});
    close_replacement(*scripted, client, 3);
}

TEST_CASE("TD removal terminal events committed before Closed never publish remote success",
          "[removal][remote][lifecycle][ordering]") {
    for (const std::string terminal : {"deadline", "disconnect", "shutdown"}) {
        DYNAMIC_SECTION(terminal) {
            const TempRemoteTree tree;
            auto runtime = std::make_unique<test::ScriptedTdRuntime>();
            auto* scripted = runtime.get();
            core::TdClient client(std::move(runtime));
            const auto store = tree.store();
            REQUIRE(scripted->wait_for_clients(1));
            const auto generation = scripted->clients().front();
            scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
            SessionFixture request(terminal == "deadline" ? 0.05 : 1.0);
            TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
            std::vector<AuditStage> stages;
            auto proof = std::async(std::launch::async, [&] {
                return remote.prove_remote_logout(plan(), snapshot(), false, request.session,
                                                  [&](AuditStage stage) {
                                                      stages.push_back(stage);
                                                      return true;
                                                  });
            });
            REQUIRE(scripted->wait_for_sent(2));
            if (terminal == "deadline") {
                std::this_thread::sleep_for(std::chrono::milliseconds(70));
            } else if (terminal == "disconnect") {
                request.session.disconnect();
            } else {
                request.session.shutdown();
            }
            scripted->push_update(generation, {}, core::AuthStateData{core::AuthState::Closed});

            const auto result = proof.get();
            REQUIRE(std::holds_alternative<RemovalOperationError>(result));
            const auto& error = std::get<RemovalOperationError>(result);
            if (terminal == "shutdown") {
                CHECK(error.code == "DAEMON_SHUTDOWN");
            } else {
                CHECK(error.code == "REMOTE_LOGOUT_UNCONFIRMED");
                CHECK(error.details["reason"] ==
                      (terminal == "deadline" ? "timeout" : "transport_lost"));
            }
            CHECK(stages == std::vector{AuditStage::RemoteLogoutSendStarted});
            close_replacement(*scripted, client, 3);
        }
    }
}

TEST_CASE("TD removal does not treat a logOut ok response as confirmation",
          "[removal][remote][timeout]") {
    const TempRemoteTree tree;
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* scripted = runtime.get();
    core::TdClient client(std::move(runtime));
    const auto store = tree.store();
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();
    scripted->push_response(generation, 1, {}, core::AuthStateData{core::AuthState::Ready});
    SessionFixture request(0.05);
    TdAccountRemovalRemote remote(client, store, tree.environment(), "work", "1.0.0");
    auto proof = std::async(std::launch::async, [&] {
        return remote.prove_remote_logout(plan(), snapshot(), false, request.session,
                                          [](AuditStage) { return true; });
    });
    REQUIRE(scripted->wait_for_sent(2));
    const auto logout = scripted->sent_functions().at(1);
    scripted->push_response(generation, logout.query_id, core::TdValue::from(core::TdOk{}));
    const auto result = proof.get();
    REQUIRE(std::holds_alternative<RemovalOperationError>(result));
    const auto& error = std::get<RemovalOperationError>(result);
    CHECK(error.code == "REMOTE_LOGOUT_UNCONFIRMED");
    CHECK(error.details["state"] == "ready");
    CHECK(error.details["reason"] == "timeout");
}
