// Exercises the production generated-TDLib-to-neutral conversion boundary.
// Tagged [tdlib] because this translation unit includes generated TDLib types.

#include "core/td_runtime_test_adapter.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

class UnsupportedAuthorizationState final : public td_api::AuthorizationState {
  public:
    static constexpr std::int32_t ID = 700'000'001;
    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }
    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

class UnsupportedCodeType final : public td_api::AuthenticationCodeType {
  public:
    static constexpr std::int32_t ID = 700'000'002;
    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }
    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

class UnsupportedEmailResetState final : public td_api::EmailAddressResetState {
  public:
    static constexpr std::int32_t ID = 700'000'003;
    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }
    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

AuthStateData convert(NativeObjectPtr native, bool authorization_state_response = true) {
    const auto erased = TdValue::from(std::move(native));
    const auto converted = detail::convert_production_authorization_state_for_test(
        erased, authorization_state_response);
    REQUIRE(converted.has_value());
    return *converted;
}

void check_delivery(td_api::object_ptr<td_api::AuthenticationCodeType> type,
                    AuthCodeDelivery expected_type, std::optional<std::int32_t> expected_length) {
    td_api::object_ptr<td_api::AuthenticationCodeType> no_next;
    NativeObjectPtr native = td_api::make_object<td_api::authorizationStateWaitCode>(
        td_api::make_object<td_api::authenticationCodeInfo>("not-retained", std::move(type),
                                                            std::move(no_next), 17));
    const auto data = convert(std::move(native));
    REQUIRE(data.state == AuthState::WaitCode);
    const auto& metadata = std::get<AuthWaitCode>(data.metadata);
    CHECK(metadata.delivery.type == expected_type);
    CHECK(metadata.delivery.expected_length == expected_length);
    CHECK_FALSE(metadata.delivery.unsupported_tdlib_type_id.has_value());
    CHECK_FALSE(metadata.next_delivery.has_value());
    CHECK(metadata.resend_timeout == 17);
}

} // namespace

