#include "common/config.hpp"
#include "common/config_test_support.hpp"
#include "common/paths.hpp"
#include "core/auth_bootstrap.hpp"
#include "core/td_authorization.hpp"
#include "support/scripted_td_runtime.hpp"
#include "support/td_client_test_access.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using namespace tgcli;

namespace {

template <typename Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

class TempBootstrapRoot {
  public:
    TempBootstrapRoot() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-bootstrap-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        for (const auto& child : {"config", "data", "state", "runtime"}) {
            const auto directory = root_ / child;
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        }
    }

    ~TempBootstrapRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempBootstrapRoot(const TempBootstrapRoot&) = delete;
    TempBootstrapRoot& operator=(const TempBootstrapRoot&) = delete;
    TempBootstrapRoot(TempBootstrapRoot&&) = delete;
    TempBootstrapRoot& operator=(TempBootstrapRoot&&) = delete;

    [[nodiscard]] paths::Environment environment(bool test_dc = false) const {
        paths::Environment result;
        result.xdg_config_home = (root_ / "config").string();
        result.xdg_data_home = (root_ / "data").string();
        result.xdg_state_home = (root_ / "state").string();
        result.xdg_runtime_dir = (root_ / "runtime").string();
        result.home = root_.string();
        result.uid = getuid();
        result.test_dc = test_dc;
        return result;
    }

    static void write_config(const paths::Environment& environment, std::string_view bytes) {
        const auto filename = paths::config_file(environment);
        const auto directory = std::filesystem::path(filename).parent_path();
        REQUIRE(std::filesystem::create_directory(directory));
        REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        std::ofstream output(filename, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(filename.c_str(), 0600) == 0);
    }

    [[nodiscard]] const std::filesystem::path& root_path() const {
        return root_;
    }

  private:
    std::filesystem::path root_;
};

struct FakeBoundary {
    FakeBoundary(std::unique_ptr<core::TdClient> client_value,
                 test::ScriptedTdRuntime* runtime_value, test::ScriptedClient first_value)
        : client(std::move(client_value)), runtime(runtime_value), first(first_value) {}

    ~FakeBoundary() {
        if (!client) {
            return;
        }
        const auto snapshot = client->auth_state();
        if (snapshot && snapshot->auth_sequence == 0) {
            runtime->push_response(first, 1, {}, core::AuthStateData{core::AuthState::Ready});
            static_cast<void>(eventually([&] { return client->auth_state()->auth_sequence != 0; }));
        }
        client->close();
    }

    FakeBoundary(const FakeBoundary&) = delete;
    FakeBoundary& operator=(const FakeBoundary&) = delete;
    FakeBoundary(FakeBoundary&&) noexcept = default;
    FakeBoundary& operator=(FakeBoundary&&) noexcept = default;

    std::unique_ptr<core::TdClient> client;
    test::ScriptedTdRuntime* runtime;
    test::ScriptedClient first;
};

FakeBoundary make_fake_boundary(core::AuthState initial = core::AuthState::WaitTdlibParameters,
                                bool update_first = false) {
    auto runtime = std::make_unique<test::ScriptedTdRuntime>();
    auto* runtime_pointer = runtime.get();
    auto client = std::make_unique<core::TdClient>(std::move(runtime));
    REQUIRE(runtime_pointer->wait_for_sent(1));
    REQUIRE(runtime_pointer->clients().size() == 1);
    const auto first = runtime_pointer->clients().front();
    if (update_first) {
        runtime_pointer->push_update(first, {}, core::AuthStateData{initial});
    } else {
        runtime_pointer->push_response(first, 1, {}, core::AuthStateData{initial});
    }
    REQUIRE(eventually([&] { return client->auth_state()->auth_sequence == 1; }));
    return {std::move(client), runtime_pointer, first};
}

core::TdSendDescriptor descriptor_for(const core::AuthStateSnapshot& snapshot,
                                      core::TdFunctionKind function, core::DescriptorKind tier,
                                      core::TdRequestOwner owner) {
    return {.function = function,
            .tier = tier,
            .owner = std::move(owner),
            .client_generation = snapshot.client_generation,
            .auth_sequence = snapshot.auth_sequence,
            .auth_state = snapshot.data.state};
}

const core::TdFieldValue* field(const core::TdFunctionData& function, std::string_view name) {
    for (const auto& candidate : function.fields()) {
        if (candidate.has_name(name)) {
            return &candidate.value();
        }
    }
    return nullptr;
}

core::BootstrapSnapshot capture(const TempBootstrapRoot& root, const paths::Environment& env,
                                const config::Store& store, std::optional<std::string> api_id = {},
                                std::optional<std::string> api_hash = {}) {
    const auto loaded = store.load();
    REQUIRE(loaded);
    auto captured =
        core::capture_bootstrap_snapshot("main", loaded.snapshot, env, env.test_dc, "0.1.0-test",
                                         std::move(api_id), std::move(api_hash));
    REQUIRE(captured.snapshot);
    static_cast<void>(root);
    return std::move(*captured.snapshot);
}

