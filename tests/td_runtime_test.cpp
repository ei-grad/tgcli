#include "core/td_client.hpp"
#include "support/scripted_td_runtime.hpp"
#include "support/td_client_test_access.hpp"

#include <barrier>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using tgcli::core::auth_state_name;
using tgcli::core::AuthCodeDelivery;
using tgcli::core::AuthCodeDeliveryInfo;
using tgcli::core::AuthEmailResetState;
using tgcli::core::AuthState;
using tgcli::core::AuthStateData;
using tgcli::core::AuthStateSnapshot;
using tgcli::core::AuthWaitCode;
using tgcli::core::AuthWaitEmailAddress;
using tgcli::core::AuthWaitEmailCode;
using tgcli::core::AuthWaitOtherDeviceConfirmation;
using tgcli::core::AuthWaitPassword;
using tgcli::core::AuthWaitPremiumPurchase;
using tgcli::core::AuthWaitRegistration;
using tgcli::core::TdClient;
using tgcli::core::TdFunctionKind;
using tgcli::core::TdSendDescriptor;
using tgcli::core::TdValue;
using tgcli::test::ScriptedClient;
using tgcli::test::ScriptedTdRuntime;

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

struct FakeClient {
    FakeClient(std::unique_ptr<TdClient> client_value, ScriptedTdRuntime* runtime_value,
               ScriptedClient first_value)
        : client(std::move(client_value)), runtime(runtime_value), first(first_value) {}

    FakeClient(const FakeClient&) = delete;
    FakeClient& operator=(const FakeClient&) = delete;
    FakeClient(FakeClient&&) noexcept = default;
    FakeClient& operator=(FakeClient&&) noexcept = default;

    ~FakeClient() {
        if (client == nullptr) {
            return;
        }
        const auto current_clients = runtime->clients();
        if (!current_clients.empty()) {
            const auto current = current_clients.back();
            static_cast<void>(eventually([&] {
                return client->auth_state()->client_generation == current.client_generation;
            }));
            const auto snapshot = client->auth_state();
            if (snapshot != nullptr && snapshot->client_generation == current.client_generation &&
                snapshot->auth_sequence == 0) {
                runtime->push_response(current, 1, {}, AuthStateData{AuthState::Ready});
                static_cast<void>(eventually([&] {
                    const auto installed = client->auth_state();
                    return installed->client_generation == current.client_generation &&
                           installed->auth_sequence != 0;
                }));
            }
        }
        client->close();
    }

    std::unique_ptr<TdClient> client;
    ScriptedTdRuntime* runtime;
    ScriptedClient first;
};

FakeClient make_fake_client(bool close_automatically = true) {
    auto runtime = std::make_unique<ScriptedTdRuntime>(close_automatically);
    auto* runtime_ptr = runtime.get();
    auto client = std::make_unique<TdClient>(std::move(runtime));
    REQUIRE(runtime_ptr->wait_for_sent(1));
    const auto clients = runtime_ptr->clients();
    REQUIRE(clients.size() == 1);
    return FakeClient{std::move(client), runtime_ptr, clients.front()};
}

std::future<TdValue> send_test_read(TdClient& client) {
    const auto snapshot = client.auth_state();
    return client.send_read(
        snapshot, TdFunctionKind::GetOption,
        TdValue::scripted_function(tgcli::core::TdFunctionData{TdFunctionKind::GetOption}));
}

} // namespace

TEST_CASE("activation emits no unsolicited update before exact query-1 bootstrap",
          "[core][td-runtime]") {
    auto fake = make_fake_client();
    const auto sent = fake.runtime->sent_functions();

    REQUIRE(sent.size() == 1);
    CHECK(fake.runtime->initialized_before_first_client());
    CHECK(sent.front().client_id == fake.first.client_id);
    CHECK(sent.front().client_generation == 1);
    CHECK(sent.front().query_id == 1);
    CHECK(sent.front().function.has_type("getAuthorizationState"));
    CHECK(sent.front().function.fields().empty());

    const auto snapshot = fake.client->auth_state();
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->client_generation == 1);
    CHECK(snapshot->auth_sequence == 0);
    CHECK(snapshot->data.state == AuthState::Unknown);
}

