// Exercises the production generated-TDLib-to-neutral conversion boundary.
// Tagged [tdlib] because this translation unit includes generated TDLib types.

#include "core/td_runtime_test_adapter.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

template <typename... Types> struct TypeList {};

using PinnedInternalLinkTypes =
    TypeList<td_api::internalLinkTypeAttachmentMenuBot, td_api::internalLinkTypeAuthenticationCode,
             td_api::internalLinkTypeBackground, td_api::internalLinkTypeBotAddToChannel,
             td_api::internalLinkTypeBotStart, td_api::internalLinkTypeBotStartInGroup,
             td_api::internalLinkTypeBusinessChat, td_api::internalLinkTypeCallsPage,
             td_api::internalLinkTypeChatAffiliateProgram, td_api::internalLinkTypeChatBoost,
             td_api::internalLinkTypeChatFolderInvite, td_api::internalLinkTypeChatInvite,
             td_api::internalLinkTypeChatSelection, td_api::internalLinkTypeContactsPage,
             td_api::internalLinkTypeDirectMessagesChat, td_api::internalLinkTypeGame,
             td_api::internalLinkTypeGiftAuction, td_api::internalLinkTypeGiftCollection,
             td_api::internalLinkTypeGroupCall, td_api::internalLinkTypeInstantView,
             td_api::internalLinkTypeInvoice, td_api::internalLinkTypeLanguagePack,
             td_api::internalLinkTypeLiveStory, td_api::internalLinkTypeMainWebApp,
             td_api::internalLinkTypeMessage, td_api::internalLinkTypeMessageDraft,
             td_api::internalLinkTypeMyProfilePage, td_api::internalLinkTypeNewChannelChat,
             td_api::internalLinkTypeNewGroupChat, td_api::internalLinkTypeNewPrivateChat,
             td_api::internalLinkTypeNewStory, td_api::internalLinkTypeOauth,
             td_api::internalLinkTypePassportDataRequest,
             td_api::internalLinkTypePhoneNumberConfirmation,
             td_api::internalLinkTypePremiumFeaturesPage, td_api::internalLinkTypePremiumGiftCode,
             td_api::internalLinkTypePremiumGiftPurchase, td_api::internalLinkTypeProxy,
             td_api::internalLinkTypePublicChat, td_api::internalLinkTypeQrCodeAuthentication,
             td_api::internalLinkTypeRequestManagedBot, td_api::internalLinkTypeRestorePurchases,
             td_api::internalLinkTypeSavedMessages, td_api::internalLinkTypeSearch,
             td_api::internalLinkTypeSettings, td_api::internalLinkTypeStarPurchase,
             td_api::internalLinkTypeStickerSet, td_api::internalLinkTypeStory,
             td_api::internalLinkTypeStoryAlbum, td_api::internalLinkTypeTextCompositionStyle,
             td_api::internalLinkTypeTheme, td_api::internalLinkTypeUnknownDeepLink,
             td_api::internalLinkTypeUpgradedGift, td_api::internalLinkTypeUserPhoneNumber,
             td_api::internalLinkTypeUserToken, td_api::internalLinkTypeVideoChat,
             td_api::internalLinkTypeWebApp>;

template <typename... Types>
std::vector<NativeObjectPtr> make_internal_link_inventory(TypeList<Types...> types) {
    static_cast<void>(types);
    std::vector<NativeObjectPtr> inventory;
    inventory.reserve(sizeof...(Types));
    (inventory.push_back(td_api::make_object<Types>()), ...);
    return inventory;
}

std::optional<TdInternalLinkKind> supported_internal_link_kind(std::int32_t type_id) {
    switch (type_id) {
    case td_api::internalLinkTypePublicChat::ID:
        return TdInternalLinkKind::PublicChat;
    case td_api::internalLinkTypeBotStart::ID:
        return TdInternalLinkKind::BotStart;
    case td_api::internalLinkTypeMessage::ID:
        return TdInternalLinkKind::Message;
    case td_api::internalLinkTypeChatInvite::ID:
        return TdInternalLinkKind::ChatInvite;
    case td_api::internalLinkTypeDirectMessagesChat::ID:
        return TdInternalLinkKind::DirectMessagesChat;
    case td_api::internalLinkTypeSavedMessages::ID:
        return TdInternalLinkKind::SavedMessages;
    default:
        return std::nullopt;
    }
}

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

class UnsupportedReactionType final : public td_api::ReactionType {
  public:
    static constexpr std::int32_t ID = 700'000'004;
    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }
    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