core::BootstrapAttempt internal_attempt(core::TdClient& client,
                                        const core::AuthStateSnapshot& snapshot) {
    static_cast<void>(client);
    static_cast<void>(snapshot);
    return {.control = {}, .interactive = false, .prompted_app = {}, .prompted_database_key = {}};
}

secret_hook::HookResult hook_value(std::string value) {
    return {std::move(value), {}};
}

} // namespace

TEST_CASE("AuthBootstrap is a closed function state and owner allowlist",
          "[core][auth-bootstrap][safety]") {
    struct Allowed {
        core::TdFunctionKind function;
        core::AuthState state;
        core::TdOwnerKind owner;
        std::uint64_t sequence = 1;
    };
    const std::vector<Allowed> allowed{
        {core::TdFunctionKind::GetAuthorizationState, core::AuthState::Unknown,
         core::TdOwnerKind::InternalAuth, 0},
        {core::TdFunctionKind::SetTdlibParameters, core::AuthState::WaitTdlibParameters,
         core::TdOwnerKind::InternalAuth},
        {core::TdFunctionKind::SetAuthenticationPhoneNumber, core::AuthState::WaitPhoneNumber,
         core::TdOwnerKind::Login},
        {core::TdFunctionKind::RequestQrCodeAuthentication, core::AuthState::WaitPhoneNumber,
         core::TdOwnerKind::Login},
        {core::TdFunctionKind::CheckAuthenticationBotToken, core::AuthState::WaitPhoneNumber,
         core::TdOwnerKind::Login},
        {core::TdFunctionKind::SetAuthenticationEmailAddress, core::AuthState::WaitEmailAddress,
         core::TdOwnerKind::Login},
        {core::TdFunctionKind::CheckAuthenticationEmailCode, core::AuthState::WaitEmailCode,
         core::TdOwnerKind::Login},
        {core::TdFunctionKind::CheckAuthenticationCode, core::AuthState::WaitCode,
         core::TdOwnerKind::Login},
        {core::TdFunctionKind::RegisterUser, core::AuthState::WaitRegistration,
         core::TdOwnerKind::Login},
        {core::TdFunctionKind::CheckAuthenticationPassword, core::AuthState::WaitPassword,
         core::TdOwnerKind::Login},
    };
    for (const auto& entry : allowed) {
        INFO(core::td_function_name(entry.function));
        const core::AuthStateSnapshot snapshot{
            .client_id = 1001,
            .client_generation = 1,
            .auth_sequence = entry.sequence,
            .data = core::AuthStateData{entry.state},
        };
        const core::TdFunctionData function{entry.function};
        CHECK_FALSE(core::authorize_td_send(descriptor_for(snapshot, entry.function,
                                                           core::DescriptorKind::AuthBootstrap,
                                                           {entry.owner, 7}),
                                            &function, snapshot, false));
    }

    CHECK(allowed.size() == 10);
}

TEST_CASE("TD send descriptor mismatches fail before the fake boundary",
          "[core][auth-bootstrap][safety]") {
    auto fake = make_fake_boundary();
    const auto snapshot = fake.client->auth_state();
    const auto sent_before = fake.runtime->sent_functions().size();

    const auto send_mismatch = [&](core::TdSendDescriptor descriptor,
                                   core::TdFunctionKind function) {
        auto response =
            fake.client->send(std::move(descriptor),
                              core::TdValue::scripted_function(core::TdFunctionData{function}));
        REQUIRE(response.wait_for(0ms) == std::future_status::ready);
        CHECK_THROWS_AS(response.get(), core::TdAuthorizationError);
        CHECK(fake.runtime->sent_functions().size() == sent_before);
    };

    auto descriptor =
        descriptor_for(*snapshot, core::TdFunctionKind::SetTdlibParameters,
                       core::DescriptorKind::AuthBootstrap, {core::TdOwnerKind::InternalAuth, 1});
    send_mismatch(descriptor, core::TdFunctionKind::GetOption);
    descriptor.tier = core::DescriptorKind::Read;
    send_mismatch(descriptor, core::TdFunctionKind::SetTdlibParameters);
    descriptor.tier = core::DescriptorKind::AuthBootstrap;
    descriptor.owner = {core::TdOwnerKind::Request, 9};
    send_mismatch(descriptor, core::TdFunctionKind::SetTdlibParameters);
    descriptor.owner = {core::TdOwnerKind::InternalAuth, 999999};
    ++descriptor.client_generation;
    send_mismatch(descriptor, core::TdFunctionKind::SetTdlibParameters);
    descriptor.client_generation = snapshot->client_generation;
    ++descriptor.auth_sequence;
    send_mismatch(descriptor, core::TdFunctionKind::SetTdlibParameters);
    descriptor.auth_sequence = snapshot->auth_sequence;
    descriptor.auth_state = core::AuthState::Ready;
    send_mismatch(descriptor, core::TdFunctionKind::SetTdlibParameters);

    descriptor.auth_state = snapshot->data.state;
    descriptor.owner = {core::TdOwnerKind::InternalAuth, 999999};
    send_mismatch(descriptor, core::TdFunctionKind::SetTdlibParameters);

    auto logout =
        descriptor_for(*snapshot, core::TdFunctionKind::LogOut, core::DescriptorKind::Destructive,
                       {core::TdOwnerKind::Request, 9});
    send_mismatch(logout, core::TdFunctionKind::LogOut);
}

