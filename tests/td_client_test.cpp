// Touches real (uninstrumented) tdlib — tagged [tdlib] and excluded from the
// TSan suite per the sanitizer policy in CLAUDE.md.

#include "core/td_client.hpp"

#include <catch2/catch_test_macros.hpp>

using tgcli::core::TdClient;
namespace td_api = td::td_api;

TEST_CASE("static tdlib_version answers without a client", "[core][tdlib]") {
    const auto version = TdClient::tdlib_version();
    CHECK_FALSE(version.empty());
    CHECK(version != "unknown");
}

TEST_CASE("request/response correlation and clean close against real tdlib", "[core][tdlib]") {
    TdClient client;

    auto future = client.send(td_api::make_object<td_api::getOption>("version"));
    auto response = future.get();
    REQUIRE(response != nullptr);
    REQUIRE(response->get_id() == td_api::optionValueString::ID);
    CHECK(static_cast<td_api::optionValueString&>(*response).value_ == TdClient::tdlib_version());

    bool saw_closed = false;
    client.subscribe_updates([&saw_closed](const td_api::Object& update) {
        if (update.get_id() == td_api::updateAuthorizationState::ID) {
            const auto& state =
                *static_cast<const td_api::updateAuthorizationState&>(update).authorization_state_;
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
