#include "common/config.hpp"
#include "common/config_test_support.hpp"
#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/login_commands.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
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

class AuthTree {
  public:
    AuthTree() {
        std::string pattern = "/tmp/tgcli-login-command-XXXXXX";
        pattern.push_back('\0');
        const char* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;
        for (const auto& directory :
             {config_root(), data_root(), state_root(), runtime_root(), config_root() + "/tgcli"}) {
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        }
        std::ofstream output(config_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << "default_account = \"main\"\n\n[accounts.main]\napi_id = 12345\n"
                  "api_hash = \"hash-value\"\nallow_write = false\n";
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

    ~AuthTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    AuthTree(const AuthTree&) = delete;
    AuthTree& operator=(const AuthTree&) = delete;
    AuthTree(AuthTree&&) = delete;
    AuthTree& operator=(AuthTree&&) = delete;

    [[nodiscard]] std::string config_root() const {
        return root_ + "/config";
    }
    [[nodiscard]] std::string data_root() const {
        return root_ + "/data";
    }
    [[nodiscard]] std::string state_root() const {
        return root_ + "/state";
    }
    [[nodiscard]] std::string runtime_root() const {
        return root_ + "/runtime";
    }
    [[nodiscard]] std::string config_path() const {
        return config_root() + "/tgcli/config.toml";
    }
    void write_config(std::string_view bytes) const {
        std::ofstream output(config_path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }
    void install_transaction_marker() const {
        const auto lock_path = config_root() + "/tgcli/config.lock";
        std::ofstream lock(lock_path, std::ios::binary | std::ios::trunc);
        REQUIRE(lock.good());
        lock.close();
        REQUIRE(::chmod(lock_path.c_str(), 0600) == 0);

        const auto marker_path = config_root() + "/tgcli/.config.toml.transaction";
        std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
        REQUIRE(marker.good());
        marker << "pending-present\n"
               << std::string(64, '0') << '\n'
               << std::string(64, '1') << '\n';
        marker.close();
        REQUIRE(::chmod(marker_path.c_str(), 0600) == 0);
    }
    void clear_transaction_marker() const {
        REQUIRE(std::filesystem::remove(config_root() + "/tgcli/.config.toml.transaction"));
    }
    [[nodiscard]] paths::Environment environment() const {
        paths::Environment result;
        result.xdg_config_home = config_root();
        result.xdg_data_home = data_root();
        result.xdg_state_home = state_root();
        result.xdg_runtime_dir = runtime_root();
        result.home = root_;
        result.uid = ::getuid();
        return result;
    }

  private:
    std::string root_;
};

class FakeAuth {
  public:
    FakeAuth(const AuthTree& tree, core::AuthStateData initial,
             core::AuthBootstrap::HookRunner hook_runner = secret_hook::run,
             bool close_automatically = true, std::string account = "main",
             std::optional<std::string> environment_api_id = {},
             std::optional<std::string> environment_api_hash = {},
             std::shared_ptr<const config::testing::StoreHooks> store_hooks = {})
        : store_(tree.config_path(), std::move(store_hooks)) {
        auto runtime = std::make_unique<test::ScriptedTdRuntime>(close_automatically);
        runtime_ = runtime.get();
        client_ = std::make_unique<core::TdClient>(std::move(runtime));
        REQUIRE(runtime_->wait_for_sent(1));
        first_ = runtime_->clients().front();
        runtime_->push_response(first_, 1, {}, std::move(initial));
        REQUIRE(wait_state_sequence(1));
        coordinator_ = std::make_unique<daemon::LoginCoordinator>(
            *client_, store_, tree.environment(), std::move(account), "0.1.0-test",
            std::move(environment_api_id), std::move(environment_api_hash), std::move(hook_runner));
        daemon::register_login_commands(dispatcher_, *coordinator_);
    }

    ~FakeAuth() {
        if (client_) {
            const auto snapshot = client_->auth_state();
            if (snapshot && snapshot->auth_sequence == 0) {
                const auto clients = runtime_->clients();
                if (!clients.empty()) {
                    runtime_->push_response(clients.back(), 1, {},
                                            core::AuthStateData{core::AuthState::Ready});
                    static_cast<void>(wait_state_sequence(1));
                }
            }
            client_->close();
        }
    }

    FakeAuth(const FakeAuth&) = delete;
    FakeAuth& operator=(const FakeAuth&) = delete;
    FakeAuth(FakeAuth&&) = delete;
    FakeAuth& operator=(FakeAuth&&) = delete;

    [[nodiscard]] bool wait_state_sequence(std::uint64_t sequence) const {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (client_->auth_state()->auth_sequence >= sequence) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return client_->auth_state()->auth_sequence >= sequence;
    }

    core::TdClient& client() {
        return *client_;
    }
    test::ScriptedTdRuntime& runtime() {
        return *runtime_;
    }
    [[nodiscard]] test::ScriptedClient first() const {
        return first_;
    }
    [[nodiscard]] const daemon::Dispatcher& dispatcher() const {
        return dispatcher_;
    }

  private:
    config::Store store_;
    test::ScriptedTdRuntime* runtime_ = nullptr;
    test::ScriptedClient first_{};
    std::unique_ptr<core::TdClient> client_;
    std::unique_ptr<daemon::LoginCoordinator> coordinator_;
    daemon::Dispatcher dispatcher_;
};

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    std::vector<json> progress;
    std::vector<json> challenges;
    int exit_code = -1;
    int terminal_count = 0;
};

json answer_for(const json& challenge) {
    json answer{{"nonce", challenge.at("nonce")},
                {"sequence", challenge.at("sequence")},
                {"client_generation", challenge.at("client_generation")},
                {"auth_sequence", challenge.at("auth_sequence")}};
    const auto kind = challenge.at("kind").get<std::string>();
    if (kind == "registration_terms") {
        answer["value"] = true;
    } else if (kind == "phone_number") {
        answer["value"] = "+12025550123";
    } else if (kind == "authentication_code") {
        answer["value"] = "12345";
    } else if (kind == "email_address") {
        answer["value"] = "ada@example.test";
    } else if (kind == "email_code") {
        answer["value"] = "654321";
    } else if (kind == "password") {
        answer["value"] = "not-logged-secret";
    } else if (kind == "bot_token") {
        answer["value"] = "not-logged-bot-token";
    } else if (kind == "registration_first_name") {
        answer["value"] = "Ada";
    } else if (kind == "registration_last_name") {
        answer["value"] = "Lovelace";
    } else if (kind == "api_id") {
        answer["value"] = "54321";
    } else if (kind == "api_hash") {
        answer["value"] = "replacement-hash";
    } else if (kind == "database_key") {
        answer["value"] = "replacement-db-key";
    } else {
        answer["value"] = "value";
    }
    return answer;
}

Outcome dispatch(const daemon::Dispatcher& dispatcher, std::vector<std::string> command,
                 json args = json::object(), bool tty = true, std::optional<double> timeout = 2.0,
                 std::function<std::optional<json>(const json&)> answer = answer_for) {
    Outcome outcome;
    daemon::CallbackSink sink(
        [](const json&) {},
        [&outcome](json progress) { outcome.progress.push_back(std::move(progress)); },
        [&outcome](json result) {
            outcome.result = std::move(result);
            outcome.exit_code = kOk;
            ++outcome.terminal_count;
        },
        [&outcome](std::string code, std::string message, json details, int exit_code) {
            outcome.error = json{{"error",
                                  {{"code", std::move(code)},
                                   {"message", std::move(message)},
                                   {"details", std::move(details)}}}};
            outcome.exit_code = exit_code;
            ++outcome.terminal_count;
        },
        [&outcome, &answer](const json& challenge) -> std::optional<json> {
            outcome.challenges.push_back(challenge);
            return answer(challenge);
        });
    proto::Request request("main");
    request.id = 77;
    request.command = std::move(command);
    request.args = std::move(args);
    request.context.tty = tty;
    request.context.timeout_seconds = timeout;
    dispatcher.dispatch(request, sink);
    return outcome;
}

std::unique_ptr<daemon::CallbackSink> callback_sink(Outcome& outcome) {
    return std::make_unique<daemon::CallbackSink>(
        [](const json&) {},
        [&outcome](json progress) { outcome.progress.push_back(std::move(progress)); },
        [&outcome](json result) {
            outcome.result = std::move(result);
            outcome.exit_code = kOk;
            ++outcome.terminal_count;
        },
        [&outcome](std::string code, std::string message, json details, int exit_code) {
            outcome.error = json{{"error",
                                  {{"code", std::move(code)},
                                   {"message", std::move(message)},
                                   {"details", std::move(details)}}}};
            outcome.exit_code = exit_code;
            ++outcome.terminal_count;
        },
        [&outcome](const json& challenge) -> std::optional<json> {
            outcome.challenges.push_back(challenge);
            return answer_for(challenge);
        });
}

core::TdUserSummary ada() {
    return {.id = 123456,
            .first_name = "Ada",
            .last_name = "Lovelace",
            .usernames = {"ada"},
            .phone_number = "12025550123",
            .is_bot = false,
            .is_premium = true};
}

const core::TdFieldValue* field(const core::TdFunctionData& function, std::string_view name) {
    for (const auto& candidate : function.fields()) {
        if (candidate.has_name(name)) {
            return &candidate.value();
        }
    }
    return nullptr;
}

struct LoginFunctionSetup {
    core::AuthStateData state;
    json args{{"qr", false}, {"bot", false}};
    std::size_t answers_before_send = 0;
};

LoginFunctionSetup login_setup(core::TdFunctionKind function) {
    switch (function) {
    case core::TdFunctionKind::SetTdlibParameters:
        return {core::AuthStateData{core::AuthState::WaitTdlibParameters}};
    case core::TdFunctionKind::SetAuthenticationPhoneNumber:
        return {core::AuthStateData{core::AuthState::WaitPhoneNumber},
                {{"qr", false}, {"bot", false}},
                1};
    case core::TdFunctionKind::RequestQrCodeAuthentication:
        return {core::AuthStateData{core::AuthState::WaitPhoneNumber},
                {{"qr", true}, {"bot", false}},
                0};
    case core::TdFunctionKind::CheckAuthenticationBotToken:
        return {core::AuthStateData{core::AuthState::WaitPhoneNumber},
                {{"qr", false}, {"bot", true}},
                1};
    case core::TdFunctionKind::SetAuthenticationEmailAddress:
        return {
            core::AuthStateData{core::AuthState::WaitEmailAddress, core::AuthWaitEmailAddress{}},
            {{"qr", false}, {"bot", false}},
            1};
    case core::TdFunctionKind::CheckAuthenticationEmailCode:
        return {core::AuthStateData{
                    core::AuthState::WaitEmailCode,
                    core::AuthWaitEmailCode{.allow_apple_id = false,
                                            .allow_google_id = false,
                                            .email_address_pattern = "a***@example.test",
                                            .expected_length = 6,
                                            .reset_state = core::AuthEmailResetState::None,
                                            .reset_delay = 0,
                                            .unsupported_reset_tdlib_type_id = std::nullopt}},
                {{"qr", false}, {"bot", false}},
                1};
    case core::TdFunctionKind::CheckAuthenticationCode:
        return {core::AuthStateData{
                    core::AuthState::WaitCode,
                    core::AuthWaitCode{.delivery = {core::AuthCodeDelivery::Sms, 5, std::nullopt},
                                       .next_delivery = std::nullopt,
                                       .resend_timeout = 30}},
                {{"qr", false}, {"bot", false}},
                1};
    case core::TdFunctionKind::RegisterUser:
        return {core::AuthStateData{core::AuthState::WaitRegistration,
                                    core::AuthWaitRegistration{.terms_text = "terms",
                                                               .minimum_user_age = 16,
                                                               .show_popup = true}},
                {{"qr", false}, {"bot", false}},
                3};
    case core::TdFunctionKind::CheckAuthenticationPassword:
        return {
            core::AuthStateData{core::AuthState::WaitPassword,
                                core::AuthWaitPassword{.hint = "hint",
                                                       .has_recovery_email_address = true,
                                                       .has_passport_data = false,
                                                       .recovery_email_address_pattern = "a***"}},
            {{"qr", false}, {"bot", false}},
            1};
    case core::TdFunctionKind::GetMe:
        return {core::AuthStateData{core::AuthState::Ready}};
    case core::TdFunctionKind::GetAuthorizationState:
    case core::TdFunctionKind::GetOption:
    case core::TdFunctionKind::GetSavedMessagesTags:
    case core::TdFunctionKind::SearchSavedMessages:
    case core::TdFunctionKind::GetActiveSessions:
    case core::TdFunctionKind::TerminateSession:
    case core::TdFunctionKind::LogOut:
    case core::TdFunctionKind::Close:
        break;
    }
    FAIL("unsupported login function setup");
    return {};
}

} // namespace

TEST_CASE("me is Ready-only and returns the curated user through the real dispatcher",
          "[auth][login][dispatch][schema]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::Ready});
    auto pending =
        std::async(std::launch::async, [&] { return dispatch(auth.dispatcher(), {"me"}); });
    if (!auth.runtime().wait_for_sent(2)) {
        const auto early = pending.get();
        const auto diagnostic =
            early.error ? early.error->dump() : std::string("me returned without an error");
        INFO(diagnostic);
        FAIL("me did not submit getMe");
    }
    const auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::GetMe);
    auth.runtime().push_response(auth.first(), sent.at(1).query_id, core::TdValue::from(ada()));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(outcome.exit_code == kOk);
    CHECK((*outcome.result)["id"] == 123456);
    CHECK_THAT(*outcome.result, test::matches_json_schema("me.result.schema.json"));
}