TEST_CASE("read functions require Ready in every authorization state",
          "[core][auth-bootstrap][safety]") {
    const std::vector<core::AuthState> states{
        core::AuthState::Unknown,
        core::AuthState::WaitTdlibParameters,
        core::AuthState::WaitPhoneNumber,
        core::AuthState::WaitPremiumPurchase,
        core::AuthState::WaitEmailAddress,
        core::AuthState::WaitEmailCode,
        core::AuthState::WaitCode,
        core::AuthState::WaitOtherDeviceConfirmation,
        core::AuthState::WaitRegistration,
        core::AuthState::WaitPassword,
        core::AuthState::Ready,
        core::AuthState::LoggingOut,
        core::AuthState::Closing,
        core::AuthState::Closed,
    };
    for (const auto state : states) {
        INFO(core::auth_state_name(state));
        const core::AuthStateSnapshot snapshot{
            .client_id = 1001,
            .client_generation = 1,
            .auth_sequence = 1,
            .data = core::AuthStateData{state},
        };
        for (const auto function :
             {core::TdFunctionKind::GetOption, core::TdFunctionKind::GetMe,
              core::TdFunctionKind::GetSavedMessagesTags, core::TdFunctionKind::SearchSavedMessages,
              core::TdFunctionKind::GetActiveSessions}) {
            const core::TdFunctionData function_data{function};
            const auto denied = core::authorize_td_send(
                descriptor_for(snapshot, function, core::DescriptorKind::Read,
                               {core::TdOwnerKind::Request, 1}),
                &function_data, snapshot, state == core::AuthState::Closed);
            CHECK(denied.has_value() == (state != core::AuthState::Ready));
        }
    }
}

TEST_CASE("dormant session termination policy is destructive Ready-only and request-owned",
          "[core][auth-bootstrap][safety][session]") {
    const core::AuthStateSnapshot ready{
        .client_id = 1001,
        .client_generation = 1,
        .auth_sequence = 1,
        .data = core::AuthStateData{core::AuthState::Ready},
    };
    const core::TdFunctionData function{core::TdFunctionKind::TerminateSession,
                                        {{"session_id", std::int64_t{0}}}};
    const auto descriptor =
        descriptor_for(ready, core::TdFunctionKind::TerminateSession,
                       core::DescriptorKind::Destructive, {core::TdOwnerKind::Request, 1});
    CHECK_FALSE(core::authorize_td_send(descriptor, &function, ready, false));

    auto wrong_tier = descriptor;
    wrong_tier.tier = core::DescriptorKind::Read;
    CHECK(core::authorize_td_send(wrong_tier, &function, ready, false) ==
          core::TdAuthorizationFailure::TierMismatch);

    auto wrong_owner = descriptor;
    wrong_owner.owner = {core::TdOwnerKind::Login, 1};
    CHECK(core::authorize_td_send(wrong_owner, &function, ready, false) ==
          core::TdAuthorizationFailure::OwnerMismatch);

    auto not_ready = ready;
    not_ready.data = core::AuthStateData{core::AuthState::WaitPhoneNumber};
    auto stale_state = descriptor;
    stale_state.auth_state = core::AuthState::WaitPhoneNumber;
    CHECK(core::authorize_td_send(stale_state, &function, not_ready, false) ==
          core::TdAuthorizationFailure::FunctionDenied);
}

TEST_CASE("TdClient read admission is an exact closed function allowlist",
          "[core][auth-bootstrap][safety][fake-boundary]") {
    auto fake = make_fake_boundary(core::AuthState::Ready);
    const auto snapshot = fake.client->auth_state();
    struct Entry {
        core::TdFunctionKind function;
        bool allowed;
    };
    const std::array entries{
        Entry{core::TdFunctionKind::GetAuthorizationState, false},
        Entry{core::TdFunctionKind::SetTdlibParameters, false},
        Entry{core::TdFunctionKind::SetAuthenticationPhoneNumber, false},
        Entry{core::TdFunctionKind::RequestQrCodeAuthentication, false},
        Entry{core::TdFunctionKind::CheckAuthenticationBotToken, false},
        Entry{core::TdFunctionKind::SetAuthenticationEmailAddress, false},
        Entry{core::TdFunctionKind::CheckAuthenticationEmailCode, false},
        Entry{core::TdFunctionKind::CheckAuthenticationCode, false},
        Entry{core::TdFunctionKind::RegisterUser, false},
        Entry{core::TdFunctionKind::CheckAuthenticationPassword, false},
        Entry{core::TdFunctionKind::GetOption, true},
        Entry{core::TdFunctionKind::GetMe, true},
        Entry{core::TdFunctionKind::GetSavedMessagesTags, true},
        Entry{core::TdFunctionKind::SearchSavedMessages, true},
        Entry{core::TdFunctionKind::GetActiveSessions, true},
        Entry{core::TdFunctionKind::TerminateSession, false},
        Entry{core::TdFunctionKind::LogOut, false},
        Entry{core::TdFunctionKind::Close, false},
    };
    std::size_t sent_count = fake.runtime->sent_functions().size();
    for (const auto& entry : entries) {
        INFO(core::td_function_name(entry.function));
        auto response = fake.client->send_read(
            snapshot, entry.function,
            core::TdValue::scripted_function(core::TdFunctionData{entry.function}));
        if (!entry.allowed) {
            REQUIRE(response.wait_for(0ms) == std::future_status::ready);
            CHECK_THROWS_AS(response.get(), core::TdAuthorizationError);
            CHECK(fake.runtime->sent_functions().size() == sent_count);
            continue;
        }
        REQUIRE(fake.runtime->wait_for_sent(sent_count + 1));
        const auto sent = fake.runtime->sent_functions();
        CHECK(sent.back().function.kind() == entry.function);
        fake.runtime->push_response(fake.first, sent.back().query_id,
                                    core::TdValue::from(core::TdOk{}));
        REQUIRE(response.wait_for(2s) == std::future_status::ready);
        CHECK(response.get().get_if<core::TdOk>() != nullptr);
        ++sent_count;
    }
}

