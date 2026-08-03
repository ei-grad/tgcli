// Touches real (uninstrumented) tdlib — tagged [tdlib] and excluded from the
// TSan suite per the sanitizer policy in CLAUDE.md.

#include "core/td_client.hpp"

#include <chrono>
#include <stdexcept>
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

TEST_CASE("request/response correlation and clean close against real tdlib", "[core][tdlib]") {
    TdClient client;

    NativeFunctionPtr request = td_api::make_object<td_api::getOption>("version");
    auto future = client.send(TdValue::from(std::move(request)));
    auto response = future.get();
    const auto* native_response = response.get_if<NativeObjectPtr>();
    REQUIRE(native_response != nullptr);
    REQUIRE(*native_response != nullptr);
    REQUIRE((*native_response)->get_id() == td_api::optionValueString::ID);
    CHECK(static_cast<const td_api::optionValueString&>(**native_response).value_ ==
          TdClient::tdlib_version());

    bool saw_closed = false;
    client.subscribe_updates([&saw_closed](const TdValue& update) {
        const auto* native_update = update.get_if<const td_api::Object*>();
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
    auto response = client.send(TdValue::from(std::move(request)));

    REQUIRE(response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    CHECK_THROWS_AS(response.get(), std::runtime_error);
}