TEST_CASE("production converter covers all pinned authorization states and metadata",
          "[core][tdlib][td-runtime-converter]") {
    std::vector<std::pair<NativeObjectPtr, AuthStateData>> cases;
    cases.emplace_back(td_api::make_object<td_api::authorizationStateWaitTdlibParameters>(),
                       AuthStateData{AuthState::WaitTdlibParameters});
    cases.emplace_back(td_api::make_object<td_api::authorizationStateWaitPhoneNumber>(),
                       AuthStateData{AuthState::WaitPhoneNumber});
    cases.emplace_back(
        td_api::make_object<td_api::authorizationStateWaitPremiumPurchase>(
            "premium_12m", 365, "support@example.test", "purchase"),
        AuthStateData{AuthState::WaitPremiumPurchase,
                      AuthWaitPremiumPurchase{.store_product_id = "premium_12m",
                                              .premium_day_count = 365,
                                              .support_email_address = "support@example.test",
                                              .support_email_subject = "purchase"}});
    cases.emplace_back(
        td_api::make_object<td_api::authorizationStateWaitEmailAddress>(true, false),
        AuthStateData{AuthState::WaitEmailAddress,
                      AuthWaitEmailAddress{.allow_apple_id = true, .allow_google_id = false}});
    cases.emplace_back(
        td_api::make_object<td_api::authorizationStateWaitEmailCode>(
            false, true,
            td_api::make_object<td_api::emailAddressAuthenticationCodeInfo>("a***@example.test", 6),
            td_api::make_object<td_api::emailAddressResetStatePending>(42)),
        AuthStateData{AuthState::WaitEmailCode,
                      AuthWaitEmailCode{.allow_apple_id = false,
                                        .allow_google_id = true,
                                        .email_address_pattern = "a***@example.test",
                                        .expected_length = 6,
                                        .reset_state = AuthEmailResetState::Pending,
                                        .reset_delay = 42,
                                        .unsupported_reset_tdlib_type_id = std::nullopt}});
    cases.emplace_back(
        td_api::make_object<td_api::authorizationStateWaitCode>(
            td_api::make_object<td_api::authenticationCodeInfo>(
                "+10000000000", td_api::make_object<td_api::authenticationCodeTypeSms>(5),
                td_api::make_object<td_api::authenticationCodeTypeCall>(6), 30)),
        AuthStateData{
            AuthState::WaitCode,
            AuthWaitCode{
                .delivery = AuthCodeDeliveryInfo{.type = AuthCodeDelivery::Sms,
                                                 .expected_length = 5,
                                                 .unsupported_tdlib_type_id = std::nullopt},
                .next_delivery = AuthCodeDeliveryInfo{.type = AuthCodeDelivery::Call,
                                                      .expected_length = 6,
                                                      .unsupported_tdlib_type_id = std::nullopt},
                .resend_timeout = 30}});
    cases.emplace_back(
        td_api::make_object<td_api::authorizationStateWaitOtherDeviceConfirmation>(
            "tg://login?token=one"),
        AuthStateData{AuthState::WaitOtherDeviceConfirmation,
                      AuthWaitOtherDeviceConfirmation{.link = "tg://login?token=one"}});
    cases.emplace_back(
        td_api::make_object<td_api::authorizationStateWaitRegistration>(
            td_api::make_object<td_api::termsOfService>(
                td_api::make_object<td_api::formattedText>(
                    "terms", std::vector<td_api::object_ptr<td_api::textEntity>>{}),
                16, true)),
        AuthStateData{AuthState::WaitRegistration, AuthWaitRegistration{.terms_text = "terms",
                                                                        .minimum_user_age = 16,
                                                                        .show_popup = true}});
    cases.emplace_back(
        td_api::make_object<td_api::authorizationStateWaitPassword>("hint", true, true,
                                                                    "r***@example.test"),
        AuthStateData{AuthState::WaitPassword,
                      AuthWaitPassword{.hint = "hint",
                                       .has_recovery_email_address = true,
                                       .has_passport_data = true,
                                       .recovery_email_address_pattern = "r***@example.test"}});
    cases.emplace_back(td_api::make_object<td_api::authorizationStateReady>(),
                       AuthStateData{AuthState::Ready});
    cases.emplace_back(td_api::make_object<td_api::authorizationStateLoggingOut>(),
                       AuthStateData{AuthState::LoggingOut});
    cases.emplace_back(td_api::make_object<td_api::authorizationStateClosing>(),
                       AuthStateData{AuthState::Closing});
    cases.emplace_back(td_api::make_object<td_api::authorizationStateClosed>(),
                       AuthStateData{AuthState::Closed});

    REQUIRE(cases.size() == 13);
    for (std::size_t index = 0; index < cases.size(); ++index) {
        CAPTURE(index);
        CHECK(convert(std::move(cases[index].first)) == cases[index].second);
    }
}

TEST_CASE("production converter covers every pinned code-delivery variant",
          "[core][tdlib][td-runtime-converter]") {
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeTelegramMessage>(6),
                   AuthCodeDelivery::TelegramMessage, 6);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeSms>(5), AuthCodeDelivery::Sms,
                   5);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeSmsWord>("A"),
                   AuthCodeDelivery::SmsWord, std::nullopt);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeSmsPhrase>("alpha"),
                   AuthCodeDelivery::SmsPhrase, std::nullopt);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeCall>(4),
                   AuthCodeDelivery::Call, 4);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeFlashCall>("123*"),
                   AuthCodeDelivery::FlashCall, std::nullopt);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeMissedCall>("+123", 7),
                   AuthCodeDelivery::MissedCall, 7);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeFragment>("https://t.me", 8),
                   AuthCodeDelivery::Fragment, 8);
    check_delivery(td_api::make_object<td_api::authenticationCodeTypeFirebaseAndroid>(nullptr, 9),
                   AuthCodeDelivery::FirebaseAndroid, 9);
    check_delivery(
        td_api::make_object<td_api::authenticationCodeTypeFirebaseIos>("receipt", 11, 10),
        AuthCodeDelivery::FirebaseIos, 10);
}