TEST_CASE("bootstrap sends all 14 exact fields while neutral data redacts credentials",
          "[core][auth-bootstrap]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    TempBootstrapRoot::write_config(env, "default_account = \"main\"\n"
                                         "[accounts.main]\n"
                                         "api_id_cmd = \"id-command-sentinel\"\n"
                                         "api_hash_cmd = \"hash-command-sentinel\"\n"
                                         "db_key_cmd = \"db-command-sentinel\"\n");
    const config::Store store(paths::config_file(env), env.uid);
    auto bootstrap_snapshot = capture(root, env, store);
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    std::vector<secret_hook::HookField> hook_calls;
    core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot),
                                  [&](const secret_hook::HookRequest& request) {
                                      hook_calls.push_back(request.field);
                                      switch (request.field) {
                                      case secret_hook::HookField::ApiId:
                                          return hook_value("314159");
                                      case secret_hook::HookField::ApiHash:
                                          return hook_value("api-hash-value-sentinel");
                                      case secret_hook::HookField::DatabaseKey:
                                          return hook_value("database-key-value-sentinel");
                                      case secret_hook::HookField::Password:
                                      case secret_hook::HookField::BotToken:
                                          return hook_value("unexpected");
                                      }
                                      return hook_value("unexpected");
                                  });

    auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
    REQUIRE(result);
    REQUIRE(fake.runtime->wait_for_sent(2));
    const auto sent = fake.runtime->sent_functions();
    REQUIRE(sent.size() == 2);
    const auto& function = sent[1].function;
    REQUIRE(function.has_type("setTdlibParameters"));
    REQUIRE(function.fields().size() == 14);
    const std::vector<std::string_view> field_names{
        "use_test_dc",
        "database_directory",
        "files_directory",
        "database_encryption_key",
        "use_file_database",
        "use_chat_info_database",
        "use_message_database",
        "use_secret_chats",
        "api_id",
        "api_hash",
        "system_language_code",
        "device_model",
        "system_version",
        "application_version",
    };
    for (const auto name : field_names) {
        INFO(name);
        REQUIRE(field(function, name) != nullptr);
    }
    CHECK(std::get<bool>(*field(function, "use_test_dc")) == false);
    CHECK(std::get<bool>(*field(function, "use_file_database")) == true);
    CHECK(std::get<bool>(*field(function, "use_chat_info_database")) == true);
    CHECK(std::get<bool>(*field(function, "use_message_database")) == true);
    CHECK(std::get<bool>(*field(function, "use_secret_chats")) == false);
    CHECK(std::get<std::int64_t>(*field(function, "api_id")) == 314159);
    const auto database_directory = std::get<std::string>(*field(function, "database_directory"));
    const auto files_directory = std::get<std::string>(*field(function, "files_directory"));
    CHECK(database_directory.ends_with("/tgcli/accounts/main/tdlib/db"));
    CHECK(files_directory.ends_with("/tgcli/accounts/main/tdlib/files"));
    for (const auto& directory : {database_directory, files_directory}) {
        struct stat status {};
        REQUIRE(::stat(directory.c_str(), &status) == 0);
        CHECK(S_ISDIR(status.st_mode));
        CHECK((status.st_mode & 07777) == 0700);
        CHECK(status.st_uid == env.uid);
    }
    CHECK(std::get<std::string>(*field(function, "system_language_code")) == "en");
    CHECK(std::get<std::string>(*field(function, "device_model")) == "tgcli");
#if defined(__APPLE__)
    CHECK(std::get<std::string>(*field(function, "system_version")) == "macOS");
#else
    CHECK(std::get<std::string>(*field(function, "system_version")) == "Linux");
