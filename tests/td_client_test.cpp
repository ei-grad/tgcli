// Touches real (uninstrumented) tdlib — tagged [tdlib] and excluded from the
// TSan suite per the sanitizer policy in CLAUDE.md.

#include "core/td_client.hpp"
#include "support/td_client_test_access.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using tgcli::core::TdClient;
using tgcli::core::TdValue;
namespace td_api = td::td_api;

namespace {

using NativeFunctionPtr = td_api::object_ptr<td_api::Function>;
using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

} // namespace

TEST_CASE("static tdlib_version answers without a client", "[core][tdlib]") {
    const auto version = TdClient::tdlib_version();
    CHECK_FALSE(version.empty());
    CHECK(version != "unknown");
}

TEST_CASE("clean close against real tdlib", "[core][tdlib]") {
    TdClient client;

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
    TdClient client;
    client.close();

    NativeFunctionPtr request = td_api::make_object<td_api::getOption>("version");
    auto response = client.send_read(
        client.auth_state(), tgcli::core::TdFunctionKind::GetOption,
        TdValue::function(std::move(request),
                          tgcli::core::TdFunctionData{tgcli::core::TdFunctionKind::GetOption}));

    REQUIRE(response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    CHECK_THROWS_AS(response.get(), std::runtime_error);
}

TEST_CASE("forged bootstrap ownership cannot reach the production TD boundary",
          "[core][tdlib][safety]") {
    TdClient client;
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