TEST_CASE("typed TD authorization failures remain terminal through real dispatch",
          "[auth][login][authorization][dispatch][schema]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::Ready});
    auth.runtime().set_before_send([](const core::TdFunctionData& function) {
        if (function.kind() == core::TdFunctionKind::GetMe) {
            throw core::TdAuthorizationError(core::TdAuthorizationFailure::FunctionDenied);
        }
    });

    const auto outcome = dispatch(auth.dispatcher(), {"me"});
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "AUTH_FUNCTION_DENIED");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"account", "main"}, {"state", "ready"}, {"function", "getMe"}});
    CHECK(outcome.exit_code == kDenied);
    CHECK(outcome.terminal_count == 1);
    CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
}

TEST_CASE("every login wait site reports a typed TD authorization failure once",
          "[auth][login][authorization][dispatch][schema]") {
    const std::array functions{core::TdFunctionKind::SetTdlibParameters,
                               core::TdFunctionKind::SetAuthenticationPhoneNumber,
                               core::TdFunctionKind::GetMe};

    for (const auto function : functions) {
        DYNAMIC_SECTION(core::td_function_name(function)) {
            const AuthTree tree;
            const auto setup = login_setup(function);
            FakeAuth auth(tree, setup.state);
            auth.runtime().set_before_send([function](const core::TdFunctionData& sent) {
                if (sent.kind() == function) {
                    throw core::TdAuthorizationError(core::TdAuthorizationFailure::FunctionDenied);
                }
            });

            const auto outcome = dispatch(auth.dispatcher(), {"login"}, setup.args);
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "AUTH_FUNCTION_DENIED");
            CHECK((*outcome.error)["error"]["details"] ==
                  json{{"account", "main"},
                       {"state", core::auth_state_name(setup.state.state)},
                       {"function", core::td_function_name(function)}});
            CHECK(outcome.exit_code == kDenied);
            CHECK(outcome.terminal_count == 1);
            CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
        }
    }
}

TEST_CASE("phone login follows state updates and returns getMe identity",
          "[auth][login][dispatch][schema]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}});
    });
    if (!auth.runtime().wait_for_sent(2)) {
        const auto early = pending.get();
        const auto diagnostic =
            early.error ? early.error->dump() : std::string("login returned without an error");
        INFO(diagnostic);
        FAIL("phone login did not submit its first auth function");
    }
    auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::SetAuthenticationPhoneNumber);
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(
        auth.first(), {},
        core::AuthStateData{core::AuthState::WaitCode,
                            core::AuthWaitCode{.delivery = {core::AuthCodeDelivery::Sms, 5, {}},
                                               .next_delivery = {},
                                               .resend_timeout = 30}});
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::CheckAuthenticationCode);
    auth.runtime().push_response(auth.first(), sent.at(2).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(auth.runtime().wait_for_sent(4));
    sent = auth.runtime().sent_functions();
    CHECK(sent.at(3).function.kind() == core::TdFunctionKind::GetMe);
    auth.runtime().push_response(auth.first(), sent.at(3).query_id, core::TdValue::from(ada()));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    CHECK(outcome.exit_code == kOk);
    CHECK((*outcome.result)["auth_state"] == "ready");
    CHECK(outcome.challenges.size() == 2);
    CHECK(outcome.challenges.at(0)["kind"] == "phone_number");
    CHECK(outcome.challenges.at(1)["kind"] == "authentication_code");
    CHECK_THAT(*outcome.result, test::matches_json_schema("login.result.schema.json"));
}

TEST_CASE("QR login emits every repeated link and never challenges for an answer",
          "[auth][login][qr][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", true}, {"bot", false}});
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::RequestQrCodeAuthentication);
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(
        auth.first(), {},
        core::AuthStateData{core::AuthState::WaitOtherDeviceConfirmation,
                            core::AuthWaitOtherDeviceConfirmation{"tg://login?token=one"}});
    std::this_thread::sleep_for(20ms);
    auth.runtime().push_update(
        auth.first(), {},
        core::AuthStateData{core::AuthState::WaitOtherDeviceConfirmation,
                            core::AuthWaitOtherDeviceConfirmation{"tg://login?token=two"}});
    std::this_thread::sleep_for(20ms);
    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(2).query_id, core::TdValue::from(ada()));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE(outcome.progress.size() == 2);
    CHECK(outcome.progress.at(0) ==
          json{{"kind", "auth_qr"}, {"auth_sequence", 2}, {"link", "tg://login?token=one"}});
    CHECK(outcome.progress.at(1) ==
          json{{"kind", "auth_qr"}, {"auth_sequence", 3}, {"link", "tg://login?token=two"}});
    CHECK(outcome.challenges.empty());
}

TEST_CASE("non-ready me and concurrent login fail with exact auth errors",
          "[auth][login][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    const auto me = dispatch(auth.dispatcher(), {"me"}, {}, false);
    REQUIRE(me.error);
    CHECK_THAT(*me.error, test::matches_json_schema("auth.error.schema.json"));
    CHECK((*me.error)["error"]["code"] == "NOT_AUTHED");
    CHECK((*me.error)["error"]["details"] ==
          json{{"account", "main"}, {"state", "wait_phone_number"}, {"reason", "not_ready"}});
    CHECK(me.exit_code == kNotAuthed);

    auto first = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 0.25);
    });
    if (!auth.runtime().wait_for_sent(2)) {
        const auto early = first.get();
        const auto diagnostic =
            early.error ? early.error->dump() : std::string("login returned without an error");
        INFO(diagnostic);
        FAIL("active login did not submit its auth function");
    }
    const auto second =
        dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, false);
    static_cast<void>(first.get());
    REQUIRE(second.error);
    CHECK_THAT(*second.error, test::matches_json_schema("auth.error.schema.json"));
    CHECK((*second.error)["error"]["code"] == "AUTH_FLOW_IN_PROGRESS");
    CHECK(second.exit_code == kNotAuthed);
}