#endif
    CHECK(std::get<std::string>(*field(function, "application_version")) == "0.1.0-test");
    CHECK(
        std::holds_alternative<core::TdRedactedValue>(*field(function, "database_encryption_key")));
    CHECK(std::holds_alternative<core::TdRedactedValue>(*field(function, "api_hash")));
    for (const auto& item : function.fields()) {
        if (const auto* value = std::get_if<std::string>(&item.value())) {
            CHECK(value->find("sentinel") == std::string::npos);
        }
    }
    CHECK(hook_calls == std::vector<secret_hook::HookField>{secret_hook::HookField::ApiId,
                                                            secret_hook::HookField::ApiHash,
                                                            secret_hook::HookField::DatabaseKey});

    const auto duplicate = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
    REQUIRE(duplicate.error);
    CHECK(duplicate.error->failure == core::BootstrapFailure::Duplicate);
    CHECK(fake.runtime->sent_functions().size() == 2);
}

TEST_CASE("bootstrap credential precedence is field-local and hooks run once",
          "[core][auth-bootstrap]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    TempBootstrapRoot::write_config(env, "default_account = \"main\"\n"
                                         "[accounts.main]\n"
                                         "api_id_cmd = \"id-hook\"\n"
                                         "api_hash_cmd = \"hash-hook\"\n"
                                         "db_key_cmd = \"db-hook\"\n");
    const config::Store store(paths::config_file(env), env.uid);
    auto bootstrap_snapshot = capture(root, env, store, "271828", "environment-hash");
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    std::vector<secret_hook::HookField> calls;
    core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot),
                                  [&](const secret_hook::HookRequest& request) {
                                      calls.push_back(request.field);
                                      if (request.field == secret_hook::HookField::DatabaseKey) {
                                          return hook_value("db-key");
                                      }
                                      return hook_value("must-not-run");
                                  });
    const auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
    REQUIRE(result);
    REQUIRE(fake.runtime->wait_for_sent(2));
    CHECK(calls == std::vector<secret_hook::HookField>{secret_hook::HookField::DatabaseKey});
    CHECK(std::get<std::int64_t>(*field(fake.runtime->sent_functions()[1].function, "api_id")) ==
          271828);
}

TEST_CASE("bootstrap binds InternalAuth and Login to the credential source matrix",
          "[core][auth-bootstrap][safety]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    const config::Store store(paths::config_file(env), env.uid);
    auto bootstrap_snapshot = capture(root, env, store, "1", "hash");
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));

    auto prompted_internal = internal_attempt(*fake.client, *auth);
    prompted_internal.prompted_database_key = "must-not-be-consumed";
    const auto source_result = bootstrap.run(auth, prompted_internal);
    REQUIRE(source_result.error);
    CHECK(source_result.error->failure == core::BootstrapFailure::AuthorizationChanged);

    auto configured_login = internal_attempt(*fake.client, *auth);
    configured_login.interactive = true;
    const auto configured_result = bootstrap.run(auth, configured_login);
    REQUIRE(configured_result.error);
    CHECK(configured_result.error->failure == core::BootstrapFailure::AuthorizationChanged);
    CHECK(fake.runtime->sent_functions().size() == 1);
}

TEST_CASE("bootstrap hook failure is bounded input state with redacted diagnostics",
          "[core][auth-bootstrap]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    TempBootstrapRoot::write_config(env, "default_account = \"main\"\n"
                                         "[accounts.main]\n"
                                         "api_id_cmd = \"command-secret-sentinel\"\n"
                                         "api_hash = \"configured-hash\"\n");
    const config::Store store(paths::config_file(env), env.uid);
    auto bootstrap_snapshot = capture(root, env, store);
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    std::atomic<int> calls = 0;
    core::AuthBootstrap bootstrap(
        *fake.client, store, std::move(bootstrap_snapshot),
        [&](const secret_hook::HookRequest& request) {
            ++calls;
            return secret_hook::HookResult{
                {}, secret_hook::HookError{request.field, secret_hook::HookFailure::Timeout, {}}};
        });

    auto attempt = internal_attempt(*fake.client, *auth);
    const auto terminal = bootstrap.run(auth, attempt);
    REQUIRE(terminal.error);
    CHECK(terminal.error->failure == core::BootstrapFailure::HookFailed);
    const auto diagnostic = core::describe(*terminal.error);
    CHECK(diagnostic.find("command-secret-sentinel") == std::string::npos);
    CHECK(diagnostic == "hook_failed: hook api_id_cmd failed: timeout");
    CHECK(calls.load() == 1);
    CHECK(fake.runtime->sent_functions().size() == 1);

    attempt.interactive = true;
    const auto input = bootstrap.run(auth, attempt);
    REQUIRE(input.error);
    CHECK(input.error->failure == core::BootstrapFailure::InputRequired);
    CHECK(calls.load() == 1);

    attempt.prompted_app.api_id = 42;
    attempt.prompted_app.api_hash = "prompted-pair";
    const auto retried = bootstrap.run(auth, attempt);
    REQUIRE(retried);
    CHECK(calls.load() == 1);
    REQUIRE(fake.runtime->wait_for_sent(2));
}