TEST_CASE("bootstrap response-first and update-first auth_sequence rules are exact",
          "[core][td-runtime]") {
    SECTION("response-first installs sequence 1 and an equal update installs sequence 2") {
        auto fake = make_fake_client();
        fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
        REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 1; }));
        CHECK(fake.client->auth_state()->data.state == AuthState::Ready);

        fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Ready});
        REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 2; }));
        CHECK(fake.client->auth_state()->data.state == AuthState::Ready);
    }

    SECTION("update-first installs sequence 1 and the later response is correlation-only") {
        auto fake = make_fake_client();
        fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Ready});
        REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 1; }));

        fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::LoggingOut});
        std::this_thread::sleep_for(20ms);
        CHECK(fake.client->auth_state()->auth_sequence == 1);
        CHECK(fake.client->auth_state()->data.state == AuthState::Ready);
    }
}

TEST_CASE("immutable auth snapshots cover all 13 pinned states and required metadata",
          "[core][td-runtime]") {
    const std::vector<AuthStateData> states{
        AuthStateData{AuthState::WaitTdlibParameters},
        AuthStateData{AuthState::WaitPhoneNumber},
        AuthStateData{AuthState::WaitPremiumPurchase,
                      AuthWaitPremiumPurchase{.store_product_id = "premium_12m",
                                              .premium_day_count = 365,
                                              .support_email_address = "support@example.test",
                                              .support_email_subject = "purchase"}},
        AuthStateData{AuthState::WaitEmailAddress,
                      AuthWaitEmailAddress{.allow_apple_id = true, .allow_google_id = false}},
        AuthStateData{AuthState::WaitEmailCode,
                      AuthWaitEmailCode{.allow_apple_id = false,
                                        .allow_google_id = true,
                                        .email_address_pattern = "a***@example.test",
                                        .expected_length = 6,
                                        .reset_state = AuthEmailResetState::Pending,
                                        .reset_delay = 42,
                                        .unsupported_reset_tdlib_type_id = std::nullopt}},
        AuthStateData{
            AuthState::WaitCode,
            AuthWaitCode{
                .delivery = AuthCodeDeliveryInfo{.type = AuthCodeDelivery::Sms,
                                                 .expected_length = 5,
                                                 .unsupported_tdlib_type_id = std::nullopt},
                .next_delivery = AuthCodeDeliveryInfo{.type = AuthCodeDelivery::Call,
                                                      .expected_length = 5,
                                                      .unsupported_tdlib_type_id = std::nullopt},
                .resend_timeout = 30}},
        AuthStateData{AuthState::WaitOtherDeviceConfirmation,
                      AuthWaitOtherDeviceConfirmation{.link = "tg://login?token=one"}},
        AuthStateData{AuthState::WaitRegistration, AuthWaitRegistration{.terms_text = "terms",
                                                                        .minimum_user_age = 16,
                                                                        .show_popup = true}},
        AuthStateData{AuthState::WaitPassword,
                      AuthWaitPassword{.hint = "hint",
                                       .has_recovery_email_address = true,
                                       .has_passport_data = true,
                                       .recovery_email_address_pattern = "r***@example.test"}},
        AuthStateData{AuthState::Ready},
        AuthStateData{AuthState::LoggingOut},
        AuthStateData{AuthState::Closing},
        AuthStateData{AuthState::Closed},
    };
    const std::vector<std::string_view> names{
        "wait_tdlib_parameters",
        "wait_phone_number",
        "wait_premium_purchase",
        "wait_email_address",
        "wait_email_code",
        "wait_code",
        "wait_other_device_confirmation",
        "wait_registration",
        "wait_password",
        "ready",
        "logging_out",
        "closing",
        "closed",
    };

    auto fake = make_fake_client();
    std::mutex seen_mutex;
    std::vector<std::shared_ptr<const AuthStateSnapshot>> seen;
    const auto subscription = fake.client->subscribe_auth_states([&](const auto& snapshot) {
        const std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(snapshot);
    });

    fake.runtime->push_response(fake.first, 1, {}, states.front());
    for (std::size_t index = 1; index < states.size(); ++index) {
        fake.runtime->push_update(fake.first, {}, states[index]);
    }
    REQUIRE(eventually([&] {
        const std::lock_guard<std::mutex> lock(seen_mutex);
        return seen.size() >= states.size();
    }));

    {
        const std::lock_guard<std::mutex> lock(seen_mutex);
        for (std::size_t index = 0; index < states.size(); ++index) {
            INFO("auth state index " << index);
            CHECK(seen[index]->client_generation == 1);
            CHECK(seen[index]->auth_sequence == index + 1);
            CHECK(seen[index]->data == states[index]);
            CHECK(auth_state_name(seen[index]->data.state) == names[index]);
        }
    }
    fake.client->unsubscribe_auth_states(subscription);
}