TEST_CASE("a listed phone rejection retries with a fresh challenge in the same occurrence",
          "[auth][login][retry][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}});
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdError{406, "PHONE_NUMBER_INVALID"}));
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::SetAuthenticationPhoneNumber);
    auth.runtime().push_response(auth.first(), sent.at(2).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(auth.runtime().wait_for_sent(4));
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(3).query_id, core::TdValue::from(ada()));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE(outcome.progress.size() == 1);
    CHECK(outcome.progress.front() == json{{"kind", "auth_retry"},
                                           {"auth_sequence", 1},
                                           {"state", "wait_phone_number"},
                                           {"credential", "phone_number"},
                                           {"tdlib_code", 406}});
    REQUIRE(outcome.challenges.size() == 2);
    CHECK(outcome.challenges.at(0)["sequence"] == 1);
    CHECK(outcome.challenges.at(1)["sequence"] == 2);
    CHECK(outcome.challenges.at(0)["nonce"] != outcome.challenges.at(1)["nonce"]);
}

TEST_CASE("the closed credential rejection matrix retries every exact tuple",
          "[auth][login][retry][credential-matrix][dispatch][schema]") {
    struct CredentialCase {
        core::TdFunctionKind function;
        std::int32_t code;
        std::string_view message;
        std::string_view credential;
    };
    static constexpr std::array<CredentialCase, 25> cases{{
        {core::TdFunctionKind::SetTdlibParameters, 400,
         "Valid api_id must be provided. Can be obtained at https://my.telegram.org",
         "app_credentials"},
        {core::TdFunctionKind::SetTdlibParameters, 400,
         "Valid api_hash must be provided. Can be obtained at https://my.telegram.org",
         "app_credentials"},
        {core::TdFunctionKind::SetTdlibParameters, 401, "Wrong database encryption key",
         "database_key"},
        {core::TdFunctionKind::SetAuthenticationPhoneNumber, 400, "Phone number must be non-empty",
         "phone_number"},
        {core::TdFunctionKind::SetAuthenticationPhoneNumber, 406, "PHONE_NUMBER_INVALID",
         "phone_number"},
        {core::TdFunctionKind::SetAuthenticationPhoneNumber, 400, "API_ID_INVALID",
         "app_credentials"},
        {core::TdFunctionKind::RequestQrCodeAuthentication, 400, "API_ID_INVALID",
         "app_credentials"},
        {core::TdFunctionKind::CheckAuthenticationBotToken, 400, "API_ID_INVALID",
         "app_credentials"},
        {core::TdFunctionKind::CheckAuthenticationBotToken, 400, "ACCESS_TOKEN_INVALID",
         "bot_token"},
        {core::TdFunctionKind::CheckAuthenticationBotToken, 400, "ACCESS_TOKEN_EXPIRED",
         "bot_token"},
        {core::TdFunctionKind::SetAuthenticationEmailAddress, 400,
         "Email address must be non-empty", "email_address"},
        {core::TdFunctionKind::SetAuthenticationEmailAddress, 400, "EMAIL_INVALID",
         "email_address"},
        {core::TdFunctionKind::CheckAuthenticationEmailCode, 400, "Code must be non-empty",
         "email_code"},
        {core::TdFunctionKind::CheckAuthenticationEmailCode, 400, "CODE_INVALID", "email_code"},
        {core::TdFunctionKind::CheckAuthenticationEmailCode, 400, "EMAIL_VERIFY_EXPIRED",
         "email_code"},
        {core::TdFunctionKind::CheckAuthenticationEmailCode, 400, "PHONE_CODE_EMPTY", "email_code"},
        {core::TdFunctionKind::CheckAuthenticationEmailCode, 400, "PHONE_CODE_INVALID",
         "email_code"},
        {core::TdFunctionKind::CheckAuthenticationEmailCode, 400, "PHONE_CODE_EXPIRED",
         "email_code"},
        {core::TdFunctionKind::CheckAuthenticationCode, 400, "PHONE_CODE_EMPTY",
         "authentication_code"},
        {core::TdFunctionKind::CheckAuthenticationCode, 400, "PHONE_CODE_INVALID",
         "authentication_code"},
        {core::TdFunctionKind::CheckAuthenticationCode, 400, "PHONE_CODE_EXPIRED",
         "authentication_code"},
        {core::TdFunctionKind::RegisterUser, 400, "First name must be non-empty",
         "registration_name"},
        {core::TdFunctionKind::RegisterUser, 400, "FIRSTNAME_INVALID", "registration_name"},
        {core::TdFunctionKind::RegisterUser, 400, "LASTNAME_INVALID", "registration_name"},
        {core::TdFunctionKind::CheckAuthenticationPassword, 400, "PASSWORD_HASH_INVALID",
         "password"},
    }};

    for (const auto& entry : cases) {
        DYNAMIC_SECTION(core::td_function_name(entry.function)
                        << ":" << entry.code << ":" << entry.message) {
            const AuthTree tree;
            const auto setup = login_setup(entry.function);
            FakeAuth auth(tree, setup.state);
            std::size_t answers = 0;
            auto pending = std::async(std::launch::async, [&] {
                return dispatch(auth.dispatcher(), {"login"}, setup.args, true, 0.05,
                                [&](const json& challenge) -> std::optional<json> {
                                    if (answers >= setup.answers_before_send) {
                                        return std::nullopt;
                                    }
                                    ++answers;
                                    return answer_for(challenge);
                                });
            });
            REQUIRE(auth.runtime().wait_for_sent(2));
            const auto sent = auth.runtime().sent_functions();
            REQUIRE(sent.size() == 2);
            CHECK(sent.at(1).function.kind() == entry.function);
            auth.runtime().push_response(
                auth.first(), sent.at(1).query_id,
                core::TdValue::from(core::TdError{entry.code, std::string(entry.message)}));

            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
            CHECK(outcome.exit_code == kTimeout);
            CHECK(outcome.terminal_count == 1);
            REQUIRE(outcome.progress.size() == 1);
            CHECK(outcome.progress.front() ==
                  json{{"kind", "auth_retry"},
                       {"auth_sequence", 1},
                       {"state", core::auth_state_name(setup.state.state)},
                       {"credential", entry.credential},
                       {"tdlib_code", entry.code}});
            CHECK(outcome.challenges.size() == setup.answers_before_send + 1);
            CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
        }
    }
}

TEST_CASE("a listed credential rejection is terminal without a TTY",
          "[auth][login][retry][dispatch][schema]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitTdlibParameters});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, false);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    const auto sent = auth.runtime().sent_functions();
    auth.runtime().push_response(
        auth.first(), sent.at(1).query_id,
        core::TdValue::from(core::TdError{
            400, "Valid api_hash must be provided. Can be obtained at https://my.telegram.org"}));

    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "AUTH_CREDENTIAL_REJECTED");
    CHECK((*outcome.error)["error"]["details"] == json{{"account", "main"},
                                                       {"state", "wait_tdlib_parameters"},
                                                       {"credential", "app_credentials"},
                                                       {"tdlib_code", 400}});
    CHECK(outcome.exit_code == kNotAuthed);
    CHECK(outcome.terminal_count == 1);
    CHECK(outcome.challenges.empty());
    CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
}

TEST_CASE("credential classification rejects every near miss and unlisted TD error",
          "[auth][login][retry][dispatch][schema]") {
    struct ErrorCase {
        std::int32_t code;
        std::string_view message;
    };
    static constexpr std::array cases{
        ErrorCase{406, "phone_number_invalid"},  ErrorCase{406, " PHONE_NUMBER_INVALID"},
        ErrorCase{406, "PHONE_NUMBER_INVALID "}, ErrorCase{400, "PHONE_NUMBER_INVALID"},
        ErrorCase{401, "PHONE_NUMBER_INVALID"},  ErrorCase{400, "ACCESS_TOKEN_INVALID"},
        ErrorCase{500, "SERVER_ERROR"},          ErrorCase{599, "SERVER_ERROR"},
    };

    for (const auto& entry : cases) {
        DYNAMIC_SECTION(entry.code << ":" << entry.message) {
            const AuthTree tree;
            FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
            auto pending = std::async(std::launch::async, [&] {
                return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}});
            });
            REQUIRE(auth.runtime().wait_for_sent(2));
            const auto sent = auth.runtime().sent_functions();
            auth.runtime().push_response(
                auth.first(), sent.at(1).query_id,
                core::TdValue::from(core::TdError{entry.code, std::string(entry.message)}));

            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
            CHECK((*outcome.error)["error"]["details"] ==
                  json{{"operation", "login"}, {"tdlib_code", entry.code}});
            CHECK(outcome.exit_code == kGeneric);
            CHECK(outcome.terminal_count == 1);
            CHECK(outcome.progress.empty());
            CHECK(outcome.challenges.size() == 1);
            CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
        }
    }
}

TEST_CASE("login rate limits parse retry bounds without overflow",
          "[auth][login][rate-limit][dispatch][schema]") {
    struct RateLimitCase {
        std::string_view message;
        std::int32_t retry_after;
    };
    static constexpr std::array cases{
        RateLimitCase{"FLOOD_WAIT_0", 0},
        RateLimitCase{"Too Many Requests: retry after 17 seconds", 17},
        RateLimitCase{"HTTP 429: retry after 17 seconds", 17},
        RateLimitCase{"DC5 FLOOD_WAIT_17", 17},
        RateLimitCase{"HTTP 429: ReTrY\tAfTeR 23 seconds", 23},
        RateLimitCase{"retry after -17 seconds", 0},
        RateLimitCase{"FLOOD_WAIT_-17", 0},
        RateLimitCase{"HTTP 429 without a retry marker", 0},
        RateLimitCase{"notretry after 29 seconds", 0},
        RateLimitCase{"prefix_FLOOD_WAIT_31", 0},
        RateLimitCase{"retry after 2147483647 seconds", std::numeric_limits<std::int32_t>::max()},
        RateLimitCase{"retry after 21474836499999999999 seconds",
                      std::numeric_limits<std::int32_t>::max()},
        RateLimitCase{"flood wait", 0},
    };

    for (const auto& entry : cases) {
        DYNAMIC_SECTION(entry.message) {
            const AuthTree tree;
            FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
            auto pending = std::async(std::launch::async, [&] {
                return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}});
            });
            REQUIRE(auth.runtime().wait_for_sent(2));
            const auto sent = auth.runtime().sent_functions();
            auth.runtime().push_response(
                auth.first(), sent.at(1).query_id,
                core::TdValue::from(core::TdError{429, std::string(entry.message)}));

            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "RATE_LIMITED");
            CHECK((*outcome.error)["error"]["details"] == json{{"operation", "login"},
                                                               {"tdlib_code", 429},
                                                               {"retry_after", entry.retry_after}});
            CHECK(outcome.exit_code == kRateLimited);
            CHECK(outcome.terminal_count == 1);
            CHECK(outcome.progress.empty());
            CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
        }
    }
}