TEST_CASE("implicit main materializes atomically only at the pre-parameter boundary",
          "[core][auth-bootstrap]") {
    SECTION("prompted credentials persist together immediately before the send") {
        const TempBootstrapRoot root;
        const auto env = root.environment();
        const config::Store store(paths::config_file(env), env.uid);
        auto bootstrap_snapshot = capture(root, env, store);
        auto fake = make_fake_boundary();
        const auto auth = fake.client->auth_state();
        core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
        auto attempt = internal_attempt(*fake.client, *auth);
        attempt.interactive = true;
        attempt.prompted_app.api_id = 777;
        attempt.prompted_app.api_hash = "prompted-hash";

        auto result = bootstrap.run(auth, attempt);
        REQUIRE(result);
        REQUIRE(result.materialized_snapshot);
        REQUIRE(result.materialized_snapshot->accounts.contains("main"));
        CHECK(result.materialized_snapshot->accounts.at("main").api_id == 777);
        CHECK(result.materialized_snapshot->accounts.at("main").api_hash == "prompted-hash");
        CHECK_FALSE(result.materialized_snapshot->accounts.at("main").allow_write);
        REQUIRE(fake.runtime->wait_for_sent(2));
    }

    SECTION("CAS conflict sends no parameters and does not merge main") {
        const TempBootstrapRoot root;
        const auto env = root.environment();
        const config::Store store(paths::config_file(env), env.uid);
        auto bootstrap_snapshot = capture(root, env, store);
        const auto missing = store.load();
        REQUIRE(missing);
        REQUIRE(store.add_account(missing.snapshot->identity, "work").status ==
                config::MutationStatus::Applied);
        auto fake = make_fake_boundary();
        const auto auth = fake.client->auth_state();
        core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
        auto attempt = internal_attempt(*fake.client, *auth);
        attempt.interactive = true;
        attempt.prompted_app.api_id = 777;
        attempt.prompted_app.api_hash = "prompted-hash";

        const auto result = bootstrap.run(auth, attempt);
        REQUIRE(result.error);
        CHECK(result.error->failure == core::BootstrapFailure::ConfigConflict);
        CHECK(fake.runtime->sent_functions().size() == 1);
        const auto current = store.load();
        REQUIRE(current);
        CHECK_FALSE(current.snapshot->accounts.contains("main"));
        CHECK(current.snapshot->accounts.contains("work"));
    }

    SECTION("pre-boundary cancellation leaves the virtual account absent") {
        const TempBootstrapRoot root;
        const auto env = root.environment();
        const config::Store store(paths::config_file(env), env.uid);
        auto bootstrap_snapshot = capture(root, env, store);
        auto fake = make_fake_boundary();
        const auto auth = fake.client->auth_state();
        core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
        const std::stop_source cancellation;
        cancellation.request_stop();
        auto attempt = internal_attempt(*fake.client, *auth);
        attempt.interactive = true;
        attempt.control.cancellation = cancellation.get_token();
        attempt.prompted_app.api_id = 777;
        attempt.prompted_app.api_hash = "prompted-hash";

        const auto result = bootstrap.run(auth, attempt);
        REQUIRE(result.error);
        CHECK(result.error->failure == core::BootstrapFailure::Cancelled);
        CHECK(fake.runtime->sent_functions().size() == 1);
        REQUIRE(store.load());
        CHECK(store.load().snapshot->accounts.empty());
    }
}

TEST_CASE("implicit main commit is conditional on the admitted auth snapshot",
          "[core][auth-bootstrap][race][safety]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    auto hooks = std::make_shared<config::testing::StoreHooks>();
    const config::Store store(paths::config_file(env), hooks, env.uid);
    auto bootstrap_snapshot = capture(root, env, store, "1", "hash");
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    hooks->at_stage = [&](config::testing::MutationStage stage) {
        if (stage != config::testing::MutationStage::BeforeCommit) {
            return;
        }
        fake.runtime->push_update(fake.first, {},
                                  core::AuthStateData{core::AuthState::WaitPhoneNumber});
        REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 2; }));
    };
    core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));

    const auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
    REQUIRE(result.error);
    CHECK(result.error->failure == core::BootstrapFailure::AuthorizationChanged);
    CHECK(fake.runtime->sent_functions().size() == 1);
    const auto current = store.load();
    REQUIRE(current);
    CHECK(current.snapshot->accounts.empty());
    CHECK(current.snapshot->identity == "missing");
}