TEST_CASE("payload-equal repeated QR updates remain observable", "[core][td-runtime]") {
    auto fake = make_fake_client();
    std::mutex seen_mutex;
    std::vector<std::shared_ptr<const AuthStateSnapshot>> seen;
    const auto subscription = fake.client->subscribe_auth_states([&](const auto& snapshot) {
        if (snapshot->data.state == AuthState::WaitOtherDeviceConfirmation) {
            const std::lock_guard<std::mutex> lock(seen_mutex);
            seen.push_back(snapshot);
        }
    });
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::WaitPhoneNumber});

    const AuthStateData first_qr{AuthState::WaitOtherDeviceConfirmation,
                                 AuthWaitOtherDeviceConfirmation{.link = "tg://login?token=same"}};
    fake.runtime->push_update(fake.first, {}, first_qr);
    fake.runtime->push_update(fake.first, {}, first_qr);
    fake.runtime->push_update(
        fake.first, {},
        AuthStateData{AuthState::WaitOtherDeviceConfirmation,
                      AuthWaitOtherDeviceConfirmation{.link = "tg://login?token=replacement"}});

    REQUIRE(eventually([&] {
        const std::lock_guard<std::mutex> lock(seen_mutex);
        return seen.size() == 3;
    }));
    {
        const std::lock_guard<std::mutex> lock(seen_mutex);
        CHECK(seen[0]->auth_sequence == 2);
        CHECK(seen[1]->auth_sequence == 3);
        CHECK(seen[2]->auth_sequence == 4);
        CHECK(std::get<AuthWaitOtherDeviceConfirmation>(seen[0]->data.metadata).link ==
              "tg://login?token=same");
        CHECK(std::get<AuthWaitOtherDeviceConfirmation>(seen[1]->data.metadata).link ==
              "tg://login?token=same");
        CHECK(std::get<AuthWaitOtherDeviceConfirmation>(seen[2]->data.metadata).link ==
              "tg://login?token=replacement");
    }
    fake.client->unsubscribe_auth_states(subscription);
}