TEST_CASE("login maps the remaining interactive states to the exact TD functions",
          "[auth][login][function-table][dispatch]") {
    const AuthTree tree;
    struct Case {
        core::AuthStateData state;
        core::TdFunctionKind function;
        std::vector<std::string> challenges;
    };
    const std::vector<Case> cases{
        {core::AuthStateData{core::AuthState::WaitEmailAddress, core::AuthWaitEmailAddress{}},
         core::TdFunctionKind::SetAuthenticationEmailAddress,
         {"email_address"}},
        {core::AuthStateData{core::AuthState::WaitEmailCode,
                             core::AuthWaitEmailCode{.allow_apple_id = false,
                                                     .allow_google_id = false,
                                                     .email_address_pattern = "a***@example.test",
                                                     .expected_length = 6,
                                                     .reset_state = core::AuthEmailResetState::None,
                                                     .reset_delay = 0,
                                                     .unsupported_reset_tdlib_type_id = {}}},
         core::TdFunctionKind::CheckAuthenticationEmailCode,
         {"email_code"}},
        {core::AuthStateData{core::AuthState::WaitRegistration,
                             core::AuthWaitRegistration{.terms_text = "terms",
                                                        .minimum_user_age = 16,
                                                        .show_popup = true}},
         core::TdFunctionKind::RegisterUser,
         {"registration_terms", "registration_first_name", "registration_last_name"}},
        {core::AuthStateData{core::AuthState::WaitPassword,
                             core::AuthWaitPassword{.hint = "hint",
                                                    .has_recovery_email_address = true,
                                                    .has_passport_data = false,
                                                    .recovery_email_address_pattern = "a***"}},
         core::TdFunctionKind::CheckAuthenticationPassword,
         {"password"}},
    };

    for (const auto& entry : cases) {
        DYNAMIC_SECTION(core::auth_state_name(entry.state.state)) {
            FakeAuth auth(tree, entry.state);
            auto pending = std::async(std::launch::async, [&] {
                return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}});
            });
            REQUIRE(auth.runtime().wait_for_sent(2));
            const auto sent = auth.runtime().sent_functions();
            CHECK(sent.at(1).function.kind() == entry.function);
            auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                         core::TdValue::from(core::TdError{500, "SERVER_ERROR"}));
            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
            REQUIRE(outcome.challenges.size() == entry.challenges.size());
            for (std::size_t index = 0; index < entry.challenges.size(); ++index) {
                CHECK(outcome.challenges.at(index)["kind"] == entry.challenges.at(index));
            }
            if (entry.function == core::TdFunctionKind::RegisterUser) {
                const auto* descriptor = field(sent.at(1).function, "disable_notification");
                REQUIRE(descriptor != nullptr);
                CHECK(std::get<bool>(*descriptor) == false);
            } else {
                const auto* descriptor = field(sent.at(1).function, "credential");
                REQUIRE(descriptor != nullptr);
                CHECK(std::get<core::TdRedactedValue>(*descriptor) ==
                      core::TdRedactedValue::Credential);
            }
        }
    }
}

TEST_CASE("a rejected database key retries once from a no-echo challenge without rerunning hook",
          "[auth][login][bootstrap][retry][dispatch]") {
    const AuthTree tree;
    tree.write_config("default_account = \"main\"\n\n[accounts.main]\napi_id = 12345\n"
                      "api_hash = \"hash-value\"\ndb_key_cmd = \"ignored\"\n");
    int hook_calls = 0;
    secret_hook::HookField observed_hook_field = secret_hook::HookField::ApiId;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitTdlibParameters},
                  [&hook_calls, &observed_hook_field](const secret_hook::HookRequest& request) {
                      ++hook_calls;
                      observed_hook_field = request.field;
                      return secret_hook::HookResult{"rejected-db-key", {}};
                  });
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}});
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::SetTdlibParameters);
    auth.runtime().push_response(
        auth.first(), sent.at(1).query_id,
        core::TdValue::from(core::TdError{401, "Wrong database encryption key"}));
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::SetTdlibParameters);
    CHECK(hook_calls == 1);
    CHECK(observed_hook_field == secret_hook::HookField::DatabaseKey);
    auth.runtime().push_response(auth.first(), sent.at(2).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    if (!auth.runtime().wait_for_sent(4)) {
        const auto early = pending.get();
        const auto diagnostic =
            early.error ? early.error->dump() : std::string("bootstrap returned without an error");
        INFO(diagnostic);
        FAIL("database-key retry did not reach getMe");
    }
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(3).query_id, core::TdValue::from(ada()));
    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE(outcome.challenges.size() == 1);
    CHECK(outcome.challenges.front()["kind"] == "database_key");
    CHECK(outcome.challenges.front()["secret"] == true);
}

TEST_CASE("rejected app credentials are CAS-replaced before a fresh generation bootstraps",
          "[auth][login][bootstrap][replacement][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitTdlibParameters}, secret_hook::run,
                  true, "main", "111", "rejected-environment-hash");
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 5.0);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    REQUIRE(field(sent.at(1).function, "api_id") != nullptr);
    CHECK(std::get<std::int64_t>(*field(sent.at(1).function, "api_id")) == 111);
    auth.runtime().push_response(
        auth.first(), sent.at(1).query_id,
        core::TdValue::from(core::TdError{
            400, "Valid api_id must be provided. Can be obtained at https://my.telegram.org"}));

    REQUIRE(auth.runtime().wait_for_clients(2));
    REQUIRE(auth.runtime().wait_for_sent(4));
    const auto replacement_client = auth.runtime().clients().back();
    sent = auth.runtime().sent_functions();
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::Close);
    CHECK(sent.at(3).function.kind() == core::TdFunctionKind::GetAuthorizationState);
    auth.runtime().push_response(replacement_client, sent.at(3).query_id, {},
                                 core::AuthStateData{core::AuthState::WaitTdlibParameters});

    REQUIRE(auth.runtime().wait_for_sent(5));
    sent = auth.runtime().sent_functions();
    CHECK(sent.at(4).function.kind() == core::TdFunctionKind::SetTdlibParameters);
    const auto* api_id = field(sent.at(4).function, "api_id");
    REQUIRE(api_id != nullptr);
    CHECK(std::get<std::int64_t>(*api_id) == 54321);
    auth.runtime().push_response(replacement_client, sent.at(4).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(replacement_client, {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(auth.runtime().wait_for_sent(6));
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(replacement_client, sent.at(5).query_id,
                                 core::TdValue::from(ada()));

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE(outcome.challenges.size() == 2);
    CHECK(outcome.challenges.at(0)["kind"] == "api_id");
    CHECK(outcome.challenges.at(1)["kind"] == "api_hash");
    const auto loaded = config::Store(tree.config_path()).load();
    REQUIRE(loaded);
    REQUIRE(loaded.snapshot->accounts.at("main").api_id);
    REQUIRE(loaded.snapshot->accounts.at("main").api_hash);
    CHECK(*loaded.snapshot->accounts.at("main").api_id == 54321);
    CHECK(*loaded.snapshot->accounts.at("main").api_hash == "replacement-hash");
}

TEST_CASE("API_ID_INVALID always replaces the app tuple before retrying on a fresh generation",
          "[auth][login][replacement][dispatch]") {
    struct Case {
        const char* name;
        json args;
        core::TdFunctionKind function;
        std::size_t initial_challenges;
    };
    const std::vector<Case> cases{
        {"phone",
         {{"qr", false}, {"bot", false}},
         core::TdFunctionKind::SetAuthenticationPhoneNumber,
         1},
        {"qr",
         {{"qr", true}, {"bot", false}},
         core::TdFunctionKind::RequestQrCodeAuthentication,
         0},
        {"bot",
         {{"qr", false}, {"bot", true}},
         core::TdFunctionKind::CheckAuthenticationBotToken,
         1},
    };

    for (const auto& entry : cases) {
        DYNAMIC_SECTION(entry.name) {
            const AuthTree tree;
            FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
            auto pending = std::async(std::launch::async, [&] {
                return dispatch(auth.dispatcher(), {"login"}, entry.args, true, 5.0);
            });
            REQUIRE(auth.runtime().wait_for_sent(2));
            auto sent = auth.runtime().sent_functions();
            CHECK(sent.at(1).function.kind() == entry.function);
            auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                         core::TdValue::from(core::TdError{400, "API_ID_INVALID"}));

            REQUIRE(auth.runtime().wait_for_clients(2));
            REQUIRE(auth.runtime().wait_for_sent(4));
            const auto replacement_client = auth.runtime().clients().back();
            sent = auth.runtime().sent_functions();
            CHECK(sent.at(2).function.kind() == core::TdFunctionKind::Close);
            CHECK(sent.at(3).function.kind() == core::TdFunctionKind::GetAuthorizationState);
            CHECK(std::ranges::count_if(sent, [&](const auto& item) {
                      return item.client_generation == auth.first().client_generation &&
                             item.function.kind() == entry.function;
                  }) == 1);
            auth.runtime().push_response(replacement_client, sent.at(3).query_id, {},
                                         core::AuthStateData{core::AuthState::WaitTdlibParameters});

            REQUIRE(auth.runtime().wait_for_sent(5));
            sent = auth.runtime().sent_functions();
            CHECK(sent.at(4).function.kind() == core::TdFunctionKind::SetTdlibParameters);
            CHECK(std::get<std::int64_t>(*field(sent.at(4).function, "api_id")) == 54321);
            auth.runtime().push_response(replacement_client, sent.at(4).query_id,
                                         core::TdValue::from(core::TdOk{}));
            auth.runtime().push_update(replacement_client, {},
                                       core::AuthStateData{core::AuthState::Ready});
            REQUIRE(auth.runtime().wait_for_sent(6));
            sent = auth.runtime().sent_functions();
            auth.runtime().push_response(replacement_client, sent.at(5).query_id,
                                         core::TdValue::from(ada()));

            const auto outcome = pending.get();
            REQUIRE(outcome.result);
            REQUIRE(outcome.challenges.size() == entry.initial_challenges + 2);
            CHECK(outcome.challenges.at(entry.initial_challenges)["kind"] == "api_id");
            CHECK(outcome.challenges.at(entry.initial_challenges + 1)["kind"] == "api_hash");
        }
    }
}

TEST_CASE("prompted app credentials for an explicit account persist before parameters are sent",
          "[auth][login][bootstrap][persistence][dispatch]") {
    const AuthTree tree;
    tree.write_config("default_account = \"main\"\n\n[accounts.main]\nallow_write = false\n");
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitTdlibParameters});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 5.0);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::SetTdlibParameters);
    const auto persisted = config::Store(tree.config_path()).load();
    REQUIRE(persisted);
    REQUIRE(persisted.snapshot->accounts.at("main").api_id);
    REQUIRE(persisted.snapshot->accounts.at("main").api_hash);
    CHECK(*persisted.snapshot->accounts.at("main").api_id == 54321);
    CHECK(*persisted.snapshot->accounts.at("main").api_hash == "replacement-hash");

    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(2).query_id, core::TdValue::from(ada()));
    CHECK(pending.get().result.has_value());
}

