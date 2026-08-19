// Exercises the production generated-TDLib-to-neutral conversion boundary.
// Tagged [tdlib] because this translation unit includes generated TDLib types.

#include "core/td_runtime_test_adapter.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
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
