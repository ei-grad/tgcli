#pragma once

#include "core/td_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tgcli::core {

class TdClient;

enum class TdFunctionKind {
    GetAuthorizationState,
    SetTdlibParameters,
    SetAuthenticationPhoneNumber,
    RequestQrCodeAuthentication,
    CheckAuthenticationBotToken,
    SetAuthenticationEmailAddress,
    CheckAuthenticationEmailCode,
    CheckAuthenticationCode,
    RegisterUser,
    CheckAuthenticationPassword,
    GetOption,
    GetMe,
    LogOut,
    Close,
};

constexpr std::string_view td_function_name(TdFunctionKind function) {
    switch (function) {
    case TdFunctionKind::GetAuthorizationState:
        return "getAuthorizationState";
    case TdFunctionKind::SetTdlibParameters:
        return "setTdlibParameters";
    case TdFunctionKind::SetAuthenticationPhoneNumber:
        return "setAuthenticationPhoneNumber";
    case TdFunctionKind::RequestQrCodeAuthentication:
        return "requestQrCodeAuthentication";
    case TdFunctionKind::CheckAuthenticationBotToken:
        return "checkAuthenticationBotToken";
    case TdFunctionKind::SetAuthenticationEmailAddress:
        return "setAuthenticationEmailAddress";
    case TdFunctionKind::CheckAuthenticationEmailCode:
        return "checkAuthenticationEmailCode";
    case TdFunctionKind::CheckAuthenticationCode:
        return "checkAuthenticationCode";
    case TdFunctionKind::RegisterUser:
        return "registerUser";
    case TdFunctionKind::CheckAuthenticationPassword:
        return "checkAuthenticationPassword";
    case TdFunctionKind::GetOption:
        return "getOption";
    case TdFunctionKind::GetMe:
        return "getMe";
    case TdFunctionKind::LogOut:
        return "logOut";
    case TdFunctionKind::Close:
        return "close";
    }
    return "other";
}

enum class TdRedactedValue { Credential };

using TdFieldValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string,
                                  std::vector<std::int64_t>, TdRedactedValue>;

namespace detail {

constexpr std::uint64_t td_descriptor_label(std::string_view label) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (const auto character : label) {
        result ^= static_cast<unsigned char>(character);
        result *= 1099511628211ULL;
    }
    return result;
}

} // namespace detail

class TdFunctionField {
  public:
    TdFunctionField(std::string_view name, TdFieldValue value)
        : name_id_(detail::td_descriptor_label(name)), value_(std::move(value)) {}

    [[nodiscard]] bool has_name(std::string_view name) const {
        return name_id_ == detail::td_descriptor_label(name);
    }

    [[nodiscard]] const TdFieldValue& value() const {
        return value_;
    }

    bool operator==(const TdFunctionField&) const = default;

  private:
    std::uint64_t name_id_;
    TdFieldValue value_;
};

class TdFunctionData {
  public:
    explicit TdFunctionData(TdFunctionKind kind, std::vector<TdFunctionField> fields = {})
        : kind_(kind), type_id_(detail::td_descriptor_label(td_function_name(kind))),
          fields_(std::move(fields)) {}

    explicit TdFunctionData(std::string_view type, std::vector<TdFunctionField> fields = {})
        : type_id_(detail::td_descriptor_label(type)), fields_(std::move(fields)) {}

    [[nodiscard]] const std::optional<TdFunctionKind>& kind() const {
        return kind_;
    }

    [[nodiscard]] bool has_type(std::string_view type) const {
        return type_id_ == detail::td_descriptor_label(type);
    }

    [[nodiscard]] const std::vector<TdFunctionField>& fields() const {
        return fields_;
    }

    bool operator==(const TdFunctionData&) const = default;

  private:
    std::optional<TdFunctionKind> kind_;
    std::uint64_t type_id_;
    std::vector<TdFunctionField> fields_;
};

// Move-only type erasure keeps generated TDLib types out of project headers.
// The optional neutral descriptor lets the shared fake inspect functions
// without depending on generated types or serializing secret-bearing values.
class TdValue {
  public:
    TdValue() = default;
    TdValue(const TdValue&) = delete;
    TdValue& operator=(const TdValue&) = delete;
    TdValue(TdValue&&) noexcept = default;
    TdValue& operator=(TdValue&&) noexcept = default;
    ~TdValue() = default;