TEST_CASE("reused query ids cannot be satisfied by late old-generation traffic",
          "[core][td-runtime]") {
    auto fake = make_fake_client();
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 1; }));
    const auto old_snapshot = fake.client->auth_state();

    auto old_future = send_test_read(*fake.client);
    REQUIRE(fake.runtime->wait_for_sent(2));
    CHECK(fake.runtime->sent_functions()[1].query_id == 2);

    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::LoggingOut});
    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Closing});
    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Closed});
    REQUIRE(fake.runtime->wait_for_clients(2));
    REQUIRE(old_future.wait_for(0ms) == std::future_status::ready);
    CHECK_THROWS_AS(old_future.get(), std::runtime_error);

    const auto second = fake.runtime->clients()[1];
    REQUIRE(fake.runtime->wait_for_sent(3));
    const auto replacement_bootstrap = fake.runtime->sent_functions()[2];
    CHECK(replacement_bootstrap.client_id == second.client_id);
    CHECK(replacement_bootstrap.client_generation == 2);
    CHECK(replacement_bootstrap.query_id == 1);
    CHECK(replacement_bootstrap.function.has_type("getAuthorizationState"));

    fake.runtime->push_response(second, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] {
        const auto snapshot = fake.client->auth_state();
        return snapshot->client_generation == 2 && snapshot->auth_sequence == 1;
    }));
    const auto sent_before_stale = fake.runtime->sent_functions().size();
    auto stale = fake.client->send_read(
        old_snapshot, TdFunctionKind::GetOption,
        TdValue::scripted_function(tgcli::core::TdFunctionData{TdFunctionKind::GetOption}));
    REQUIRE(stale.wait_for(0ms) == std::future_status::ready);
    CHECK_THROWS_AS(stale.get(), tgcli::core::TdAuthorizationError);
    CHECK(fake.runtime->sent_functions().size() == sent_before_stale);
    auto new_future = send_test_read(*fake.client);
    REQUIRE(fake.runtime->wait_for_sent(4));
    CHECK(fake.runtime->sent_functions()[3].query_id == 2);

    fake.runtime->push_response(fake.first, 2, TdValue::from(std::string("old")));
    fake.runtime->push_update(fake.first, {},
                              AuthStateData{AuthState::WaitOtherDeviceConfirmation,
                                            AuthWaitOtherDeviceConfirmation{.link = "stale"}});
    CHECK(new_future.wait_for(20ms) == std::future_status::timeout);
    CHECK(fake.client->auth_state()->client_generation == 2);
    CHECK(fake.client->auth_state()->data.state == AuthState::Ready);

    fake.runtime->push_response(second, 2, TdValue::from(std::string("new")));
    auto response = new_future.get();
    REQUIRE(response.get_if<std::string>() != nullptr);
    CHECK(*response.get_if<std::string>() == "new");
}

TEST_CASE("pre-bootstrap sends are rejected without reserving or emitting a query",
          "[core][td-runtime][safety]") {
    auto fake = make_fake_client();
    auto queued = send_test_read(*fake.client);
    std::this_thread::sleep_for(20ms);
    REQUIRE(fake.runtime->sent_functions().size() == 1);
    REQUIRE(queued.wait_for(0ms) == std::future_status::ready);
    CHECK_THROWS_AS(queued.get(), tgcli::core::TdAuthorizationError);

    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Closed});
    REQUIRE(fake.runtime->wait_for_clients(2));
    REQUIRE(fake.runtime->wait_for_sent(2));

    const auto sent = fake.runtime->sent_functions();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].client_generation == 2);
    CHECK(sent[1].query_id == 1);
    CHECK(sent[1].function.has_type("getAuthorizationState"));
}

TEST_CASE("authorization publication cannot cross an admitted runtime send",
          "[core][td-runtime][race][safety]") {
    auto fake = make_fake_client();
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 1; }));

    std::promise<void> send_entered;
    auto send_entered_future = send_entered.get_future();
    std::promise<void> release_send;
    auto release_send_future = release_send.get_future().share();
    fake.runtime->set_before_send([&](const tgcli::core::TdFunctionData& function) {
        if (function.kind() == TdFunctionKind::GetOption) {
            send_entered.set_value();
            release_send_future.wait();
        }
    });

    std::future<TdValue> response;
    std::thread sender([&] { response = send_test_read(*fake.client); });
    REQUIRE(send_entered_future.wait_for(2s) == std::future_status::ready);
    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::WaitPhoneNumber});
    std::this_thread::sleep_for(20ms);
    CHECK(fake.client->auth_state()->auth_sequence == 1);
    CHECK(fake.client->auth_state()->data.state == AuthState::Ready);

    release_send.set_value();
    sender.join();
    REQUIRE(fake.runtime->wait_for_sent(2));
    REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 2; }));
    fake.runtime->set_before_send({});
    fake.runtime->push_response(fake.first, 2, TdValue::from(std::string("ok")));
    CHECK(*response.get().get_if<std::string>() == "ok");
}