TEST_CASE("app replacement CAS remains bound to the pre-challenge config identity",
          "[auth][login][replacement][conflict][dispatch]") {
    const AuthTree tree;
    const config::Store store(tree.config_path());
    const auto admitted = store.load();
    REQUIRE(admitted);
    const auto expected_identity = admitted.snapshot->identity;
    std::optional<std::string> concurrent_identity;
    bool mutation_applied = false;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 5.0,
                        [&](const json& challenge) -> std::optional<json> {
                            if (challenge.at("kind") == "api_hash") {
                                const auto before = store.load();
                                if (before) {
                                    const auto mutation = store.replace_app_credentials(
                                        before.snapshot->identity, "main",
                                        {.api_id = 77777, .api_hash = "concurrent-hash"});
                                    mutation_applied =
                                        mutation.status == config::MutationStatus::Applied;
                                    if (mutation.snapshot) {
                                        concurrent_identity = mutation.snapshot->identity;
                                    }
                                }
                            }
                            return answer_for(challenge);
                        });
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    const auto sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdError{400, "API_ID_INVALID"}));

    const auto outcome = pending.get();
    CHECK(mutation_applied);
    REQUIRE(concurrent_identity);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "CONFIG_CONFLICT");
    CHECK((*outcome.error)["error"]["details"] == json{{"path", tree.config_path()},
                                                       {"expected", expected_identity},
                                                       {"current", *concurrent_identity}});
    CHECK(auth.runtime().sent_functions().size() == 2);
    const auto current = store.load();
    REQUIRE(current);
    CHECK(current.snapshot->accounts.at("main").api_id == 77777);
    CHECK(current.snapshot->accounts.at("main").api_hash == "concurrent-hash");
}

TEST_CASE("app replacement CAS remains bound to the snapshot admitted before parameters",
          "[auth][login][replacement][conflict][admission][dispatch]") {
    const AuthTree tree;
    const config::Store store(tree.config_path());
    const auto admitted = store.load();
    REQUIRE(admitted);
    const auto admitted_identity = admitted.snapshot->identity;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitTdlibParameters});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 5.0);
    });

    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    REQUIRE(sent.at(1).function.kind() == core::TdFunctionKind::SetTdlibParameters);
    REQUIRE(field(sent.at(1).function, "api_id") != nullptr);
    CHECK(std::get<std::int64_t>(*field(sent.at(1).function, "api_id")) == 12345);

    const auto concurrent = store.replace_app_credentials(
        admitted_identity, "main", {.api_id = 77777, .api_hash = "concurrent-hash"});
    REQUIRE(concurrent.status == config::MutationStatus::Applied);
    REQUIRE(concurrent.snapshot);

    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(auth.first(), {},
                               core::AuthStateData{core::AuthState::WaitPhoneNumber});
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    REQUIRE(sent.at(2).function.kind() == core::TdFunctionKind::SetAuthenticationPhoneNumber);
    auth.runtime().push_response(auth.first(), sent.at(2).query_id,
                                 core::TdValue::from(core::TdError{400, "API_ID_INVALID"}));

    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "CONFIG_CONFLICT");
    CHECK((*outcome.error)["error"]["details"] == json{{"path", tree.config_path()},
                                                       {"expected", admitted_identity},
                                                       {"current", concurrent.snapshot->identity}});
    REQUIRE(outcome.challenges.size() == 3);
    CHECK(outcome.challenges.at(0)["kind"] == "phone_number");
    CHECK(outcome.challenges.at(1)["kind"] == "api_id");
    CHECK(outcome.challenges.at(2)["kind"] == "api_hash");
    CHECK(auth.runtime().sent_functions().size() == 3);
    CHECK(auth.runtime().clients().size() == 1);

    const auto current = store.load();
    REQUIRE(current);
    CHECK(current.snapshot->identity == concurrent.snapshot->identity);
    CHECK(current.snapshot->accounts.at("main").api_id == 77777);
    CHECK(current.snapshot->accounts.at("main").api_hash == "concurrent-hash");
}

TEST_CASE("a locally invalid api_id is challenged again without fabricating a TDLib error",
          "[auth][login][bootstrap][validation][dispatch]") {
    const AuthTree tree;
    tree.write_config("default_account = \"main\"\n\n[accounts.main]\nallow_write = false\n");
    std::size_t api_id_answers = 0;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitTdlibParameters});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 5.0,
                        [&](const json& challenge) -> std::optional<json> {
                            auto answer = answer_for(challenge);
                            if (challenge.at("kind") == "api_id" && api_id_answers++ == 0) {
                                answer["value"] = "not-an-api-id";
                            }
                            return answer;
                        });
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(2).query_id, core::TdValue::from(ada()));

    const auto outcome = pending.get();
    REQUIRE(outcome.result);
    REQUIRE(outcome.challenges.size() == 3);
    CHECK(outcome.challenges.at(0)["kind"] == "api_id");
    CHECK(outcome.challenges.at(1)["kind"] == "api_id");
    CHECK(outcome.challenges.at(2)["kind"] == "api_hash");
    CHECK(outcome.progress.empty());
}

TEST_CASE("login fails once when authorization is lost while getMe is pending",
          "[auth][login][lifecycle][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::Ready});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}});
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auth.runtime().push_update(auth.first(), {},
                               core::AuthStateData{core::AuthState::WaitPhoneNumber});

    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
    CHECK(outcome.exit_code == kNotAuthed);
    CHECK(*outcome.error ==
          json{{"error",
                {{"code", "NOT_AUTHED"},
                 {"message", "authorization was lost before login identity completed"},
                 {"details",
                  {{"account", "main"},
                   {"state", "wait_phone_number"},
                   {"reason", "authorization_lost"}}}}}});
}

TEST_CASE("Ready getMe preserves receive order across repeated Ready updates",
          "[auth][login][ordering][dispatch]") {
    for (const std::string_view command : {"login", "me"}) {
        for (const bool response_first : {true, false}) {
            DYNAMIC_SECTION(std::string(command) + (response_first
                                                        ? " response before Ready"
                                                        : " repeated Ready before response")) {
                const AuthTree tree;
                FakeAuth auth(tree, core::AuthStateData{core::AuthState::Ready});
                auto pending = std::async(std::launch::async, [&] {
                    return dispatch(auth.dispatcher(), {std::string(command)},
                                    command == "login" ? json{{"qr", false}, {"bot", false}}
                                                       : json::object());
                });
                REQUIRE(auth.runtime().wait_for_sent(2));
                const auto sent = auth.runtime().sent_functions();
                REQUIRE(sent.size() == 2);
                CHECK(sent.at(1).function.kind() == core::TdFunctionKind::GetMe);

                auth.runtime().set_receive_paused(true);
                if (response_first) {
                    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                                 core::TdValue::from(ada()));
                    auth.runtime().push_update(auth.first(), {},
                                               core::AuthStateData{core::AuthState::Ready});
                } else {
                    auth.runtime().push_update(auth.first(), {},
                                               core::AuthStateData{core::AuthState::Ready});
                    auth.runtime().push_update(auth.first(), {},
                                               core::AuthStateData{core::AuthState::Ready});
                    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                                 core::TdValue::from(ada()));
                }
                auth.runtime().set_receive_paused(false);

                const auto outcome = pending.get();
                REQUIRE(outcome.result);
                CHECK(outcome.error == std::nullopt);
                CHECK(outcome.exit_code == kOk);
                CHECK(outcome.terminal_count == 1);
                const auto final_sent = auth.runtime().sent_functions();
                CHECK(std::ranges::count_if(final_sent, [](const auto& item) {
                          return item.function.kind() == core::TdFunctionKind::GetMe;
                      }) == 1);
            }
        }
    }
}