class UnsupportedMessageContent final : public td_api::MessageContent {
  public:
    static constexpr std::int32_t ID = 700'000'005;
    [[nodiscard]] std::int32_t get_id() const final {
        return ID;
    }
    void store(td::TlStorerToString& storer, const char* field_name) const final {
        static_cast<void>(storer);
        static_cast<void>(field_name);
    }
};

class UnsupportedSessionDeviceType final : public td_api::SessionDeviceType {
  public:
    static constexpr std::int32_t ID = 700'000'006;
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

TdValue convert_response(NativeObjectPtr native) {
    return detail::convert_production_response_for_test(TdValue::from(std::move(native)));
}

TdValue convert_sessions(NativeObjectPtr native) {
    return detail::convert_production_sessions_for_test(TdValue::from(std::move(native)));
}

td_api::object_ptr<td_api::session>
session(std::int64_t id, td_api::object_ptr<td_api::SessionDeviceType> device_type =
                             td_api::make_object<td_api::sessionDeviceTypeLinux>()) {
    auto value = td_api::make_object<td_api::session>();
    value->id_ = id;
    value->is_current_ = true;
    value->is_password_pending_ = true;
    value->is_unconfirmed_ = true;
    value->can_accept_secret_chats_ = true;
    value->can_accept_calls_ = false;
    value->device_type_ = std::move(device_type);
    value->api_id_ = -2'147'483'648;
    value->application_name_ = "tgcli 🧪";
    value->application_version_ = "1.2.3";
    value->is_official_application_ = false;
    value->device_model_ = "workstation";
    value->platform_ = "Linux";
    value->system_version_ = "6.8";
    value->log_in_date_ = 1;
    value->last_active_date_ = std::numeric_limits<std::int32_t>::max();
    value->ip_address_ = "203.0.113.7";
    value->location_ = "Athens";
    return value;
}

NativeObjectPtr session_result(std::vector<td_api::object_ptr<td_api::session>> items,
                               std::int32_t ttl = 180) {
    return td_api::make_object<td_api::sessions>(std::move(items), ttl);
}

const TdSessionConversionError& require_session_error(TdValue& converted) {
    CHECK(converted.get_if<TdSessions>() == nullptr);
    const auto* error = converted.get_if<TdSessionConversionError>();
    REQUIRE(error != nullptr);
    return *error;
}

const TdFieldValue* function_field(const TdFunctionData& function, std::string_view name) {
    for (const auto& candidate : function.fields()) {
        if (candidate.has_name(name)) {
            return &candidate.value();
        }
    }
    return nullptr;
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

struct ScriptedLogWrite {
    struct Result {
        ssize_t count = 0;
        int error = 0;
    };

    static ssize_t invoke(void* context, int fd, const void* bytes, std::size_t size) noexcept {
        auto& script = *static_cast<ScriptedLogWrite*>(context);
        if (script.calls >= script.result_count || script.calls >= script.results.size()) {
            errno = EOVERFLOW;
            return -1;
        }
        const auto index = script.calls++;
        script.descriptors.at(index) = fd;
        script.requests.at(index) = static_cast<const char*>(bytes);
        script.request_sizes.at(index) = size;
        const auto result = script.results.at(index);
        errno = result.error;
        if (result.count > 0) {
            const auto count = static_cast<std::size_t>(result.count);
            const auto available = script.output.size() - script.output_size;
            const auto copied = std::min(std::min(count, size), available);
            std::memcpy(script.output.data() + script.output_size, bytes, copied);
            script.output_size += copied;
        }
        return result.count;
    }

    [[nodiscard]] std::string_view request(std::size_t index) const {
        return {requests.at(index), request_sizes.at(index)};
    }

    [[nodiscard]] std::string_view written() const {
        return {output.data(), output_size};
    }

    std::array<Result, 4> results{};
    std::size_t result_count = 0;
    std::size_t calls = 0;
    std::array<int, 4> descriptors{};
    std::array<const char*, 4> requests{};
    std::array<std::size_t, 4> request_sizes{};
    std::array<char, 512> output{};
    std::size_t output_size = 0;
};

} // namespace

TEST_CASE("async TDLib log failure diagnostics remain one sanitized NDJSON record",
          "[core][tdlib][logging]") {
    std::array<int, 2> output_pipe{};
    REQUIRE(::pipe(output_pipe.data()) == 0);
    const int saved_stderr = ::dup(STDERR_FILENO);
    REQUIRE(saved_stderr >= 0);
    REQUIRE(::dup2(output_pipe[1], STDERR_FILENO) == STDERR_FILENO);
    REQUIRE(::close(output_pipe[1]) == 0);
    detail::reset_process_log_failure_for_test(true);
    std::thread reporter([] {
        detail::report_process_log_failure_for_test();
        detail::report_process_log_failure_for_test();
    });
    reporter.join();
    REQUIRE(::dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    REQUIRE(::close(saved_stderr) == 0);
    std::string json_message(512, '\0');
    const auto byte_count = ::read(output_pipe[0], json_message.data(), json_message.size());
    REQUIRE(byte_count > 0);
    json_message.resize(static_cast<std::size_t>(byte_count));
    REQUIRE(::close(output_pipe[0]) == 0);

    CHECK(json_message == detail::process_log_failure_message_for_test(true));
    REQUIRE_FALSE(json_message.empty());
    CHECK(json_message.back() == '\n');
    CHECK(std::count(json_message.begin(), json_message.end(), '\n') == 1);
    const auto parsed = nlohmann::json::parse(json_message);
    CHECK(parsed ==
          nlohmann::json{{"warning", "TDLib log sink failed; further records suppressed"}});
    for (const std::string_view sentinel :
         {"token-secret", "code-secret", "password-secret", "database-key-secret"}) {
        CHECK(json_message.find(sentinel) == std::string_view::npos);
    }

    const auto human_message = detail::process_log_failure_message_for_test(false);
    CHECK(human_message == "warning: TDLib log sink failed; further records suppressed\n");
}

TEST_CASE("async TDLib log failure diagnostics handle best-effort write outcomes",
          "[core][tdlib][logging]") {
    const auto message = detail::process_log_failure_message_for_test(true);

    SECTION("EINTR is retried without changing the incoming errno") {
        ScriptedLogWrite script;
        script.results[0] = {-1, EINTR};
        script.results[1] = {static_cast<ssize_t>(message.size()), 0};
        script.result_count = 2;
        detail::reset_process_log_failure_for_test(true);
        errno = EDOM;

        detail::report_process_log_failure_with_writer_for_test(ScriptedLogWrite::invoke, &script);
        detail::report_process_log_failure_with_writer_for_test(ScriptedLogWrite::invoke, &script);

        CHECK(errno == EDOM);
        REQUIRE(script.calls == 2);
        CHECK(script.descriptors[0] == STDERR_FILENO);
        CHECK(script.descriptors[1] == STDERR_FILENO);
        CHECK(script.request(0) == message);
        CHECK(script.request(1) == message);
        CHECK(script.written() == message);
    }

    SECTION("partial writes advance to the unwritten remainder") {
        ScriptedLogWrite script;
        script.results[0] = {3, 0};
        script.results[1] = {static_cast<ssize_t>(message.size() - 3), 0};
        script.result_count = 2;
        detail::reset_process_log_failure_for_test(true);
        errno = ERANGE;

        detail::report_process_log_failure_with_writer_for_test(ScriptedLogWrite::invoke, &script);

        CHECK(errno == ERANGE);
        REQUIRE(script.calls == 2);
        CHECK(script.request(0) == message);
        CHECK(script.request(1) == message.substr(3));
        CHECK(script.written() == message);
    }

    SECTION("a zero-byte write stops without spinning") {
        ScriptedLogWrite script;
        script.results[0] = {0, 0};
        script.result_count = 1;
        detail::reset_process_log_failure_for_test(true);
        errno = EBUSY;

        detail::report_process_log_failure_with_writer_for_test(ScriptedLogWrite::invoke, &script);
        detail::report_process_log_failure_with_writer_for_test(ScriptedLogWrite::invoke, &script);

        CHECK(errno == EBUSY);
        CHECK(script.calls == 1);
        CHECK(script.written().empty());
    }

    SECTION("a terminal error stops without recursive reporting") {
        ScriptedLogWrite script;
        script.results[0] = {-1, EIO};
        script.result_count = 1;
        detail::reset_process_log_failure_for_test(true);
        errno = ENOTTY;

        detail::report_process_log_failure_with_writer_for_test(ScriptedLogWrite::invoke, &script);
        detail::report_process_log_failure_with_writer_for_test(ScriptedLogWrite::invoke, &script);

        CHECK(errno == ENOTTY);
        CHECK(script.calls == 1);
        CHECK(script.written().empty());
    }
}

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

TEST_CASE("production converter preserves every Saved Messages reaction variant",
          "[core][tdlib][td-runtime-converter][saved]") {
    std::vector<td_api::object_ptr<td_api::savedMessagesTag>> values;
    values.push_back(td_api::make_object<td_api::savedMessagesTag>(
        td_api::make_object<td_api::reactionTypeEmoji>("👩🏽‍💻️"), "work", 7));
    values.push_back(td_api::make_object<td_api::savedMessagesTag>(
        td_api::make_object<td_api::reactionTypeCustomEmoji>(9223372036854775807LL), "", 2));
    values.push_back(td_api::make_object<td_api::savedMessagesTag>(
        td_api::make_object<td_api::reactionTypePaid>(), "paid", 1));
    values.push_back(td_api::make_object<td_api::savedMessagesTag>(
        td_api::make_object<UnsupportedReactionType>(), "unknown", 1));
    values.push_back(td_api::make_object<td_api::savedMessagesTag>(nullptr, "null", 1));
    values.emplace_back();
    NativeObjectPtr native = td_api::make_object<td_api::savedMessagesTags>(std::move(values));
    auto converted = convert_response(std::move(native));
    const auto* tags = converted.get_if<TdSavedMessagesTags>();
    REQUIRE(tags != nullptr);
    REQUIRE(tags->tags.size() == 6);
    CHECK(tags->tags[0] ==
          TdSavedMessagesTag{.tag = {.kind = TdReactionKind::Emoji,
                                     .emoji = "👩🏽‍💻️",
                                     .custom_emoji_id = 0,
                                     .tdlib_type_id = td_api::reactionTypeEmoji::ID},
                             .label = "work",
                             .count = 7});
    CHECK(tags->tags[1].tag.kind == TdReactionKind::CustomEmoji);
    CHECK(tags->tags[1].tag.custom_emoji_id == 9223372036854775807LL);
    CHECK(tags->tags[2].tag.kind == TdReactionKind::Paid);
    CHECK(tags->tags[2].tag.tdlib_type_id == td_api::reactionTypePaid::ID);
    CHECK(tags->tags[3].tag.kind == TdReactionKind::Unknown);
    CHECK(tags->tags[3].tag.tdlib_type_id == UnsupportedReactionType::ID);
    CHECK(tags->tags[4].tag.kind == TdReactionKind::Unknown);
    CHECK(tags->tags[4].tag.tdlib_type_id == 0);
    CHECK(tags->tags[5].tag.kind == TdReactionKind::Unknown);
    CHECK(tags->tags[5].tag.tdlib_type_id == 0);
}

TEST_CASE("production converter gives Saved text and non-text messages the exact summary",
          "[core][tdlib][td-runtime-converter][saved]") {
    auto text_message = td_api::make_object<td_api::message>();
    text_message->id_ = 200;
    text_message->chat_id_ = 42;
    text_message->date_ = 1782993600;
    text_message->content_ = td_api::make_object<td_api::messageText>(
        td_api::make_object<td_api::formattedText>(
            "idea 🧪", std::vector<td_api::object_ptr<td_api::textEntity>>{}),
        nullptr, nullptr);

    auto non_text_message = td_api::make_object<td_api::message>();
    non_text_message->id_ = 199;
    non_text_message->chat_id_ = 42;
    non_text_message->date_ = 1782993540;
    non_text_message->content_ = td_api::make_object<UnsupportedMessageContent>();

    std::vector<td_api::object_ptr<td_api::message>> messages;
    messages.push_back(std::move(text_message));
    messages.push_back(std::move(non_text_message));
    NativeObjectPtr native =
        td_api::make_object<td_api::foundChatMessages>(2, std::move(messages), 190);
    auto converted = convert_response(std::move(native));
    const auto* found = converted.get_if<TdFoundSavedMessages>();
    REQUIRE(found != nullptr);
    CHECK(found->next_from_message_id == 190);
    CHECK(found->messages == std::vector<TdSavedMessageSummary>{
                                 {.id = 200, .chat_id = 42, .date = 1782993600, .text = "idea 🧪"},
                                 {.id = 199, .chat_id = 42, .date = 1782993540, .text = ""}});
}

TEST_CASE("production session factories retain exact descriptors and native types",
          "[core][tdlib][td-runtime-converter][session]") {
    static_assert(
        std::is_same_v<td_api::terminateSession::ReturnType, td_api::object_ptr<td_api::ok>>);

    auto list = detail::make_production_get_active_sessions_for_test();
    REQUIRE(list.function_data().has_value());
    CHECK(list.function_data()->kind() == TdFunctionKind::GetActiveSessions);
    CHECK(list.function_data()->fields().empty());
    CHECK(detail::production_function_matches_for_test(list, TdFunctionKind::GetActiveSessions));
    CHECK_FALSE(
        detail::production_function_matches_for_test(list, TdFunctionKind::TerminateSession));

    for (const auto id : {std::numeric_limits<std::int64_t>::min(), std::int64_t{0},
                          std::numeric_limits<std::int64_t>::max()}) {
        CAPTURE(id);
        auto terminate = detail::make_production_terminate_session_for_test(id);
        REQUIRE(terminate.function_data().has_value());
        CHECK(terminate.function_data()->kind() == TdFunctionKind::TerminateSession);
        REQUIRE(terminate.function_data()->fields().size() == 1);
        const auto* session_id = function_field(*terminate.function_data(), "session_id");
        REQUIRE(session_id != nullptr);
        CHECK(std::get<std::int64_t>(*session_id) == id);
        CHECK(detail::production_function_matches_for_test(terminate,
                                                           TdFunctionKind::TerminateSession));
        CHECK(detail::production_terminate_session_id_for_test(terminate) == id);
        CHECK_FALSE(detail::production_function_matches_for_test(
            terminate, TdFunctionKind::GetActiveSessions));
    }
}

TEST_CASE("production converter covers all pinned session device variants",
          "[core][tdlib][td-runtime-converter][session]") {
    std::vector<std::pair<td_api::object_ptr<td_api::SessionDeviceType>, TdSessionDeviceType>>
        cases;
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeAndroid>(),
                       TdSessionDeviceType::Android);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeApple>(),
                       TdSessionDeviceType::Apple);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeBrave>(),
                       TdSessionDeviceType::Brave);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeChrome>(),
                       TdSessionDeviceType::Chrome);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeEdge>(),
                       TdSessionDeviceType::Edge);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeFirefox>(),
                       TdSessionDeviceType::Firefox);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeIpad>(),
                       TdSessionDeviceType::Ipad);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeIphone>(),
                       TdSessionDeviceType::Iphone);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeLinux>(),
                       TdSessionDeviceType::Linux);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeMac>(),
                       TdSessionDeviceType::Mac);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeOpera>(),
                       TdSessionDeviceType::Opera);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeSafari>(),
                       TdSessionDeviceType::Safari);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeUbuntu>(),
                       TdSessionDeviceType::Ubuntu);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeUnknown>(),
                       TdSessionDeviceType::Unknown);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeVivaldi>(),
                       TdSessionDeviceType::Vivaldi);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeWindows>(),
                       TdSessionDeviceType::Windows);
    cases.emplace_back(td_api::make_object<td_api::sessionDeviceTypeXbox>(),
                       TdSessionDeviceType::Xbox);

    constexpr std::array<std::string_view, 17> expected_names{
        "android", "apple", "brave",  "chrome", "edge",    "firefox", "ipad",    "iphone", "linux",
        "mac",     "opera", "safari", "ubuntu", "unknown", "vivaldi", "windows", "xbox",
    };
    REQUIRE(cases.size() == 17);
    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto& [device, expected] = cases[index];
        CAPTURE(expected_names.at(index));
        CHECK(td_session_device_type_name(expected) == expected_names.at(index));
        std::vector<td_api::object_ptr<td_api::session>> items;
        items.push_back(session(42, std::move(device)));
        auto converted = convert_sessions(session_result(std::move(items)));
        const auto* sessions = converted.get_if<TdSessions>();
        REQUIRE(sessions != nullptr);
        REQUIRE(sessions->items.size() == 1);
        CHECK(sessions->items.front().device_type == expected);
        CHECK(td_session_device_type_name(sessions->items.front().device_type) ==
              expected_names.at(index));
    }
}