TEST_CASE("unsupported initial state cannot repeat query 1 and still admits close query 2",
          "[core][td-runtime][lifecycle][safety]") {
    auto fake = make_fake_client(false);
    fake.runtime->push_response(fake.first, 1, {},
                                AuthStateData{AuthState::Unknown, {}, 0x7fffffff});
    REQUIRE(eventually([&] { return fake.client->auth_state()->auth_sequence == 1; }));
    const auto snapshot = fake.client->auth_state();
    auto repeated = fake.client->send(
        TdSendDescriptor{.function = TdFunctionKind::GetAuthorizationState,
                         .tier = tgcli::core::DescriptorKind::AuthBootstrap,
                         .owner = {tgcli::core::TdOwnerKind::InternalAuth, 999999},
                         .client_generation = snapshot->client_generation,
                         .auth_sequence = snapshot->auth_sequence,
                         .auth_state = snapshot->data.state},
        TdValue::scripted_function(
            tgcli::core::TdFunctionData{TdFunctionKind::GetAuthorizationState}));
    REQUIRE(repeated.wait_for(0ms) == std::future_status::ready);
    CHECK_THROWS_AS(repeated.get(), tgcli::core::TdAuthorizationError);
    CHECK(fake.runtime->sent_functions().size() == 1);

    auto closing = std::async(std::launch::async, [&] { fake.client->close(); });
    REQUIRE(fake.runtime->wait_for_sent(2));
    const auto sent = fake.runtime->sent_functions();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].query_id == 2);
    CHECK(sent[1].function.kind() == TdFunctionKind::Close);
    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Closed});
    CHECK(closing.wait_for(2s) == std::future_status::ready);
}

TEST_CASE("Closed replaces a logout generation once but daemon shutdown never respawns",
          "[core][td-runtime]") {
    auto fake = make_fake_client();
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return fake.client->auth_state()->data.state == AuthState::Ready; }));

    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::LoggingOut});
    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Closing});
    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Closed});
    REQUIRE(fake.runtime->wait_for_clients(2));
    REQUIRE(fake.runtime->wait_for_sent(2));
    std::this_thread::sleep_for(20ms);
    CHECK(fake.runtime->clients().size() == 2);

    const auto replacement = fake.runtime->clients()[1];
    const auto sent = fake.runtime->sent_functions();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].client_id == replacement.client_id);
    CHECK(sent[1].query_id == 1);
    CHECK(sent[1].function.has_type("getAuthorizationState"));

    fake.runtime->push_response(replacement, 1, {}, AuthStateData{AuthState::WaitPhoneNumber});
    REQUIRE(eventually([&] { return fake.client->auth_state()->client_generation == 2; }));
    fake.client->close();
    std::this_thread::sleep_for(20ms);
    CHECK(fake.runtime->clients().size() == 2);
}

TEST_CASE("Closed closes request admission before auth-state callbacks run",
          "[core][td-runtime][lifecycle]") {
    auto fake = make_fake_client();
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return fake.client->auth_state()->data.state == AuthState::Ready; }));
    auto unresolved_response = send_test_read(*fake.client);
    REQUIRE(fake.runtime->wait_for_sent(2));

    std::promise<void> callback_entered;
    auto callback_entered_future = callback_entered.get_future();
    std::promise<void> release_callback;
    auto release_callback_future = release_callback.get_future().share();
    bool unresolved_was_pending_in_callback = false;
    const auto subscription = fake.client->subscribe_auth_states(
        [&](const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
            if (snapshot->data.state != AuthState::Closed) {
                return;
            }
            unresolved_was_pending_in_callback =
                unresolved_response.wait_for(0ms) == std::future_status::timeout;
            callback_entered.set_value();
            release_callback_future.wait();
        });

    const auto sent_before_closed = fake.runtime->sent_functions().size();
    fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Closed});
    const bool callback_observed =
        callback_entered_future.wait_for(2s) == std::future_status::ready;

    auto response = send_test_read(*fake.client);
    const bool response_ready = response.wait_for(0ms) == std::future_status::ready;
    bool response_failed = false;
    if (response_ready) {
        try {
            static_cast<void>(response.get());
        } catch (const std::runtime_error&) {
            response_failed = true;
        }
    }
    const auto sent_while_callback_blocked = fake.runtime->sent_functions().size();

    release_callback.set_value();
    const bool replacement_created = fake.runtime->wait_for_clients(2);
    const bool unresolved_response_ready =
        unresolved_response.wait_for(2s) == std::future_status::ready;
    bool unresolved_response_failed = false;
    if (unresolved_response_ready) {
        try {
            static_cast<void>(unresolved_response.get());
        } catch (const std::runtime_error&) {
            unresolved_response_failed = true;
        }
    }
    fake.client->unsubscribe_auth_states(subscription);

    REQUIRE(callback_observed);
    CHECK(unresolved_was_pending_in_callback);
    CHECK(response_ready);
    CHECK(response_failed);
    CHECK(sent_while_callback_blocked == sent_before_closed);
    CHECK(unresolved_response_ready);
    CHECK(unresolved_response_failed);
    CHECK(replacement_created);
}