TEST_CASE("bootstrap rejects stale authorization and isolates production from test DC",
          "[core][auth-bootstrap][safety]") {
    SECTION("test identity mismatch is rejected at capture") {
        const TempBootstrapRoot root;
        const auto env = root.environment(true);
        const config::Store store(paths::config_file(env), env.uid);
        const auto loaded = store.load();
        REQUIRE(loaded);
        const auto captured = core::capture_bootstrap_snapshot("main", loaded.snapshot, env, false,
                                                               "test", "1", "hash");
        REQUIRE(captured.error);
        CHECK(captured.error->failure == core::BootstrapFailure::InvalidSnapshot);
    }

    SECTION("test roots and parameter bit remain test-only") {
        const TempBootstrapRoot root;
        const auto env = root.environment(true);
        const config::Store store(paths::config_file(env), env.uid);
        auto bootstrap_snapshot = capture(root, env, store, "1", "hash");
        CHECK(bootstrap_snapshot.config_path.find("/tgcli-test/") != std::string::npos);
        CHECK(bootstrap_snapshot.database_directory.find("/tgcli-test/") != std::string::npos);
        auto fake = make_fake_boundary();
        const auto auth = fake.client->auth_state();
        core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
        const auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
        REQUIRE(result);
        REQUIRE(result.materialized_snapshot);
        REQUIRE(result.materialized_snapshot->accounts.contains("main"));
        CHECK_FALSE(result.materialized_snapshot->accounts.at("main").api_id);
        CHECK_FALSE(result.materialized_snapshot->accounts.at("main").api_hash);
        REQUIRE(fake.runtime->wait_for_sent(2));
        CHECK(std::get<bool>(*field(fake.runtime->sent_functions()[1].function, "use_test_dc")));
    }

    SECTION("an auth sequence change before the boundary sends and materializes nothing") {
        const TempBootstrapRoot root;
        const auto env = root.environment();
        const config::Store store(paths::config_file(env), env.uid);
        auto bootstrap_snapshot = capture(root, env, store, "1", "hash");
        auto fake = make_fake_boundary();
        const auto stale = fake.client->auth_state();
        fake.runtime->push_update(fake.first, {},
                                  core::AuthStateData{core::AuthState::WaitPhoneNumber});
        REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 2; }));
        core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
        const auto result = bootstrap.run(stale, internal_attempt(*fake.client, *stale));
        REQUIRE(result.error);
        CHECK(result.error->failure == core::BootstrapFailure::AuthorizationChanged);
        CHECK(fake.runtime->sent_functions().size() == 1);
        REQUIRE(store.load());
        CHECK(store.load().snapshot->accounts.empty());
    }
}

TEST_CASE("bootstrap path identity uses exact normalized derived roots",
          "[core][auth-bootstrap][safety]") {
    SECTION("foreign-looking parent components remain valid") {
        const TempBootstrapRoot root;
        auto env = root.environment();
        const auto parent = root.root_path() / "tgcli-test";
        for (const auto& child : {"config", "data", "state", "runtime"}) {
            const auto directory = parent / child;
            REQUIRE(std::filesystem::create_directories(directory / "unused"));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
            REQUIRE(::chmod((directory / "unused").c_str(), 0700) == 0);
        }
        env.xdg_config_home = (parent / "config" / "unused" / "..").string();
        env.xdg_data_home = (parent / "data" / "unused" / "..").string();
        env.xdg_state_home = (parent / "state" / "unused" / "..").string();
        env.xdg_runtime_dir = (parent / "runtime" / "unused" / "..").string();
        const config::Store store(
            std::filesystem::path(paths::config_file(env)).lexically_normal().string(), env.uid);
        const auto loaded = store.load();
        REQUIRE(loaded);
        const auto captured = core::capture_bootstrap_snapshot("main", loaded.snapshot, env, false,
                                                               "test", "1", "hash");
        REQUIRE(captured.snapshot);
        CHECK(captured.snapshot->config_namespace_directory.ends_with("/tgcli-test/config/tgcli"));
        CHECK(captured.snapshot->data_namespace_directory.ends_with("/tgcli-test/data/tgcli"));
        CHECK(captured.snapshot->state_namespace_directory.ends_with("/tgcli-test/state/tgcli"));
        CHECK(
            captured.snapshot->runtime_namespace_directory.ends_with("/tgcli-test/runtime/tgcli"));
    }

    SECTION("a symlinked namespace root is rejected before TD send") {
        const TempBootstrapRoot root;
        const auto env = root.environment();
        const auto target = root.root_path() / "symlink-target";
        REQUIRE(std::filesystem::create_directory(target));
        REQUIRE(::chmod(target.c_str(), 0700) == 0);
        const auto data_namespace = root.root_path() / "data" / "tgcli";
        REQUIRE(::symlink(target.c_str(), data_namespace.c_str()) == 0);
        const config::Store store(paths::config_file(env), env.uid);
        auto bootstrap_snapshot = capture(root, env, store, "1", "hash");
        auto fake = make_fake_boundary();
        const auto auth = fake.client->auth_state();
        core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
        const auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
        REQUIRE(result.error);
        CHECK(result.error->failure == core::BootstrapFailure::PathInvalid);
        CHECK(fake.runtime->sent_functions().size() == 1);
    }
}