TEST_CASE("production session conversion is strict lossless and order preserving",
          "[core][tdlib][td-runtime-converter][session]") {
    std::vector<td_api::object_ptr<td_api::session>> items;
    items.push_back(session(std::numeric_limits<std::int64_t>::min()));
    auto zero = session(0, td_api::make_object<td_api::sessionDeviceTypeUnknown>());
    zero->is_current_ = false;
    zero->is_password_pending_ = false;
    zero->is_unconfirmed_ = false;
    zero->can_accept_secret_chats_ = false;
    zero->can_accept_calls_ = true;
    zero->api_id_ = std::numeric_limits<std::int32_t>::max();
    zero->application_name_ = "tabs\tand\nlines";
    zero->application_version_.clear();
    zero->is_official_application_ = true;
    zero->log_in_date_ = 0;
    zero->last_active_date_ = 0;
    zero->ip_address_.clear();
    zero->location_.clear();
    items.push_back(std::move(zero));
    items.push_back(session(std::numeric_limits<std::int64_t>::max(),
                            td_api::make_object<td_api::sessionDeviceTypeWindows>()));

    auto converted = convert_sessions(session_result(std::move(items), 366));
    const auto* sessions = converted.get_if<TdSessions>();
    REQUIRE(sessions != nullptr);
    CHECK(sessions->inactive_session_ttl_days == 366);
    REQUIRE(sessions->items.size() == 3);
    CHECK(sessions->items[0] == TdSession{.id = "-9223372036854775808",
                                          .is_current = true,
                                          .is_password_pending = true,
                                          .is_unconfirmed = true,
                                          .can_accept_secret_chats = true,
                                          .can_accept_calls = false,
                                          .device_type = TdSessionDeviceType::Linux,
                                          .api_id = std::numeric_limits<std::int32_t>::min(),
                                          .application_name = "tgcli 🧪",
                                          .application_version = "1.2.3",
                                          .is_official_application = false,
                                          .device_model = "workstation",
                                          .platform = "Linux",
                                          .system_version = "6.8",
                                          .log_in_date = "1970-01-01T00:00:01Z",
                                          .last_active_date = "2038-01-19T03:14:07Z",
                                          .ip_address = "203.0.113.7",
                                          .location = "Athens"});
    CHECK(sessions->items[1].id == "0");
    CHECK(sessions->items[1].device_type == TdSessionDeviceType::Unknown);
    CHECK(sessions->items[1].application_name == "tabs\tand\nlines");
    CHECK_FALSE(sessions->items[1].log_in_date.has_value());
    CHECK_FALSE(sessions->items[1].last_active_date.has_value());
    CHECK(sessions->items[2].id == "9223372036854775807");
    CHECK(sessions->items[2].device_type == TdSessionDeviceType::Windows);
}