    template <typename T> static TdValue from(T value) {
        TdValue result;
        result.value_ = std::make_unique<Holder<T>>(std::move(value));
        return result;
    }

    template <typename T> static TdValue function(T value, TdFunctionData function) {
        TdValue result = from(std::move(value));
        result.function_ = std::move(function);
        return result;
    }

    static TdValue scripted_function(TdFunctionData function) {
        TdValue result;
        result.function_ = std::move(function);
        return result;
    }

    template <typename T> [[nodiscard]] T* get_if() {
        auto* holder = dynamic_cast<Holder<T>*>(value_.get());
        return holder == nullptr ? nullptr : &holder->value;
    }

    template <typename T> [[nodiscard]] const T* get_if() const {
        const auto* holder = dynamic_cast<const Holder<T>*>(value_.get());
        return holder == nullptr ? nullptr : &holder->value;
    }

    [[nodiscard]] bool has_value() const {
        return value_ != nullptr;
    }

    [[nodiscard]] const std::optional<TdFunctionData>& function_data() const {
        return function_;
    }

    void set_receive_event_sequence(std::uint64_t sequence) {
        receive_event_sequence_ = sequence;
    }

    [[nodiscard]] std::uint64_t receive_event_sequence() const {
        return receive_event_sequence_;
    }

  private:
    struct ValueBase {
        ValueBase() = default;
        ValueBase(const ValueBase&) = delete;
        ValueBase& operator=(const ValueBase&) = delete;
        ValueBase(ValueBase&&) = delete;
        ValueBase& operator=(ValueBase&&) = delete;
        virtual ~ValueBase() = default;
    };

    template <typename T> struct Holder final : ValueBase {
        explicit Holder(T stored) : value(std::move(stored)) {}
        T value;
    };

    std::unique_ptr<ValueBase> value_;
    std::optional<TdFunctionData> function_;
    std::uint64_t receive_event_sequence_ = 0;
};

enum class AuthState {
    Unknown,
    WaitTdlibParameters,
    WaitPhoneNumber,
    WaitPremiumPurchase,
    WaitEmailAddress,
    WaitEmailCode,
    WaitCode,
    WaitOtherDeviceConfirmation,
    WaitRegistration,
    WaitPassword,
    Ready,
    LoggingOut,
    Closing,
    Closed,
};

constexpr std::string_view auth_state_name(AuthState state) {
    switch (state) {
    case AuthState::Unknown:
        return "unknown";
    case AuthState::WaitTdlibParameters:
        return "wait_tdlib_parameters";
    case AuthState::WaitPhoneNumber:
        return "wait_phone_number";
    case AuthState::WaitPremiumPurchase:
        return "wait_premium_purchase";
    case AuthState::WaitEmailAddress:
        return "wait_email_address";
    case AuthState::WaitEmailCode:
        return "wait_email_code";
    case AuthState::WaitCode:
        return "wait_code";
    case AuthState::WaitOtherDeviceConfirmation:
        return "wait_other_device_confirmation";
    case AuthState::WaitRegistration:
        return "wait_registration";
    case AuthState::WaitPassword:
        return "wait_password";
    case AuthState::Ready:
        return "ready";
    case AuthState::LoggingOut:
        return "logging_out";
    case AuthState::Closing:
        return "closing";
    case AuthState::Closed:
        return "closed";
    }
    return "unknown";
}

struct AuthWaitPremiumPurchase {
    std::string store_product_id;
    std::int32_t premium_day_count = 0;
    std::string support_email_address;
    std::string support_email_subject;

    bool operator==(const AuthWaitPremiumPurchase&) const = default;
};

struct AuthWaitEmailAddress {
    bool allow_apple_id = false;
    bool allow_google_id = false;

    bool operator==(const AuthWaitEmailAddress&) const = default;
};

enum class AuthEmailResetState { None, Available, Pending, Unknown };