TEST_CASE("Ready getMe reports the first later non-ready state exactly once",
          "[auth][login][ordering][authorization][dispatch]") {
    for (const std::string_view command : {"login", "me"}) {
        DYNAMIC_SECTION(command) {
            const AuthTree tree;
            FakeAuth auth(tree, core::AuthStateData{core::AuthState::Ready});
            auto pending = std::async(std::launch::async, [&] {
                return dispatch(auth.dispatcher(), {std::string(command)},
                                command == "login" ? json{{"qr", false}, {"bot", false}}
                                                   : json::object());
            });
            REQUIRE(auth.runtime().wait_for_sent(2));
            const auto sent = auth.runtime().sent_functions();

            auth.runtime().set_receive_paused(true);
            auth.runtime().push_update(auth.first(), {},
                                       core::AuthStateData{core::AuthState::Ready});
            auth.runtime().push_update(auth.first(), {},
                                       core::AuthStateData{core::AuthState::Ready});
            auth.runtime().push_update(auth.first(), {},
                                       core::AuthStateData{core::AuthState::WaitPhoneNumber});
            auth.runtime().push_update(auth.first(), {},
                                       core::AuthStateData{core::AuthState::WaitPassword});
            auth.runtime().push_update(auth.first(), {},
                                       core::AuthStateData{core::AuthState::Ready});
            auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                         core::TdValue::from(ada()));
            auth.runtime().set_receive_paused(false);
            REQUIRE(auth.wait_state_sequence(6));

            const auto outcome = pending.get();
            REQUIRE(outcome.error);
            CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
            CHECK((*outcome.error)["error"]["details"] == json{{"account", "main"},
                                                               {"state", "wait_phone_number"},
                                                               {"reason", "authorization_lost"}});
            CHECK(outcome.exit_code == kNotAuthed);
            CHECK(outcome.terminal_count == 1);
            CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
        }
    }
}

TEST_CASE("Ready getMe resolves stale admission failures against authoritative updates",
          "[auth][login][ordering][authorization][dispatch]") {
    for (const bool authorization_lost : {false, true}) {
        DYNAMIC_SECTION(std::string(authorization_lost ? "first non-ready update wins"
                                                       : "newer Ready snapshot is retried")) {
            const AuthTree tree;
            tree.install_transaction_marker();
            auto hooks = std::make_shared<config::testing::StoreHooks>();
            std::mutex barrier_mutex;
            std::condition_variable barrier_cv;
            bool read_blocked = false;
            bool release_read = false;
            hooks->at_read_stage = [&](config::testing::ReadStage) {
                std::unique_lock lock(barrier_mutex);
                read_blocked = true;
                barrier_cv.notify_all();
                static_cast<void>(barrier_cv.wait_for(lock, 2s, [&] { return release_read; }));
            };
            FakeAuth auth(tree, core::AuthStateData{core::AuthState::Ready}, secret_hook::run, true,
                          "main", {}, {}, hooks);
            auto pending = std::async(std::launch::async, [&] {
                return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true,
                                5.0);
            });
            {
                std::unique_lock lock(barrier_mutex);
                REQUIRE(barrier_cv.wait_for(lock, 2s, [&] { return read_blocked; }));
            }

            auth.runtime().push_update(auth.first(), {},
                                       core::AuthStateData{core::AuthState::Ready});
            if (authorization_lost) {
                auth.runtime().push_update(auth.first(), {},
                                           core::AuthStateData{core::AuthState::WaitPhoneNumber});
                auth.runtime().push_update(auth.first(), {},
                                           core::AuthStateData{core::AuthState::Ready});
            }
            REQUIRE(auth.wait_state_sequence(authorization_lost ? 4 : 2));
            tree.clear_transaction_marker();
            {
                const std::lock_guard lock(barrier_mutex);
                release_read = true;
            }
            barrier_cv.notify_all();

            if (authorization_lost) {
                const auto outcome = pending.get();
                REQUIRE(outcome.error);
                CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
                CHECK((*outcome.error)["error"]["details"] ==
                      json{{"account", "main"},
                           {"state", "wait_phone_number"},
                           {"reason", "authorization_lost"}});
                CHECK(outcome.exit_code == kNotAuthed);
                CHECK(outcome.terminal_count == 1);
                CHECK(auth.runtime().sent_functions().size() == 1);
            } else {
                REQUIRE(auth.runtime().wait_for_sent(2));
                const auto sent = auth.runtime().sent_functions();
                REQUIRE(sent.size() == 2);
                CHECK(sent.at(1).function.kind() == core::TdFunctionKind::GetMe);
                auth.runtime().push_response(auth.first(), 1, core::TdValue::from(ada()));
                CHECK(pending.wait_for(20ms) == std::future_status::timeout);
                auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                             core::TdValue::from(ada()));
                const auto outcome = pending.get();
                REQUIRE(outcome.result);
                CHECK(outcome.error == std::nullopt);
                CHECK(outcome.exit_code == kOk);
                CHECK(outcome.terminal_count == 1);
            }
        }
    }
}

TEST_CASE("Ready update flood remains bounded by the original getMe deadline",
          "[auth][login][timeout][ordering][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::Ready});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"me"}, json::object(), true, 0.05);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    for (std::size_t index = 0; index < 64; ++index) {
        auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    }

    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    CHECK((*outcome.error)["error"]["details"] == json{{"operation", "me"}, {"state", "ready"}});
    CHECK(outcome.exit_code == kTimeout);
    CHECK(outcome.terminal_count == 1);
}

TEST_CASE("bot login uses its hook once and exposes only a redacted descriptor",
          "[auth][login][bot][hook][redaction]") {
    const AuthTree tree;
    tree.write_config("default_account = \"main\"\n\n[accounts.main]\napi_id = 12345\n"
                      "api_hash = \"hash-value\"\nbot_token_cmd = \"ignored\"\n");
    int hook_calls = 0;
    secret_hook::HookField observed_field = secret_hook::HookField::ApiId;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber},
                  [&hook_calls, &observed_field](const secret_hook::HookRequest& request) {
                      ++hook_calls;
                      observed_field = request.field;
                      return secret_hook::HookResult{"sensitive-bot-token", {}};
                  });
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", true}}, false);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    const auto sent = auth.runtime().sent_functions();
    REQUIRE(sent.size() == 2);
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::CheckAuthenticationBotToken);
    const auto* credential = field(sent.at(1).function, "credential");
    REQUIRE(credential != nullptr);
    CHECK(std::get<core::TdRedactedValue>(*credential) == core::TdRedactedValue::Credential);
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdError{400, "ACCESS_TOKEN_INVALID"}));

    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
    CHECK(outcome.exit_code == kNotAuthed);
    CHECK((*outcome.error)["error"]["code"] == "AUTH_CREDENTIAL_REJECTED");
    CHECK((*outcome.error)["error"]["details"] == json{{"account", "main"},
                                                       {"state", "wait_phone_number"},
                                                       {"credential", "bot_token"},
                                                       {"tdlib_code", 400}});
    CHECK(hook_calls == 1);
    CHECK(observed_field == secret_hook::HookField::BotToken);
    CHECK(outcome.challenges.empty());
    CHECK(outcome.error->dump().find("sensitive-bot-token") == std::string::npos);
}

TEST_CASE("password hook command comes from the immutable login admission snapshot",
          "[auth][login][hook][admission][dispatch]") {
    const AuthTree tree;
    tree.write_config("default_account = \"main\"\n\n[accounts.main]\napi_id = 12345\n"
                      "api_hash = \"hash-value\"\npassword_cmd = \"command-a\"\n");
    std::optional<std::string> observed_command;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber},
                  [&](const secret_hook::HookRequest& request) {
                      observed_command = request.command;
                      return secret_hook::HookResult{"password-secret", {}};
                  });
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", true}, {"bot", false}}, false, 5.0);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    tree.write_config("default_account = \"main\"\n\n[accounts.main]\napi_id = 12345\n"
                      "api_hash = \"hash-value\"\npassword_cmd = \"command-b\"\n");
    auto sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(
        auth.first(), {},
        core::AuthStateData{core::AuthState::WaitPassword,
                            core::AuthWaitPassword{.hint = "hint",
                                                   .has_recovery_email_address = false,
                                                   .has_passport_data = false,
                                                   .recovery_email_address_pattern = ""}});
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::CheckAuthenticationPassword);
    CHECK(observed_command == "command-a");
    auth.runtime().push_response(auth.first(), sent.at(2).query_id,
                                 core::TdValue::from(core::TdError{500, "SERVER_ERROR"}));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "TDLIB_ERROR");
}

TEST_CASE("challenge errors report the routed coordinator account",
          "[auth][login][account][dispatch]") {
    const AuthTree tree;
    tree.write_config("default_account = \"work\"\n\n[accounts.work]\napi_id = 12345\n"
                      "api_hash = \"hash-value\"\nallow_write = false\n");
    const FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber},
                        secret_hook::run, true, "work");
    const auto outcome =
        dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, false);
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "AUTH_INPUT_REQUIRED");
    CHECK((*outcome.error)["error"]["details"]["account"] == "work");
}

TEST_CASE("non-TTY bot login without a hook fails before sending a token",
          "[auth][login][bot][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    const auto outcome =
        dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", true}}, false);

    REQUIRE(outcome.error);
    CHECK(outcome.exit_code == kNotAuthed);
    CHECK(*outcome.error == json{{"error",
                                  {{"code", "AUTH_INPUT_REQUIRED"},
                                   {"message", "authentication input requires a TTY"},
                                   {"details",
                                    {{"account", "main"},
                                     {"state", "wait_phone_number"},
                                     {"challenge", "bot_token"}}}}}});
    CHECK(auth.runtime().sent_functions().size() == 1);
    CHECK(outcome.challenges.empty());
}

