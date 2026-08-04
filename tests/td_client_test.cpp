// Touches real (uninstrumented) tdlib — tagged [tdlib] and excluded from the
// TSan suite per the sanitizer policy in CLAUDE.md.

#include "core/td_client.hpp"
#include "support/scripted_td_runtime.hpp"
#include "support/td_client_test_access.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

using tgcli::core::TdClient;
using tgcli::core::TdValue;
namespace td_api = td::td_api;

namespace {

using NativeFunctionPtr = td_api::object_ptr<td_api::Function>;
using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

class PrivateTdLog {
  public:
    PrivateTdLog() {
        std::vector<char> pattern{'/', 't', 'm', 'p', '/', 't', 'g', 'c', 'l', 'i', '-', 't',
                                  'd', 'l', 'o', 'g', '-', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
        char* created = ::mkdtemp(pattern.data());
        if (created == nullptr || ::chmod(created, 0700) != 0) {
            throw std::runtime_error("cannot create private TDLib test log directory");
        }
        directory_ = created;
    }

    ~PrivateTdLog() {
        std::filesystem::remove_all(directory_);
    }

    [[nodiscard]] tgcli::core::TdLogConfiguration configuration() const {
        return {.file_path = directory_ + "/tdlib.log"};
    }

    [[nodiscard]] std::string path() const {
        return directory_ + "/tdlib.log";
    }

  private:
    std::string directory_;
};

} // namespace

TEST_CASE("static tdlib_version answers without a client", "[core][tdlib]") {
    const auto version = TdClient::tdlib_version();
    CHECK_FALSE(version.empty());
    CHECK(version != "unknown");
}

TEST_CASE("production TDLib logging is ERROR-only before client creation",
          "[core][tdlib][logging]") {
    const PrivateTdLog log;
    TdClient client(log.configuration());

    auto verbosity =
        td::ClientManager::execute(td_api::make_object<td_api::getLogVerbosityLevel>());
    REQUIRE(verbosity != nullptr);
    REQUIRE(verbosity->get_id() == td_api::logVerbosityLevel::ID);
    CHECK(static_cast<td_api::logVerbosityLevel&>(*verbosity).verbosity_level_ ==
          tgcli::core::kTdLogVerbosity);

    auto stream = td::ClientManager::execute(td_api::make_object<td_api::getLogStream>());
    REQUIRE(stream != nullptr);
    REQUIRE(stream->get_id() == td_api::logStreamEmpty::ID);

    constexpr std::string_view safe_error = "safe-error-marker";
    constexpr std::string_view credential = "authentication-code-must-not-appear";
    auto error_result = td::ClientManager::execute(td_api::make_object<td_api::addLogMessage>(
        tgcli::core::kTdLogVerbosity, std::string(safe_error)));
    REQUIRE(error_result != nullptr);
    REQUIRE(error_result->get_id() == td_api::ok::ID);
    auto request = td_api::make_object<td_api::checkAuthenticationCode>(std::string(credential));
    auto info_result = td::ClientManager::execute(td_api::make_object<td_api::addLogMessage>(
        tgcli::core::kTdLogInfoVerbosity, td_api::to_string(request)));
    REQUIRE(info_result != nullptr);
    REQUIRE(info_result->get_id() == td_api::ok::ID);

    std::ifstream input(log.path(), std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    CHECK(contents.find(safe_error) != std::string::npos);
    CHECK(contents.find(credential) == std::string::npos);
    client.close();
}

TEST_CASE("clean close against real tdlib", "[core][tdlib]") {
    const PrivateTdLog log;
    TdClient client(log.configuration());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.auth_state()->auth_sequence == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto snapshot = client.auth_state();
    REQUIRE(snapshot->auth_sequence != 0);

    bool saw_closed = false;
    client.subscribe_updates([&saw_closed](const TdValue& update) {
        const auto* native_update = update.get_if<NativeObjectPtr>();
        if (native_update == nullptr || *native_update == nullptr) {
            return;
        }
        const auto& object = **native_update;
        if (object.get_id() == td_api::updateAuthorizationState::ID) {
            const auto& state =
                *static_cast<const td_api::updateAuthorizationState&>(object).authorization_state_;
            if (state.get_id() == td_api::authorizationStateClosed::ID) {
                saw_closed = true;
            }
        }
    });

    client.close();
    CHECK(saw_closed);
    // close() is idempotent; a second call must not hang or crash.
    client.close();
}

TEST_CASE("send after close returns a ready exceptional future", "[core][tdlib][lifecycle]") {
    const PrivateTdLog log;
    TdClient client(log.configuration());
    client.close();

    NativeFunctionPtr request = td_api::make_object<td_api::getOption>("version");
    auto response = client.send_read(
        client.auth_state(), tgcli::core::TdFunctionKind::GetOption,
        TdValue::function(std::move(request),
                          tgcli::core::TdFunctionData{tgcli::core::TdFunctionKind::GetOption}));

    REQUIRE(response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    CHECK_THROWS_AS(response.get(), std::runtime_error);
}

TEST_CASE("deadline-aware close can resume after a timed-out proof", "[core][lifecycle]") {
    auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>(false);
    auto* scripted = runtime.get();
    TdClient client(std::move(runtime));
    REQUIRE(scripted->wait_for_clients(1));
    const auto generation = scripted->clients().front();

    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    CHECK_FALSE(client.close_until(first_deadline));
    CHECK(std::chrono::steady_clock::now() < first_deadline + std::chrono::milliseconds(100));

    scripted->push_response(
        generation, 1, {}, tgcli::core::AuthStateData{tgcli::core::AuthState::WaitTdlibParameters});
    REQUIRE(scripted->wait_for_sent(2));
    scripted->push_update(generation, {},
                          tgcli::core::AuthStateData{tgcli::core::AuthState::Closed});
    CHECK(client.close_until(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    CHECK(client.close_until(std::chrono::steady_clock::now()));
}

TEST_CASE("forged bootstrap ownership cannot reach the production TD boundary",
          "[core][tdlib][safety]") {
    const PrivateTdLog log;
    TdClient client(log.configuration());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.auth_state()->auth_sequence == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto snapshot = client.auth_state();
    REQUIRE(snapshot->auth_sequence != 0);
    REQUIRE(snapshot->data.state == tgcli::core::AuthState::WaitTdlibParameters);

    NativeFunctionPtr request = td_api::make_object<td_api::getMe>();
    auto response = client.send(
        tgcli::core::TdSendDescriptor{
            .function = tgcli::core::TdFunctionKind::SetTdlibParameters,
            .tier = tgcli::core::DescriptorKind::AuthBootstrap,
            .owner = {tgcli::core::TdOwnerKind::InternalAuth, 1},
            .client_generation = snapshot->client_generation,
            .auth_sequence = snapshot->auth_sequence,
            .auth_state = snapshot->data.state,
        },
        TdValue::function(
            std::move(request),
            tgcli::core::TdFunctionData{tgcli::core::TdFunctionKind::SetTdlibParameters}));

    REQUIRE(response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    CHECK_THROWS_AS(response.get(), tgcli::core::TdAuthorizationError);
}