struct AuthWaitEmailCode {
    bool allow_apple_id = false;
    bool allow_google_id = false;
    std::string email_address_pattern;
    std::int32_t expected_length = 0;
    AuthEmailResetState reset_state = AuthEmailResetState::None;
    std::int32_t reset_delay = 0;
    std::optional<std::int32_t> unsupported_reset_tdlib_type_id;

    bool operator==(const AuthWaitEmailCode&) const = default;
};

enum class AuthCodeDelivery {
    Unknown,
    TelegramMessage,
    Sms,
    SmsWord,
    SmsPhrase,
    Call,
    FlashCall,
    MissedCall,
    Fragment,
    FirebaseAndroid,
    FirebaseIos,
};

struct AuthCodeDeliveryInfo {
    AuthCodeDelivery type = AuthCodeDelivery::Unknown;
    std::optional<std::int32_t> expected_length;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const AuthCodeDeliveryInfo&) const = default;
};

struct AuthWaitCode {
    AuthCodeDeliveryInfo delivery;
    std::optional<AuthCodeDeliveryInfo> next_delivery;
    std::int32_t resend_timeout = 0;

    bool operator==(const AuthWaitCode&) const = default;
};

struct AuthWaitOtherDeviceConfirmation {
    std::string link;

    bool operator==(const AuthWaitOtherDeviceConfirmation&) const = default;
};

struct AuthWaitRegistration {
    std::string terms_text;
    std::int32_t minimum_user_age = 0;
    bool show_popup = false;

    bool operator==(const AuthWaitRegistration&) const = default;
};

struct AuthWaitPassword {
    std::string hint;
    bool has_recovery_email_address = false;
    bool has_passport_data = false;
    std::string recovery_email_address_pattern;

    bool operator==(const AuthWaitPassword&) const = default;
};

using AuthStateMetadata =
    std::variant<std::monostate, AuthWaitPremiumPurchase, AuthWaitEmailAddress, AuthWaitEmailCode,
                 AuthWaitCode, AuthWaitOtherDeviceConfirmation, AuthWaitRegistration,
                 AuthWaitPassword>;

struct AuthStateData {
    AuthStateData() = default;
    explicit AuthStateData(AuthState state_value, AuthStateMetadata metadata_value = {},
                           std::optional<std::int32_t> unsupported_type_id = std::nullopt)
        : state(state_value), metadata(std::move(metadata_value)),
          unsupported_tdlib_type_id(unsupported_type_id) {}

    AuthState state = AuthState::Unknown;
    AuthStateMetadata metadata;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const AuthStateData&) const = default;
};

struct AuthStateSnapshot {
    std::int32_t client_id = 0;
    std::uint64_t client_generation = 0;
    std::uint64_t auth_sequence = 0;
    std::uint64_t receive_event_sequence = 0;
    AuthStateData data;
};

enum class DescriptorKind { Read, AuthBootstrap, Write, Destructive, Lifecycle };
enum class TdOwnerKind { InternalAuth, Login, Request, Lifecycle };

struct TdRequestOwner {
    TdRequestOwner() = default;
    TdRequestOwner(TdOwnerKind kind_value, std::uint64_t id_value,
                   std::shared_ptr<const void> capability_value = {})
        : kind(kind_value), id(id_value), capability(std::move(capability_value)) {}

    TdOwnerKind kind = TdOwnerKind::Request;
    std::uint64_t id = 0;
    std::shared_ptr<const void> capability;

    bool operator==(const TdRequestOwner&) const = default;
};

struct TdSendDescriptor {
    TdFunctionKind function = TdFunctionKind::GetAuthorizationState;
    DescriptorKind tier = DescriptorKind::Read;
    TdRequestOwner owner;
    std::uint64_t client_generation = 0;
    std::uint64_t auth_sequence = 0;
    AuthState auth_state = AuthState::Unknown;
};

enum class TdAuthorizationFailure {
    FunctionMismatch,
    TierMismatch,
    OwnerMismatch,
    GenerationMismatch,
    AuthSequenceMismatch,
    AuthStateMismatch,
    FunctionDenied,
    GenerationClosed,
};