TEST_CASE("parameter bootstrap preserves response-first and update-first query ordering",
          "[core][auth-bootstrap]") {
    for (const bool update_first : {false, true}) {
        INFO("update_first=" << update_first);
        const TempBootstrapRoot root;
        const auto env = root.environment();
        TempBootstrapRoot::write_config(env, "default_account = \"main\"\n"
                                             "[accounts.main]\n"
                                             "api_id = 1\n"
                                             "api_hash = \"hash\"\n");
        const config::Store store(paths::config_file(env), env.uid);
        auto bootstrap_snapshot = capture(root, env, store);
        auto fake = make_fake_boundary(core::AuthState::WaitTdlibParameters, update_first);
        const auto auth = fake.client->auth_state();
        core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
        const auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
        REQUIRE(result);
        REQUIRE(fake.runtime->wait_for_sent(2));
        const auto sent = fake.runtime->sent_functions();
        REQUIRE(sent.size() == 2);
        CHECK(sent.at(0).query_id == 1);
        CHECK(sent.at(0).function.has_type("getAuthorizationState"));
        CHECK(sent.at(1).query_id == 2);
        CHECK(sent.at(1).function.has_type("setTdlibParameters"));

        if (update_first) {
            fake.runtime->push_response(fake.first, 1, {},
                                        core::AuthStateData{core::AuthState::Ready});
            std::this_thread::sleep_for(20ms);
            CHECK(fake.client->auth_state()->auth_sequence == 1);
            CHECK(fake.client->auth_state()->data.state == core::AuthState::WaitTdlibParameters);
        }
    }
}

TEST_CASE("concurrent bootstrap attempts emit one parameter request",
          "[core][auth-bootstrap][race]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    TempBootstrapRoot::write_config(env, "default_account = \"main\"\n"
                                         "[accounts.main]\n"
                                         "api_id = 1\n"
                                         "api_hash = \"hash\"\n");
    const config::Store store(paths::config_file(env), env.uid);
    auto bootstrap_snapshot = capture(root, env, store);
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    core::AuthBootstrap bootstrap(*fake.client, store, std::move(bootstrap_snapshot));
    std::barrier start(3);
    std::array<int, 2> outcomes{};
    std::array<std::thread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers.at(index) = std::thread([&, index] {
            start.arrive_and_wait();
            auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
            if (result) {
                outcomes.at(index) = 1;
            } else if (result.error && result.error->failure == core::BootstrapFailure::Duplicate) {
                outcomes.at(index) = 2;
            } else {
                outcomes.at(index) = 100;
            }
        });
    }
    start.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(outcomes[0] + outcomes[1] == 3);
    CHECK(outcomes[0] * outcomes[1] == 2);
    REQUIRE(fake.runtime->wait_for_sent(2));
    CHECK(fake.runtime->sent_functions().size() == 2);
}

TEST_CASE("authorization change during a hook stops before paths config and TD send",
          "[core][auth-bootstrap][race]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    TempBootstrapRoot::write_config(env, "[accounts.main]\n"
                                         "api_id_cmd = \"id-hook\"\n"
                                         "api_hash = \"hash\"\n");
    const config::Store store(paths::config_file(env), env.uid);
    auto bootstrap_snapshot = capture(root, env, store);
    const auto data_directory = bootstrap_snapshot.account_data_directory;
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    core::AuthBootstrap bootstrap(
        *fake.client, store, std::move(bootstrap_snapshot), [&](const secret_hook::HookRequest&) {
            fake.runtime->push_update(fake.first, {},
                                      core::AuthStateData{core::AuthState::WaitPhoneNumber});
            REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 2; }));
            return hook_value("1");
        });

    const auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
    REQUIRE(result.error);
    CHECK(result.error->failure == core::BootstrapFailure::AuthorizationChanged);
    CHECK(fake.runtime->sent_functions().size() == 1);
    CHECK_FALSE(std::filesystem::exists(data_directory));
}

TEST_CASE("successful database-key hook bytes are not reused after authorization changes",
          "[core][auth-bootstrap][race][safety]") {
    const TempBootstrapRoot root;
    const auto env = root.environment();
    TempBootstrapRoot::write_config(env, "[accounts.main]\n"
                                         "api_id = 1\n"
                                         "api_hash = \"hash\"\n"
                                         "db_key_cmd = \"db-hook\"\n");
    const config::Store store(paths::config_file(env), env.uid);
    auto bootstrap_snapshot = capture(root, env, store);
    auto fake = make_fake_boundary();
    const auto auth = fake.client->auth_state();
    std::atomic<int> calls = 0;
    core::AuthBootstrap bootstrap(
        *fake.client, store, std::move(bootstrap_snapshot),
        [&](const secret_hook::HookRequest& request) {
            REQUIRE(request.field == secret_hook::HookField::DatabaseKey);
            ++calls;
            fake.runtime->push_update(fake.first, {},
                                      core::AuthStateData{core::AuthState::WaitPhoneNumber});
            REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 2; }));
            return hook_value("database-key-retention-sentinel");
        });

    const auto result = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
    REQUIRE(result.error);
    CHECK(result.error->failure == core::BootstrapFailure::AuthorizationChanged);
    CHECK(core::describe(*result.error).find("retention-sentinel") == std::string::npos);
    CHECK(calls.load() == 1);
    CHECK(fake.runtime->sent_functions().size() == 1);

    const auto repeated = bootstrap.run(auth, internal_attempt(*fake.client, *auth));
    REQUIRE(repeated.error);
    CHECK(repeated.error->failure == core::BootstrapFailure::AuthorizationChanged);
    CHECK(calls.load() == 1);
}
