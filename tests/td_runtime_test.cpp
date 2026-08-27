#include "core/td_client.hpp"
#include "support/scripted_td_runtime.hpp"
#include "support/td_client_test_access.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
using tgcli::core::TdAuthorizationError;
using tgcli::core::TdClient;
using tgcli::core::TdFunctionData;
using tgcli::core::TdFunctionField;
using tgcli::core::TdFunctionKind;
using tgcli::core::TdOk;
using tgcli::core::TdSendDescriptor;
using tgcli::core::TdSession;
using tgcli::core::TdSessionDeviceType;
using tgcli::core::TdSessions;
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

FakeClient make_fake_client(bool close_automatically = true,
                            const tgcli::core::TdLogConfiguration& logging = {}) {
    auto runtime = std::make_unique<ScriptedTdRuntime>(close_automatically);
    auto* runtime_ptr = runtime.get();
    auto client = std::make_unique<TdClient>(std::move(runtime), logging);
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

const tgcli::core::TdFieldValue* field(const TdFunctionData& function, std::string_view name) {
    for (const TdFunctionField& candidate : function.fields()) {
        if (candidate.has_name(name)) {
            return &candidate.value();
        }
    }
    return nullptr;
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

TEST_CASE("scripted session factories mirror exact dormant descriptors",
          "[core][td-runtime][session][fake-boundary]") {
    ScriptedTdRuntime runtime;
    auto list = runtime.make_get_active_sessions();
    REQUIRE(list.function_data().has_value());
    CHECK(list.function_data()->kind() == TdFunctionKind::GetActiveSessions);
    CHECK(list.function_data()->fields().empty());

    for (const auto id : {std::numeric_limits<std::int64_t>::min(), std::int64_t{0},
                          std::numeric_limits<std::int64_t>::max()}) {
        CAPTURE(id);
        auto terminate = runtime.make_terminate_session(id);
        REQUIRE(terminate.function_data().has_value());
        CHECK(terminate.function_data()->kind() == TdFunctionKind::TerminateSession);
        REQUIRE(terminate.function_data()->fields().size() == 1);
        const auto* session_id = field(*terminate.function_data(), "session_id");
        REQUIRE(session_id != nullptr);
        CHECK(std::get<std::int64_t>(*session_id) == id);
    }
}

TEST_CASE("TdClient exposes only the dormant session read seam",
          "[core][td-runtime][session][fake-boundary]") {
    auto fake = make_fake_client();
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return fake.client->auth_state()->data.state == AuthState::Ready; }));

    auto response = fake.client->get_active_sessions(fake.client->auth_state());
    REQUIRE(fake.runtime->wait_for_sent(2));
    const auto sent = fake.runtime->sent_functions();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].function.kind() == TdFunctionKind::GetActiveSessions);
    CHECK(sent[1].function.fields().empty());

    const TdSessions expected{
        .items =
            {
                TdSession{
                    .id = "-9223372036854775808",
                    .is_current = true,
                    .is_password_pending = false,
                    .is_unconfirmed = true,
                    .can_accept_secret_chats = false,
                    .can_accept_calls = true,
                    .device_type = TdSessionDeviceType::Linux,
                    .api_id = -1001,
                    .application_name = "tgcli alpha 🧪",
                    .application_version = "1.8.65-a",
                    .is_official_application = false,
                    .device_model = "workstation-a",
                    .platform = "Linux-a",
                    .system_version = "6.8-a",
                    .log_in_date = "1970-01-01T00:00:01Z",
                    .last_active_date = "2038-01-19T03:14:07Z",
                    .ip_address = "203.0.113.7",
                    .location = "Athens-a",
                },
                TdSession{
                    .id = "9223372036854775807",
                    .is_current = false,
                    .is_password_pending = true,
                    .is_unconfirmed = false,
                    .can_accept_secret_chats = true,
                    .can_accept_calls = false,
                    .device_type = TdSessionDeviceType::Xbox,
                    .api_id = 2002,
                    .application_name = "tgcli beta",
                    .application_version = "1.8.65-b",
                    .is_official_application = true,
                    .device_model = "console-b",
                    .platform = "Xbox-b",
                    .system_version = "10.0-b",
                    .log_in_date = "2000-02-29T12:34:56Z",
                    .last_active_date = "2100-12-31T23:59:59Z",
                    .ip_address = "2001:db8::2",
                    .location = "Thessaloniki-b",
                },
            },
        .inactive_session_ttl_days = 366,
    };
    fake.runtime->push_response(fake.first, sent[1].query_id, TdValue::from(expected));
    REQUIRE(response.wait_for(2s) == std::future_status::ready);
    const auto value = response.get();
    const auto* sessions = value.get_if<TdSessions>();
    REQUIRE(sessions != nullptr);
    REQUIRE(sessions->items.size() == 2);
    CHECK(sessions->items[0] == expected.items[0]);
    CHECK(sessions->items[1] == expected.items[1]);
    CHECK(sessions->inactive_session_ttl_days == expected.inactive_session_ttl_days);
    CHECK(*sessions == expected);
    CHECK(std::ranges::none_of(fake.runtime->sent_functions(), [](const auto& request) {
        return request.function.kind() == TdFunctionKind::TerminateSession;
    }));
}