constexpr std::string_view authorization_failure_name(TdAuthorizationFailure failure) {
    switch (failure) {
    case TdAuthorizationFailure::FunctionMismatch:
        return "function_mismatch";
    case TdAuthorizationFailure::TierMismatch:
        return "tier_mismatch";
    case TdAuthorizationFailure::OwnerMismatch:
        return "owner_mismatch";
    case TdAuthorizationFailure::GenerationMismatch:
        return "generation_mismatch";
    case TdAuthorizationFailure::AuthSequenceMismatch:
        return "auth_sequence_mismatch";
    case TdAuthorizationFailure::AuthStateMismatch:
        return "auth_state_mismatch";
    case TdAuthorizationFailure::FunctionDenied:
        return "function_denied";
    case TdAuthorizationFailure::GenerationClosed:
        return "generation_closed";
    }
    return "function_denied";
}

struct TdlibParameters {
    TdlibParameters() = default;
    ~TdlibParameters();
    TdlibParameters(const TdlibParameters&) = delete;
    TdlibParameters& operator=(const TdlibParameters&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    TdlibParameters(TdlibParameters&& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    TdlibParameters& operator=(TdlibParameters&& other);

    bool use_test_dc = false;
    std::string database_directory;
    std::string files_directory;
    std::string database_encryption_key;
    bool use_file_database = true;
    bool use_chat_info_database = true;
    bool use_message_database = true;
    bool use_secret_chats = false;
    std::int32_t api_id = 0;
    std::string api_hash;
    std::string system_language_code = "en";
    std::string device_model = "tgcli";
    std::string system_version;
    std::string application_version;
};

TdFunctionData describe_tdlib_parameters(const TdlibParameters& parameters);

struct TdAuthRequest {
    explicit TdAuthRequest(TdFunctionKind function_value);
    TdAuthRequest(TdFunctionKind function_value, std::string&& value_value);
    TdAuthRequest(TdFunctionKind function_value, std::string&& value_value,
                  std::string&& secondary_value);
    ~TdAuthRequest();
    TdAuthRequest(const TdAuthRequest&) = delete;
    TdAuthRequest& operator=(const TdAuthRequest&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    TdAuthRequest(TdAuthRequest&& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    TdAuthRequest& operator=(TdAuthRequest&& other);

    TdFunctionKind function;
    std::string value;
    std::string secondary;
};

struct TdOk {};

struct TdError {
    std::int32_t code = 0;
    std::string message;
};

struct TdUserSummary {
    std::int64_t id = 0;
    std::string first_name;
    std::string last_name;
    std::vector<std::string> usernames;
    std::string phone_number;
    bool is_bot = false;
    bool is_premium = false;
};

enum class TdBuiltinFunction { GetAuthorizationState, LogOut, Close };

struct TdRuntimeEvent {
    std::int32_t client_id = 0;
    std::uint64_t client_generation = 0;
    std::uint64_t query_id = 0;
    TdValue object;
    std::optional<AuthStateData> authorization_state;
};

// The sole production/fake boundary used by TdClient. Implementations stamp
// every received event with the generation assigned at client creation.
class TdRuntime {
  public:
    TdRuntime() = default;
    TdRuntime(const TdRuntime&) = delete;
    TdRuntime& operator=(const TdRuntime&) = delete;
    TdRuntime(TdRuntime&&) = delete;
    TdRuntime& operator=(TdRuntime&&) = delete;
    virtual ~TdRuntime() = default;

  private:
    friend class TdClient;

    virtual void initialize_process(const TdLogConfiguration& logging) = 0;
    virtual std::int32_t create_client(std::uint64_t client_generation) = 0;
    virtual TdValue make_function(TdBuiltinFunction function) = 0;
    virtual TdValue make_set_tdlib_parameters(TdlibParameters parameters) = 0;
    virtual TdValue make_auth_function(TdAuthRequest request) = 0;
    virtual void send(std::int32_t client_id, std::uint64_t client_generation,
                      std::uint64_t query_id, TdValue function) = 0;
    virtual std::optional<TdRuntimeEvent> receive(std::chrono::milliseconds timeout) = 0;
};

std::unique_ptr<TdRuntime> make_production_td_runtime();
std::string production_tdlib_version();

} // namespace tgcli::core