TEST_CASE("premium and unsupported authorization states fail closed without a send",
          "[auth][login][dispatch]") {
    const AuthTree tree;
    SECTION("premium purchase") {
        FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPremiumPurchase,
                                                core::AuthWaitPremiumPurchase{
                                                    .store_product_id = "premium.30",
                                                    .premium_day_count = 30,
                                                    .support_email_address = "support@example.test",
                                                    .support_email_subject = "Premium"}});
        const auto outcome = dispatch(auth.dispatcher(), {"login"});
        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
        CHECK(outcome.exit_code == kNotAuthed);
        CHECK((*outcome.error)["error"]["code"] == "AUTH_PREMIUM_REQUIRED");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"account", "main"},
                   {"state", "wait_premium_purchase"},
                   {"store_product_id", "premium.30"},
                   {"premium_day_count", 30},
                   {"support_email_address", "support@example.test"},
                   {"support_email_subject", "Premium"}});
        CHECK(auth.runtime().sent_functions().size() == 1);
    }

    SECTION("unknown top-level TDLib variant") {
        FakeAuth auth(tree, core::AuthStateData{core::AuthState::Unknown, {}, 987654});
        const auto outcome = dispatch(auth.dispatcher(), {"login"});
        REQUIRE(outcome.error);
        CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
        CHECK(outcome.exit_code == kGeneric);
        CHECK((*outcome.error)["error"]["code"] == "UNSUPPORTED_AUTH_STATE");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"account", "main"}, {"tdlib_type_id", 987654}});
        CHECK(auth.runtime().sent_functions().size() == 1);
    }
}

TEST_CASE("declining registration terms cancels login before names or TDLib send",
          "[auth][login][registration][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitRegistration,
                                            core::AuthWaitRegistration{.terms_text = "terms",
                                                                       .minimum_user_age = 16,
                                                                       .show_popup = true}});
    const auto outcome = dispatch(auth.dispatcher(), {"login"}, json::object(), true, 2.0,
                                  [](const json& challenge) {
                                      auto answer = answer_for(challenge);
                                      answer["value"] = false;
                                      return std::optional<json>{std::move(answer)};
                                  });

    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
    CHECK(outcome.exit_code == kNotAuthed);
    CHECK((*outcome.error)["error"]["code"] == "AUTH_CANCELLED");
    CHECK((*outcome.error)["error"]["details"] == json{{"account", "main"},
                                                       {"state", "wait_registration"},
                                                       {"challenge", "registration_terms"}});
    REQUIRE(outcome.challenges.size() == 1);
    CHECK(outcome.challenges.front()["kind"] == "registration_terms");
    CHECK(auth.runtime().sent_functions().size() == 1);
}

TEST_CASE("same-state update between api_id and api_hash restarts the credential exchange",
          "[auth][login][challenge][supersession][dispatch]") {
    const AuthTree tree;
    tree.write_config("default_account = \"main\"\n\n[accounts.main]\nallow_write = false\n");
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitTdlibParameters});
    Outcome outcome;
    auto sink = callback_sink(outcome);
    proto::Request request("main");
    request.id = 90;
    request.command = {"login"};
    request.args = {{"qr", false}, {"bot", false}};
    request.context.tty = true;
    request.context.timeout_seconds = 5.0;
    daemon::RequestSession session(request, *sink);
    bool injected = false;
    bool update_observed = false;
    session.set_challenge_return_hook([&](daemon::ChallengeStatus status) {
        if (status != daemon::ChallengeStatus::Answered || injected) {
            return;
        }
        injected = true;
        auth.runtime().push_update(auth.first(), {},
                                   core::AuthStateData{core::AuthState::WaitTdlibParameters});
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auth.client().auth_state()->auth_sequence >= 2) {
                update_observed = true;
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    auto pending = std::async(std::launch::async, [&] { auth.dispatcher().dispatch(session); });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
    REQUIRE(auth.runtime().wait_for_sent(3));
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(auth.first(), sent.at(2).query_id, core::TdValue::from(ada()));
    pending.get();

    CHECK(update_observed);
    REQUIRE(outcome.result);
    REQUIRE(outcome.challenges.size() == 3);
    CHECK(outcome.challenges.at(0)["kind"] == "api_id");
    CHECK(outcome.challenges.at(0)["auth_sequence"] == 1);
    CHECK(outcome.challenges.at(1)["kind"] == "api_id");
    CHECK(outcome.challenges.at(1)["auth_sequence"] == 2);
    CHECK(outcome.challenges.at(2)["kind"] == "api_hash");
    CHECK(outcome.challenges.at(2)["auth_sequence"] == 2);
}

TEST_CASE("same-state update between terms and names restarts registration confirmation",
          "[auth][login][registration][challenge][supersession][dispatch]") {
    const AuthTree tree;
    const core::AuthStateData registration{core::AuthState::WaitRegistration,
                                           core::AuthWaitRegistration{.terms_text = "terms",
                                                                      .minimum_user_age = 16,
                                                                      .show_popup = true}};
    FakeAuth auth(tree, registration);
    Outcome outcome;
    auto sink = callback_sink(outcome);
    proto::Request request("main");
    request.id = 91;
    request.command = {"login"};
    request.args = {{"qr", false}, {"bot", false}};
    request.context.tty = true;
    request.context.timeout_seconds = 5.0;
    daemon::RequestSession session(request, *sink);
    bool injected = false;
    bool update_observed = false;
    session.set_challenge_return_hook([&](daemon::ChallengeStatus status) {
        if (status != daemon::ChallengeStatus::Answered || injected) {
            return;
        }
        injected = true;
        auth.runtime().push_update(auth.first(), {}, registration);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auth.client().auth_state()->auth_sequence >= 2) {
                update_observed = true;
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    auto pending = std::async(std::launch::async, [&] { auth.dispatcher().dispatch(session); });
    REQUIRE(auth.runtime().wait_for_sent(2));
    const auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::RegisterUser);
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdError{500, "SERVER_ERROR"}));
    pending.get();

    CHECK(update_observed);
    REQUIRE(outcome.error);
    REQUIRE(outcome.challenges.size() == 4);
    CHECK(outcome.challenges.at(0)["kind"] == "registration_terms");
    CHECK(outcome.challenges.at(0)["auth_sequence"] == 1);
    CHECK(outcome.challenges.at(1)["kind"] == "registration_terms");
    CHECK(outcome.challenges.at(1)["auth_sequence"] == 2);
    CHECK(outcome.challenges.at(2)["kind"] == "registration_first_name");
    CHECK(outcome.challenges.at(3)["kind"] == "registration_last_name");
}

TEST_CASE("auth response and update precedence follows queued TD receive order",
          "[auth][login][ordering][dispatch]") {
    constexpr std::string_view config =
        "default_account = \"main\"\n\n[accounts.main]\napi_id = 12345\n"
        "api_hash = \"hash-value\"\nbot_token_cmd = \"token-a\"\n";

    SECTION("response before update preserves the credential rejection") {
        const AuthTree tree;
        tree.write_config(config);
        FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber},
                      [](const secret_hook::HookRequest&) {
                          return secret_hook::HookResult{"bot-token", {}};
                      });
        auto pending = std::async(std::launch::async, [&] {
            return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", true}}, false,
                            5.0);
        });
        REQUIRE(auth.runtime().wait_for_sent(2));
        const auto sent = auth.runtime().sent_functions();
        auth.runtime().set_receive_paused(true);
        auth.runtime().push_response(
            auth.first(), sent.at(1).query_id,
            core::TdValue::from(core::TdError{400, "ACCESS_TOKEN_INVALID"}));
        auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
        auth.runtime().set_receive_paused(false);
        const auto outcome = pending.get();
        REQUIRE(outcome.error);
        CHECK((*outcome.error)["error"]["code"] == "AUTH_CREDENTIAL_REJECTED");
    }

    SECTION("update before response advances to the newer authorization state") {
        const AuthTree tree;
        tree.write_config(config);
        FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber},
                      [](const secret_hook::HookRequest&) {
                          return secret_hook::HookResult{"bot-token", {}};
                      });
        auto pending = std::async(std::launch::async, [&] {
            return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", true}}, false,
                            5.0);
        });
        REQUIRE(auth.runtime().wait_for_sent(2));
        auto sent = auth.runtime().sent_functions();
        auth.runtime().set_receive_paused(true);
        auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Ready});
        auth.runtime().push_response(
            auth.first(), sent.at(1).query_id,
            core::TdValue::from(core::TdError{400, "ACCESS_TOKEN_INVALID"}));
        auth.runtime().set_receive_paused(false);
        REQUIRE(auth.runtime().wait_for_sent(3));
        sent = auth.runtime().sent_functions();
        CHECK(sent.at(2).function.kind() == core::TdFunctionKind::GetMe);
        auth.runtime().push_response(auth.first(), sent.at(2).query_id, core::TdValue::from(ada()));
        const auto outcome = pending.get();
        REQUIRE(outcome.result);
        CHECK(outcome.error == std::nullopt);
    }

    SECTION("an authoritative update also wins over a typed local authorization failure") {
        for (const bool advance_after_delayed_delivery : {false, true}) {
            DYNAMIC_SECTION((advance_after_delayed_delivery
                                 ? "a delayed duplicate does not hide the next transition"
                                 : "the directly adopted update reaches getMe successfully")) {
                const AuthTree tree;
                FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
                std::mutex barrier_mutex;
                std::condition_variable barrier_cv;
                bool challenge_hook_entered = false;
                bool allow_update = false;
                bool subscriber_entered = false;
                bool release_subscriber = false;
                std::uint64_t delivered_sequence = 0;
                const auto blocking_subscription = auth.client().subscribe_auth_states(
                    [&](const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
                        if (!snapshot || snapshot->auth_sequence < 2) {
                            return;
                        }
                        std::unique_lock lock(barrier_mutex);
                        subscriber_entered = true;
                        barrier_cv.notify_all();
                        static_cast<void>(
                            barrier_cv.wait_for(lock, 2s, [&] { return release_subscriber; }));
                    });
                Outcome outcome;
                auto sink = callback_sink(outcome);
                proto::Request request("main");
                request.id = 92;
                request.command = {"login"};
                request.args = {{"qr", false}, {"bot", false}};
                request.context.tty = true;
                request.context.timeout_seconds = 2.0;
                daemon::RequestSession session(request, *sink);
                bool update_observed = false;
                session.set_challenge_return_hook([&](daemon::ChallengeStatus status) {
                    if (status != daemon::ChallengeStatus::Answered || update_observed) {
                        return;
                    }
                    {
                        std::unique_lock lock(barrier_mutex);
                        challenge_hook_entered = true;
                        barrier_cv.notify_all();
                        static_cast<void>(
                            barrier_cv.wait_for(lock, 2s, [&] { return allow_update; }));
                    }
                    auth.runtime().push_update(auth.first(), {},
                                               core::AuthStateData{core::AuthState::Ready});
                    std::unique_lock lock(barrier_mutex);
                    update_observed =
                        barrier_cv.wait_for(lock, 2s, [&] { return subscriber_entered; });
                });
                auto pending =
                    std::async(std::launch::async, [&] { auth.dispatcher().dispatch(session); });
                bool hook_observed = false;
                {
                    std::unique_lock lock(barrier_mutex);
                    hook_observed =
                        barrier_cv.wait_for(lock, 2s, [&] { return challenge_hook_entered; });
                }
                const auto trailing_subscription = auth.client().subscribe_auth_states(
                    [&](const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
                        if (!snapshot) {
                            return;
                        }
                        const std::lock_guard lock(barrier_mutex);
                        delivered_sequence = std::max(delivered_sequence, snapshot->auth_sequence);
                        barrier_cv.notify_all();
                    });
                {
                    const std::lock_guard lock(barrier_mutex);
                    allow_update = true;
                }
                barrier_cv.notify_all();
                const bool first_get_me_sent = auth.runtime().wait_for_sent(2);
                {
                    const std::lock_guard lock(barrier_mutex);
                    release_subscriber = true;
                }
                barrier_cv.notify_all();
                bool delayed_delivery_completed = false;
                {
                    std::unique_lock lock(barrier_mutex);
                    delayed_delivery_completed =
                        barrier_cv.wait_for(lock, 2s, [&] { return delivered_sequence >= 2; });
                }

                bool later_update_delivered = false;
                const auto sent = auth.runtime().sent_functions();
                if (first_get_me_sent && delayed_delivery_completed &&
                    advance_after_delayed_delivery) {
                    auth.runtime().push_update(
                        auth.first(), {}, core::AuthStateData{core::AuthState::WaitPhoneNumber});
                    {
                        std::unique_lock lock(barrier_mutex);
                        later_update_delivered =
                            barrier_cv.wait_for(lock, 2s, [&] { return delivered_sequence >= 3; });
                    }
                } else if (first_get_me_sent) {
                    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                                 core::TdValue::from(ada()));
                }

                pending.get();
                auth.client().unsubscribe_auth_states(trailing_subscription);
                auth.client().unsubscribe_auth_states(blocking_subscription);
                CHECK(hook_observed);
                CHECK(update_observed);
                CHECK(first_get_me_sent);
                CHECK(delayed_delivery_completed);
                CHECK(later_update_delivered == advance_after_delayed_delivery);
                REQUIRE(sent.size() == 2);
                CHECK(sent.at(1).function.kind() == core::TdFunctionKind::GetMe);
                if (advance_after_delayed_delivery) {
                    REQUIRE(outcome.error);
                    CHECK((*outcome.error)["error"]["code"] == "NOT_AUTHED");
                    CHECK((*outcome.error)["error"]["details"] ==
                          json{{"account", "main"},
                               {"state", "wait_phone_number"},
                               {"reason", "authorization_lost"}});
                } else {
                    REQUIRE(outcome.result);
                    CHECK(outcome.error == std::nullopt);
                }
                CHECK(outcome.terminal_count == 1);
            }
        }
    }
}