TEST_CASE("intentional close waits for the initial auth snapshot before sending close",
          "[core][td-runtime][lifecycle]") {
    SECTION("a non-Closed initial state admits close as query 2") {
        auto fake = make_fake_client(false);
        std::promise<void> close_started;
        auto close_started_future = close_started.get_future();
        auto close_future = std::async(std::launch::async, [&] {
            close_started.set_value();
            fake.client->close();
        });
        REQUIRE(close_started_future.wait_for(2s) == std::future_status::ready);
        const bool close_was_early = fake.runtime->wait_for_sent(2, 100ms);

        fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
        const bool close_was_sent = fake.runtime->wait_for_sent(2);
        const auto sent = fake.runtime->sent_functions();
        fake.runtime->push_update(fake.first, {}, AuthStateData{AuthState::Closed});
        const bool close_completed = close_future.wait_for(2s) == std::future_status::ready;

        CHECK_FALSE(close_was_early);
        REQUIRE(close_was_sent);
        REQUIRE(sent.size() == 2);
        CHECK(sent[0].query_id == 1);
        CHECK(sent[0].function.has_type("getAuthorizationState"));
        CHECK(sent[1].query_id == 2);
        CHECK(sent[1].function.has_type("close"));
        CHECK(close_completed);
    }

    SECTION("an initial Closed state completes shutdown without emitting close") {
        auto fake = make_fake_client(false);
        std::promise<void> close_started;
        auto close_started_future = close_started.get_future();
        auto close_future = std::async(std::launch::async, [&] {
            close_started.set_value();
            fake.client->close();
        });
        REQUIRE(close_started_future.wait_for(2s) == std::future_status::ready);
        const bool close_was_early = fake.runtime->wait_for_sent(2, 100ms);

        fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Closed});
        const bool close_completed = close_future.wait_for(2s) == std::future_status::ready;
        const auto sent = fake.runtime->sent_functions();

        CHECK_FALSE(close_was_early);
        CHECK(close_completed);
        REQUIRE(sent.size() == 1);
        CHECK(sent.front().query_id == 1);
        CHECK(sent.front().function.has_type("getAuthorizationState"));
    }
}

TEST_CASE("fake-boundary send and close race resolves every future exactly once",
          "[core][td-runtime][lifecycle]") {
    for (int iteration = 0; iteration < 32; ++iteration) {
        auto fake = make_fake_client();
        fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
        REQUIRE(
            eventually([&] { return fake.client->auth_state()->data.state == AuthState::Ready; }));

        std::future<TdValue> response;
        std::barrier start(3);
        std::thread sender([&] {
            start.arrive_and_wait();
            response = send_test_read(*fake.client);
        });
        std::thread closer([&] {
            start.arrive_and_wait();
            fake.client->close();
        });
        start.arrive_and_wait();
        sender.join();
        closer.join();

        REQUIRE(response.valid());
        REQUIRE(response.wait_for(0ms) == std::future_status::ready);
        CHECK_THROWS_AS(response.get(), std::runtime_error);
    }
}