TEST_CASE("production converter preserves nulls and unsupported generated type ids",
          "[core][tdlib][td-runtime-converter]") {
    SECTION("null optional metadata remains neutral") {
        NativeObjectPtr wait_code =
            td_api::make_object<td_api::authorizationStateWaitCode>(nullptr);
        const auto code_data = convert(std::move(wait_code));
        const auto& code = std::get<AuthWaitCode>(code_data.metadata);
        CHECK(code.delivery.type == AuthCodeDelivery::Unknown);
        CHECK_FALSE(code.delivery.expected_length.has_value());
        CHECK_FALSE(code.delivery.unsupported_tdlib_type_id.has_value());
        CHECK_FALSE(code.next_delivery.has_value());
        CHECK(code.resend_timeout == 0);

        td_api::object_ptr<td_api::AuthenticationCodeType> no_delivery;
        td_api::object_ptr<td_api::AuthenticationCodeType> no_next_delivery;
        NativeObjectPtr wait_code_without_types =
            td_api::make_object<td_api::authorizationStateWaitCode>(
                td_api::make_object<td_api::authenticationCodeInfo>(
                    "not-retained", std::move(no_delivery), std::move(no_next_delivery), 23));
        const auto code_without_types_data = convert(std::move(wait_code_without_types));
        const auto& code_without_types = std::get<AuthWaitCode>(code_without_types_data.metadata);
        CHECK(code_without_types.delivery.type == AuthCodeDelivery::Unknown);
        CHECK_FALSE(code_without_types.delivery.unsupported_tdlib_type_id.has_value());
        CHECK_FALSE(code_without_types.next_delivery.has_value());
        CHECK(code_without_types.resend_timeout == 23);

        NativeObjectPtr wait_email = td_api::make_object<td_api::authorizationStateWaitEmailCode>(
            false, false, nullptr, nullptr);
        const auto email_data = convert(std::move(wait_email));
        const auto& email = std::get<AuthWaitEmailCode>(email_data.metadata);
        CHECK(email.email_address_pattern.empty());
        CHECK(email.expected_length == 0);
        CHECK(email.reset_state == AuthEmailResetState::None);
        CHECK_FALSE(email.unsupported_reset_tdlib_type_id.has_value());

        const auto registration_data =
            convert(td_api::make_object<td_api::authorizationStateWaitRegistration>(nullptr));
        CHECK(registration_data.metadata == AuthStateMetadata{AuthWaitRegistration{}});
    }

    SECTION("both email-reset variants retain their delay") {
        NativeObjectPtr wait_email = td_api::make_object<td_api::authorizationStateWaitEmailCode>(
            true, true, nullptr, td_api::make_object<td_api::emailAddressResetStateAvailable>(73));
        const auto email_data = convert(std::move(wait_email));
        const auto& email = std::get<AuthWaitEmailCode>(email_data.metadata);
        CHECK(email.reset_state == AuthEmailResetState::Available);
        CHECK(email.reset_delay == 73);
        CHECK_FALSE(email.unsupported_reset_tdlib_type_id.has_value());
    }

    SECTION("unsupported top-level and nested ids are retained") {
        NativeObjectPtr unsupported_state = td_api::make_object<UnsupportedAuthorizationState>();
        const auto top_level = convert(std::move(unsupported_state));
        CHECK(top_level.state == AuthState::Unknown);
        CHECK(top_level.unsupported_tdlib_type_id == UnsupportedAuthorizationState::ID);

        td_api::object_ptr<td_api::AuthenticationCodeType> unsupported_code =
            td_api::make_object<UnsupportedCodeType>();
        NativeObjectPtr wait_code = td_api::make_object<td_api::authorizationStateWaitCode>(
            td_api::make_object<td_api::authenticationCodeInfo>(
                "not-retained", std::move(unsupported_code), nullptr, 0));
        const auto code_data = convert(std::move(wait_code));
        const auto& code = std::get<AuthWaitCode>(code_data.metadata);
        CHECK(code.delivery.type == AuthCodeDelivery::Unknown);
        CHECK(code.delivery.unsupported_tdlib_type_id == UnsupportedCodeType::ID);

        td_api::object_ptr<td_api::EmailAddressResetState> unsupported_reset =
            td_api::make_object<UnsupportedEmailResetState>();
        NativeObjectPtr wait_email = td_api::make_object<td_api::authorizationStateWaitEmailCode>(
            false, false, nullptr, std::move(unsupported_reset));
        const auto email_data = convert(std::move(wait_email));
        const auto& email = std::get<AuthWaitEmailCode>(email_data.metadata);
        CHECK(email.reset_state == AuthEmailResetState::Unknown);
        CHECK(email.unsupported_reset_tdlib_type_id == UnsupportedEmailResetState::ID);
    }

    SECTION("updateAuthorizationState uses the same production converter") {
        const auto update_data =
            convert(td_api::make_object<td_api::updateAuthorizationState>(
                        td_api::make_object<td_api::authorizationStateReady>()),
                    false);
        CHECK(update_data == AuthStateData{AuthState::Ready});
    }
}