TEST_CASE("TdClient authorizes both message-read functions only as Ready reads",
          "[core][td-runtime][msg][authorization][fake-boundary]") {
    auto fake = make_fake_client();
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return fake.client->auth_state()->data.state == AuthState::Ready; }));

    auto messages = fake.client->get_messages(fake.client->auth_state(), -1001, {123, 123, 124});
    REQUIRE(fake.runtime->wait_for_sent(2));
    auto sent = fake.runtime->sent_functions();
    REQUIRE(sent.size() == 2);
    CHECK(sent.back().function.kind() == TdFunctionKind::GetMessages);
    fake.runtime->push_response(fake.first, sent.back().query_id,
                                TdValue::from(tgcli::core::TdMessages{
                                    .messages = {std::nullopt, std::nullopt, std::nullopt}}));
    REQUIRE(messages.wait_for(2s) == std::future_status::ready);
    CHECK(messages.get().get_if<tgcli::core::TdMessages>() != nullptr);

    auto link = fake.client->get_message_link(fake.client->auth_state(), -1001, 123, 0, 0, "",
                                              false, false);
    REQUIRE(fake.runtime->wait_for_sent(3));
    sent = fake.runtime->sent_functions();
    REQUIRE(sent.size() == 3);
    CHECK(sent.back().function.kind() == TdFunctionKind::GetMessageLink);
    fake.runtime->push_response(fake.first, sent.back().query_id,
                                TdValue::from(tgcli::core::TdMessageLink{
                                    .link = "urn:telegram:message", .is_public = false}));
    REQUIRE(link.wait_for(2s) == std::future_status::ready);
    CHECK(link.get().get_if<tgcli::core::TdMessageLink>() != nullptr);
}

TEST_CASE("TdClient authorizes all seven read-history functions only as Ready reads",
          "[core][td-runtime][read][authorization][fake-boundary]") {
    auto fake = make_fake_client();
    fake.runtime->push_response(fake.first, 1, {}, AuthStateData{AuthState::Ready});
    REQUIRE(eventually([&] { return fake.client->auth_state()->data.state == AuthState::Ready; }));

    std::size_t expected_count = 2;
    const auto admitted = [&](TdFunctionKind expected, std::future<TdValue> response) {
        REQUIRE(fake.runtime->wait_for_sent(expected_count));
        const auto sent = fake.runtime->sent_functions();
        REQUIRE(sent.size() == expected_count);
        CHECK(sent.back().function.kind() == expected);
        fake.runtime->push_response(fake.first, sent.back().query_id, TdValue::from(TdOk{}));
        REQUIRE(response.wait_for(2s) == std::future_status::ready);
        CHECK(response.get().get_if<TdOk>() != nullptr);
        ++expected_count;
    };

    admitted(TdFunctionKind::GetChatHistory,
             fake.client->get_chat_history(fake.client->auth_state(), -1001, 123, 0, 21, true));
    admitted(TdFunctionKind::GetChatMessageByDate,
             fake.client->get_chat_message_by_date(fake.client->auth_state(), -1001, -17));
    admitted(TdFunctionKind::GetMessageThread,
             fake.client->get_message_thread(fake.client->auth_state(), -1001, 500));
    admitted(TdFunctionKind::GetForumTopicHistory,
             fake.client->get_forum_topic_history(fake.client->auth_state(), -1001, 7, 123, 0, 20));
    admitted(
        TdFunctionKind::GetMessageThreadHistory,
        fake.client->get_message_thread_history(fake.client->auth_state(), -1001, 500, 123, 0, 20));
    admitted(TdFunctionKind::GetDirectMessagesChatTopicHistory,
             fake.client->get_direct_messages_chat_topic_history(fake.client->auth_state(), -1001,
                                                                 600, 123, 0, 20));
    admitted(
        TdFunctionKind::GetSavedMessagesTopicHistory,
        fake.client->get_saved_messages_topic_history(fake.client->auth_state(), 700, 123, 0, 20));

    auto denied = make_fake_client();
    denied.runtime->push_response(denied.first, 1, {}, AuthStateData{AuthState::WaitPhoneNumber});
    REQUIRE(eventually(
        [&] { return denied.client->auth_state()->data.state == AuthState::WaitPhoneNumber; }));
    auto response =
        denied.client->get_chat_history(denied.client->auth_state(), -1001, 0, 0, 20, false);
    REQUIRE(response.wait_for(2s) == std::future_status::ready);
    CHECK_THROWS_AS(response.get(), TdAuthorizationError);
    CHECK(denied.runtime->sent_functions().size() == 1);
}

TEST_CASE("process logging configuration is frozen before the first client id",
          "[core][td-runtime][logging]") {
    const tgcli::core::TdLogConfiguration logging{
        .file_path = "/state/tgcli/accounts/work/tdlib.log",
        .json_diagnostics = true,
    };
    auto fake = make_fake_client(true, logging);

    CHECK(fake.runtime->initialized_before_first_client());
    CHECK(fake.runtime->logging_configuration() == logging);
    CHECK(fake.runtime->clients().size() == 1);
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
    CHECK(fake.runtime->sent_functions()[1].query_id == 3);

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
    CHECK(fake.runtime->sent_functions()[3].query_id == 3);

    fake.runtime->push_response(fake.first, 3, TdValue::from(std::string("old")));
    fake.runtime->push_update(fake.first, {},
                              AuthStateData{AuthState::WaitOtherDeviceConfirmation,
                                            AuthWaitOtherDeviceConfirmation{.link = "stale"}});
    CHECK(new_future.wait_for(20ms) == std::future_status::timeout);
    CHECK(fake.client->auth_state()->client_generation == 2);
    CHECK(fake.client->auth_state()->data.state == AuthState::Ready);

    fake.runtime->push_response(second, 3, TdValue::from(std::string("new")));
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
    fake.runtime->push_response(fake.first, 3, TdValue::from(std::string("ok")));
    CHECK(*response.get().get_if<std::string>() == "ok");
}

TEST_CASE("unsupported initial state cannot repeat query 1 and still admits close query 3",
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
    CHECK(sent[1].query_id == 3);
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
    SECTION("a non-Closed initial state admits close as query 3") {
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
        CHECK(sent[1].query_id == 3);
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