TEST_CASE("production session conversion rejects malformed input all or nothing",
          "[core][tdlib][td-runtime-converter][session]") {
    SECTION("null result") {
        NativeObjectPtr native;
        auto converted = convert_sessions(std::move(native));
        CHECK_FALSE(require_session_error(converted).tdlib_type_id.has_value());
    }

    SECTION("unexpected result type preserves its type id") {
        auto converted = convert_sessions(td_api::make_object<td_api::ok>());
        CHECK(require_session_error(converted).tdlib_type_id == td_api::ok::ID);
    }

    SECTION("null session") {
        std::vector<td_api::object_ptr<td_api::session>> items;
        items.emplace_back();
        auto converted = convert_sessions(session_result(std::move(items)));
        CHECK_FALSE(require_session_error(converted).tdlib_type_id.has_value());
    }

    SECTION("null device") {
        auto item = session(1);
        item->device_type_ = nullptr;
        std::vector<td_api::object_ptr<td_api::session>> items;
        items.push_back(std::move(item));
        auto converted = convert_sessions(session_result(std::move(items)));
        CHECK_FALSE(require_session_error(converted).tdlib_type_id.has_value());
    }

    SECTION("future device type is not folded to unknown") {
        std::vector<td_api::object_ptr<td_api::session>> items;
        items.push_back(session(1, td_api::make_object<UnsupportedSessionDeviceType>()));
        auto converted = convert_sessions(session_result(std::move(items)));
        CHECK(require_session_error(converted).tdlib_type_id == UnsupportedSessionDeviceType::ID);
    }

    SECTION("duplicate ids") {
        std::vector<td_api::object_ptr<td_api::session>> items;
        items.push_back(session(7));
        items.push_back(session(7));
        auto converted = convert_sessions(session_result(std::move(items)));
        CHECK_FALSE(require_session_error(converted).tdlib_type_id.has_value());
    }

    SECTION("invalid TTL") {
        for (const auto ttl : {0, 367}) {
            CAPTURE(ttl);
            std::vector<td_api::object_ptr<td_api::session>> items;
            items.push_back(session(1));
            auto converted = convert_sessions(session_result(std::move(items), ttl));
            CHECK_FALSE(require_session_error(converted).tdlib_type_id.has_value());
        }
    }

    SECTION("negative dates") {
        for (const bool login_date : {false, true}) {
            CAPTURE(login_date);
            auto item = session(1);
            if (login_date) {
                item->log_in_date_ = -1;
            } else {
                item->last_active_date_ = -1;
            }
            std::vector<td_api::object_ptr<td_api::session>> items;
            items.push_back(std::move(item));
            auto converted = convert_sessions(session_result(std::move(items)));
            CHECK_FALSE(require_session_error(converted).tdlib_type_id.has_value());
        }
    }

    SECTION("invalid UTF-8") {
        auto item = session(1);
        item->location_ = std::string("\xC3\x28", 2);
        std::vector<td_api::object_ptr<td_api::session>> items;
        items.push_back(std::move(item));
        auto converted = convert_sessions(session_result(std::move(items)));
        CHECK_FALSE(require_session_error(converted).tdlib_type_id.has_value());
    }
}