TEST_CASE("an in-flight login timeout replaces the generation before releasing its lease",
          "[auth][login][timeout][lifecycle][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 0.05);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    REQUIRE(auth.runtime().wait_for_clients(2));

    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK_THAT(*outcome.error, test::matches_json_schema("auth.error.schema.json"));
    CHECK(outcome.exit_code == kTimeout);
    CHECK(outcome.terminal_count == 1);
    CHECK(*outcome.error ==
          json{{"error",
                {{"code", "TIMEOUT"},
                 {"message", "authentication deadline elapsed"},
                 {"details", {{"operation", "login"}, {"state", "wait_phone_number"}}}}}});

    const auto sent = auth.runtime().sent_functions();
    REQUIRE(sent.size() >= 4);
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::SetAuthenticationPhoneNumber);
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::Close);
    CHECK(sent.at(3).function.kind() == core::TdFunctionKind::GetAuthorizationState);
    CHECK(auth.client().auth_state()->client_generation != auth.first().client_generation);
}

TEST_CASE("a disconnected login orphan emits nothing and releases only after settling",
          "[auth][login][disconnect][lifecycle][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    Outcome outcome;
    auto sink = callback_sink(outcome);
    proto::Request request("main");
    request.id = 88;
    request.command = {"login"};
    request.args = {{"qr", false}, {"bot", false}};
    request.context.tty = true;
    request.context.timeout_seconds = 2.0;
    daemon::RequestSession session(request, *sink);
    auto pending = std::async(std::launch::async, [&] { auth.dispatcher().dispatch(session); });
    REQUIRE(auth.runtime().wait_for_sent(2));
    const auto sent = auth.runtime().sent_functions();
    session.disconnect();
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdError{406, "PHONE_NUMBER_INVALID"}));
    pending.get();

    CHECK(outcome.terminal_count == 0);
    CHECK(outcome.result == std::nullopt);
    CHECK(outcome.error == std::nullopt);
    CHECK(outcome.progress.empty());
    REQUIRE(outcome.challenges.size() == 1);

    const auto resumed =
        dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, false);
    REQUIRE(resumed.error);
    CHECK((*resumed.error)["error"]["code"] == "AUTH_INPUT_REQUIRED");
}

TEST_CASE("a disconnected orphan deadline replaces the generation without a terminal",
          "[auth][login][disconnect][timeout][lifecycle][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    Outcome outcome;
    auto sink = callback_sink(outcome);
    proto::Request request("main");
    request.id = 89;
    request.command = {"login"};
    request.args = {{"qr", false}, {"bot", false}};
    request.context.tty = true;
    request.context.timeout_seconds = 0.05;
    daemon::RequestSession session(request, *sink);
    auto pending = std::async(std::launch::async, [&] { auth.dispatcher().dispatch(session); });
    REQUIRE(auth.runtime().wait_for_sent(2));
    session.disconnect();
    REQUIRE(auth.runtime().wait_for_clients(2));
    pending.get();
    REQUIRE(auth.runtime().wait_for_sent(4));

    CHECK(outcome.terminal_count == 0);
    CHECK(outcome.result == std::nullopt);
    CHECK(outcome.error == std::nullopt);
    CHECK(outcome.progress.empty());
    REQUIRE(outcome.challenges.size() == 1);
    const auto sent = auth.runtime().sent_functions();
    REQUIRE(sent.size() >= 4);
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::Close);
    CHECK(sent.at(3).function.kind() == core::TdFunctionKind::GetAuthorizationState);
    CHECK(auth.client().auth_state()->client_generation != auth.first().client_generation);
}

TEST_CASE("timeout quarantines the login lease until Closed creates a replacement",
          "[auth][login][timeout][lifecycle][quarantine][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber}, secret_hook::run,
                  false);
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, true, 0.05);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    const auto timed_out = pending.get();
    REQUIRE(timed_out.error);
    CHECK((*timed_out.error)["error"]["code"] == "TIMEOUT");
    REQUIRE(auth.runtime().wait_for_sent(3));
    auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::Close);
    CHECK(auth.runtime().clients().size() == 1);

    const auto quarantined =
        dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, false, 0.5);
    REQUIRE(quarantined.error);
    CHECK((*quarantined.error)["error"]["code"] == "AUTH_FLOW_IN_PROGRESS");

    auth.runtime().push_update(auth.first(), {}, core::AuthStateData{core::AuthState::Closed});
    REQUIRE(auth.runtime().wait_for_clients(2));
    REQUIRE(auth.runtime().wait_for_sent(4));
    const auto replacement = auth.runtime().clients().back();
    sent = auth.runtime().sent_functions();
    auth.runtime().push_response(replacement, sent.at(3).query_id, {},
                                 core::AuthStateData{core::AuthState::WaitPhoneNumber});
    REQUIRE(auth.wait_state_sequence(1));

    std::optional<Outcome> resumed;
    const auto resume_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < resume_deadline) {
        auto candidate =
            dispatch(auth.dispatcher(), {"login"}, {{"qr", false}, {"bot", false}}, false, 0.5);
        if (candidate.error && (*candidate.error)["error"]["code"] != "AUTH_FLOW_IN_PROGRESS") {
            resumed = std::move(candidate);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(resumed);
    REQUIRE(resumed->error);
    CHECK((*resumed->error)["error"]["code"] == "AUTH_INPUT_REQUIRED");
    auth.runtime().set_close_automatically(true);
}

TEST_CASE("QR wait timeout restarts the generation after the request response settled",
          "[auth][login][qr][timeout][lifecycle][dispatch]") {
    const AuthTree tree;
    FakeAuth auth(tree, core::AuthStateData{core::AuthState::WaitPhoneNumber});
    auto pending = std::async(std::launch::async, [&] {
        return dispatch(auth.dispatcher(), {"login"}, {{"qr", true}, {"bot", false}}, true, 0.05);
    });
    REQUIRE(auth.runtime().wait_for_sent(2));
    auto sent = auth.runtime().sent_functions();
    CHECK(sent.at(1).function.kind() == core::TdFunctionKind::RequestQrCodeAuthentication);
    auth.runtime().push_response(auth.first(), sent.at(1).query_id,
                                 core::TdValue::from(core::TdOk{}));
    REQUIRE(auth.runtime().wait_for_clients(2));
    const auto outcome = pending.get();
    REQUIRE(outcome.error);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    sent = auth.runtime().sent_functions();
    REQUIRE(sent.size() >= 4);
    CHECK(sent.at(2).function.kind() == core::TdFunctionKind::Close);
    CHECK(sent.at(3).function.kind() == core::TdFunctionKind::GetAuthorizationState);
}