TEST_CASE("production converter preserves resolver chat and link metadata",
          "[core][tdlib][td-runtime-converter][resolver]") {
    SECTION("chat variants retain the identity relation") {
        struct ChatCase {
            td_api::object_ptr<td_api::ChatType> type;
            TdChatKind expected_kind;
            std::int64_t related_id;
        };
        std::vector<ChatCase> cases;
        cases.push_back(
            {td_api::make_object<td_api::chatTypePrivate>(10), TdChatKind::Private, 10});
        cases.push_back(
            {td_api::make_object<td_api::chatTypeBasicGroup>(20), TdChatKind::BasicGroup, 20});
        cases.push_back({td_api::make_object<td_api::chatTypeSupergroup>(30, false),
                         TdChatKind::Supergroup, 30});
        cases.push_back(
            {td_api::make_object<td_api::chatTypeSupergroup>(40, true), TdChatKind::Channel, 40});
        cases.push_back(
            {td_api::make_object<td_api::chatTypeSecret>(50, 60), TdChatKind::Secret, 60});
        for (auto& test_case : cases) {
            auto chat = td_api::make_object<td_api::chat>();
            chat->id_ = -100;
            chat->title_ = "Resolver chat";
            const auto type_id = test_case.type->get_id();
            chat->type_ = std::move(test_case.type);
            auto converted = convert_response(std::move(chat));
            const auto* value = converted.get_if<TdChat>();
            REQUIRE(value != nullptr);
            CHECK(value->id == -100);
            CHECK(value->title == "Resolver chat");
            CHECK(value->kind == test_case.expected_kind);
            CHECK(value->related_id == test_case.related_id);
            CHECK(value->tdlib_type_id == type_id);
        }

        auto unknown = td_api::make_object<td_api::chat>();
        unknown->id_ = -200;
        unknown->title_ = "Unknown";
        auto converted = convert_response(std::move(unknown));
        const auto* value = converted.get_if<TdChat>();
        REQUIRE(value != nullptr);
        CHECK(value->kind == TdChatKind::Unknown);
        CHECK(value->related_id == 0);
        CHECK(value->tdlib_type_id == 0);
    }

    SECTION("chat lists and supergroup usernames remain neutral") {
        auto chats = convert_response(
            td_api::make_object<td_api::chats>(3, std::vector<std::int64_t>{-1, -2, -3}));
        const auto* list = chats.get_if<TdChats>();
        REQUIRE(list != nullptr);
        CHECK(list->chat_ids == std::vector<std::int64_t>{-1, -2, -3});

        auto usernames = td_api::make_object<td_api::usernames>();
        usernames->active_usernames_ = {"project", "project_news"};
        auto supergroup = td_api::make_object<td_api::supergroup>();
        supergroup->id_ = 55;
        supergroup->usernames_ = std::move(usernames);
        supergroup->is_channel_ = true;
        auto converted = convert_response(std::move(supergroup));
        const auto* value = converted.get_if<TdSupergroup>();
        REQUIRE(value != nullptr);
        CHECK(*value ==
              TdSupergroup{.id = 55, .usernames = {"project", "project_news"}, .is_channel = true});
    }

    SECTION("pinned internal link inventory is exhaustive and retains exact branch data") {
        auto inventory = make_internal_link_inventory(PinnedInternalLinkTypes{});
        REQUIRE(inventory.size() == 57);
        std::vector<std::int32_t> type_ids;
        type_ids.reserve(inventory.size());
        for (auto& native : inventory) {
            const auto type_id = native->get_id();
            type_ids.push_back(type_id);
            auto converted = convert_response(std::move(native));
            const auto* value = converted.get_if<TdInternalLink>();
            REQUIRE(value != nullptr);
            CHECK(value->tdlib_type_id == type_id);
            const auto supported = supported_internal_link_kind(type_id);
            CHECK(value->kind == supported.value_or(TdInternalLinkKind::Unsupported));
        }
        std::ranges::sort(type_ids);
        CHECK(std::ranges::adjacent_find(type_ids) == type_ids.end());

        auto public_chat = td_api::make_object<td_api::internalLinkTypePublicChat>();
        public_chat->chat_username_ = "project";
        auto converted_public = convert_response(std::move(public_chat));
        const auto* public_value = converted_public.get_if<TdInternalLink>();
        REQUIRE(public_value != nullptr);
        CHECK(public_value->kind == TdInternalLinkKind::PublicChat);
        CHECK(public_value->username == "project");
        CHECK(public_value->tdlib_type_id == td_api::internalLinkTypePublicChat::ID);

        auto message = td_api::make_object<td_api::internalLinkTypeMessage>();
        message->url_ = "https://t.me/project/123";
        auto converted_message = convert_response(std::move(message));
        const auto* message_value = converted_message.get_if<TdInternalLink>();
        REQUIRE(message_value != nullptr);
        CHECK(message_value->kind == TdInternalLinkKind::Message);
        CHECK(message_value->url == "https://t.me/project/123");

        auto bot_start = td_api::make_object<td_api::internalLinkTypeBotStart>();
        bot_start->bot_username_ = "helper_bot";
        auto converted_bot_start = convert_response(std::move(bot_start));
        const auto* bot_start_value = converted_bot_start.get_if<TdInternalLink>();
        REQUIRE(bot_start_value != nullptr);
        CHECK(bot_start_value->kind == TdInternalLinkKind::BotStart);
        CHECK(bot_start_value->username == "helper_bot");

        auto invite = td_api::make_object<td_api::internalLinkTypeChatInvite>();
        invite->invite_link_ = "https://t.me/+invite";
        auto converted_invite = convert_response(std::move(invite));
        const auto* invite_value = converted_invite.get_if<TdInternalLink>();
        REQUIRE(invite_value != nullptr);
        CHECK(invite_value->kind == TdInternalLinkKind::ChatInvite);
        CHECK(invite_value->url == "https://t.me/+invite");

        auto direct = td_api::make_object<td_api::internalLinkTypeDirectMessagesChat>();
        direct->channel_username_ = "project";
        auto converted_direct = convert_response(std::move(direct));
        const auto* direct_value = converted_direct.get_if<TdInternalLink>();
        REQUIRE(direct_value != nullptr);
        CHECK(direct_value->kind == TdInternalLinkKind::DirectMessagesChat);
        CHECK(direct_value->username == "project");

        auto saved = convert_response(td_api::make_object<td_api::internalLinkTypeSavedMessages>());
        const auto* saved_value = saved.get_if<TdInternalLink>();
        REQUIRE(saved_value != nullptr);
        CHECK(saved_value->kind == TdInternalLinkKind::SavedMessages);

        auto unsupported = td_api::make_object<td_api::internalLinkTypeUnknownDeepLink>();
        auto converted_unsupported = convert_response(std::move(unsupported));
        const auto* unsupported_value = converted_unsupported.get_if<TdInternalLink>();
        REQUIRE(unsupported_value != nullptr);
        CHECK(unsupported_value->kind == TdInternalLinkKind::Unsupported);
        CHECK(unsupported_value->tdlib_type_id == td_api::internalLinkTypeUnknownDeepLink::ID);
    }

    SECTION("message, invite, and full-info metadata is preserved") {
        auto native_message = td_api::make_object<td_api::message>();
        native_message->id_ = 123;
        auto info = td_api::make_object<td_api::messageLinkInfo>();
        info->is_public_ = true;
        info->chat_id_ = -1001;
        info->message_ = std::move(native_message);
        info->topic_id_ = td_api::make_object<td_api::messageTopicForum>(7);
        auto converted_info = convert_response(std::move(info));
        const auto* message_info = converted_info.get_if<TdMessageLinkInfo>();
        REQUIRE(message_info != nullptr);
        CHECK(message_info->is_public);
        CHECK(message_info->chat_id == -1001);
        CHECK(message_info->message_id == 123);
        REQUIRE(message_info->topic);
        CHECK(message_info->topic->kind == TdTopicKind::Forum);
        CHECK(message_info->topic->id == 7);

        auto invite = td_api::make_object<td_api::chatInviteLinkInfo>();
        invite->chat_id_ = -7;
        invite->is_public_ = false;
        auto converted_invite = convert_response(std::move(invite));
        CHECK(*converted_invite.get_if<TdChatInviteLinkInfo>() ==
              TdChatInviteLinkInfo{.chat_id = -7, .is_public = false});

        auto full = td_api::make_object<td_api::supergroupFullInfo>();
        full->direct_messages_chat_id_ = -8;
        auto converted_full = convert_response(std::move(full));
        CHECK(*converted_full.get_if<TdSupergroupFullInfo>() ==
              TdSupergroupFullInfo{.direct_messages_chat_id = -8});
    }
}
