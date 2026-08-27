#pragma once

#include "common/secure_wipe.hpp"
#include "core/m6_td.hpp"
#include "core/td_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tgcli::core {

class TdClient;
using TdEventClock = std::chrono::steady_clock;

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
    GetCurrentState,
    GetContacts,
    SearchContacts,
    AddContact,
    RemoveContacts,
    SetMessageSenderBlockList,
    GetSavedMessagesTags,
    SearchSavedMessages,
    GetActiveSessions,
    TerminateSession,
    GetChatFolder,
    CreateChatFolder,
    EditChatFolder,
    DeleteChatFolder,
    GetForumTopics,
    GetForumTopic,
    CreateForumTopic,
    EditForumTopic,
    ToggleForumTopicIsClosed,
    GetChatMember,
    SetChatTitle,
    SetChatPhoto,
    SetChatDescription,
    CreateChatInviteLink,
    RevokeChatInviteLink,
    SetChatMemberStatus,
    SetChatPermissions,
    GetStorageStatistics,
    OptimizeStorage,
    GetChat,
    GetChatHistory,
    GetChatMessageByDate,
    GetMessageThread,
    GetForumTopicHistory,
    GetMessageThreadHistory,
    GetDirectMessagesChatTopicHistory,
    GetSavedMessagesTopicHistory,
    GetMessages,
    GetMessageLink,
    GetChats,
    LoadChats,
    SearchPublicChat,
    GetInternalLinkType,
    GetMessageLinkInfo,
    CheckChatInviteLink,
    GetUser,
    GetBasicGroupFullInfo,
    GetSupergroup,
    GetSupergroupFullInfo,
    GetSupergroupMembers,
    CreatePrivateChat,
    GetMessage,
    GetMessageProperties,
    GetMessageAvailableReactions,
    ParseTextEntities,
    SendMessage,
    ForwardMessages,
    EditMessageText,
    DeleteMessages,
    AddMessageReaction,
    RemoveMessageReaction,
    PinChatMessage,
    UnpinChatMessage,
    ViewMessages,
    SetChatNotificationSettings,
    ToggleChatIsPinned,
    AddChatToList,
    JoinChat,
    JoinChatByInviteLink,
    LeaveChat,
    LogOut,
    Close,
};

[[nodiscard]] TdFunctionKind td_m6_request_kind(const TdM6Request& request) noexcept;
[[nodiscard]] bool valid_td_m6_request(const TdM6Request& request) noexcept;
[[nodiscard]] bool td_m6_request_is_read(const TdM6Request& request) noexcept;

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
    case TdFunctionKind::GetCurrentState:
        return "getCurrentState";
    case TdFunctionKind::GetContacts:
        return "getContacts";
    case TdFunctionKind::SearchContacts:
        return "searchContacts";
    case TdFunctionKind::AddContact:
        return "addContact";
    case TdFunctionKind::RemoveContacts:
        return "removeContacts";
    case TdFunctionKind::SetMessageSenderBlockList:
        return "setMessageSenderBlockList";
    case TdFunctionKind::GetSavedMessagesTags:
        return "getSavedMessagesTags";
    case TdFunctionKind::SearchSavedMessages:
        return "searchSavedMessages";
    case TdFunctionKind::GetActiveSessions:
        return "getActiveSessions";
    case TdFunctionKind::TerminateSession:
        return "terminateSession";
    case TdFunctionKind::GetChatFolder:
        return "getChatFolder";
    case TdFunctionKind::CreateChatFolder:
        return "createChatFolder";
    case TdFunctionKind::EditChatFolder:
        return "editChatFolder";
    case TdFunctionKind::DeleteChatFolder:
        return "deleteChatFolder";
    case TdFunctionKind::GetForumTopics:
        return "getForumTopics";
    case TdFunctionKind::GetForumTopic:
        return "getForumTopic";
    case TdFunctionKind::CreateForumTopic:
        return "createForumTopic";
    case TdFunctionKind::EditForumTopic:
        return "editForumTopic";
    case TdFunctionKind::ToggleForumTopicIsClosed:
        return "toggleForumTopicIsClosed";
    case TdFunctionKind::GetChatMember:
        return "getChatMember";
    case TdFunctionKind::SetChatTitle:
        return "setChatTitle";
    case TdFunctionKind::SetChatPhoto:
        return "setChatPhoto";
    case TdFunctionKind::SetChatDescription:
        return "setChatDescription";
    case TdFunctionKind::CreateChatInviteLink:
        return "createChatInviteLink";
    case TdFunctionKind::RevokeChatInviteLink:
        return "revokeChatInviteLink";
    case TdFunctionKind::SetChatMemberStatus:
        return "setChatMemberStatus";
    case TdFunctionKind::SetChatPermissions:
        return "setChatPermissions";
    case TdFunctionKind::GetStorageStatistics:
        return "getStorageStatistics";
    case TdFunctionKind::OptimizeStorage:
        return "optimizeStorage";
    case TdFunctionKind::GetChat:
        return "getChat";
    case TdFunctionKind::GetChatHistory:
        return "getChatHistory";
    case TdFunctionKind::GetChatMessageByDate:
        return "getChatMessageByDate";
    case TdFunctionKind::GetMessageThread:
        return "getMessageThread";
    case TdFunctionKind::GetForumTopicHistory:
        return "getForumTopicHistory";
    case TdFunctionKind::GetMessageThreadHistory:
        return "getMessageThreadHistory";
    case TdFunctionKind::GetDirectMessagesChatTopicHistory:
        return "getDirectMessagesChatTopicHistory";
    case TdFunctionKind::GetSavedMessagesTopicHistory:
        return "getSavedMessagesTopicHistory";
    case TdFunctionKind::GetMessages:
        return "getMessages";
    case TdFunctionKind::GetMessageLink:
        return "getMessageLink";
    case TdFunctionKind::GetChats:
        return "getChats";
    case TdFunctionKind::LoadChats:
        return "loadChats";
    case TdFunctionKind::SearchPublicChat:
        return "searchPublicChat";
    case TdFunctionKind::GetInternalLinkType:
        return "getInternalLinkType";
    case TdFunctionKind::GetMessageLinkInfo:
        return "getMessageLinkInfo";
    case TdFunctionKind::CheckChatInviteLink:
        return "checkChatInviteLink";
    case TdFunctionKind::GetUser:
        return "getUser";
    case TdFunctionKind::GetBasicGroupFullInfo:
        return "getBasicGroupFullInfo";
    case TdFunctionKind::GetSupergroup:
        return "getSupergroup";
    case TdFunctionKind::GetSupergroupFullInfo:
        return "getSupergroupFullInfo";
    case TdFunctionKind::GetSupergroupMembers:
        return "getSupergroupMembers";
    case TdFunctionKind::CreatePrivateChat:
        return "createPrivateChat";
    case TdFunctionKind::GetMessage:
        return "getMessage";
    case TdFunctionKind::GetMessageProperties:
        return "getMessageProperties";
    case TdFunctionKind::GetMessageAvailableReactions:
        return "getMessageAvailableReactions";
    case TdFunctionKind::ParseTextEntities:
        return "parseTextEntities";
    case TdFunctionKind::SendMessage:
        return "sendMessage";
    case TdFunctionKind::ForwardMessages:
        return "forwardMessages";
    case TdFunctionKind::EditMessageText:
        return "editMessageText";
    case TdFunctionKind::DeleteMessages:
        return "deleteMessages";
    case TdFunctionKind::AddMessageReaction:
        return "addMessageReaction";
    case TdFunctionKind::RemoveMessageReaction:
        return "removeMessageReaction";
    case TdFunctionKind::PinChatMessage:
        return "pinChatMessage";
    case TdFunctionKind::UnpinChatMessage:
        return "unpinChatMessage";
    case TdFunctionKind::ViewMessages:
        return "viewMessages";
    case TdFunctionKind::SetChatNotificationSettings:
        return "setChatNotificationSettings";
    case TdFunctionKind::ToggleChatIsPinned:
        return "toggleChatIsPinned";
    case TdFunctionKind::AddChatToList:
        return "addChatToList";
    case TdFunctionKind::JoinChat:
        return "joinChat";
    case TdFunctionKind::JoinChatByInviteLink:
        return "joinChatByInviteLink";
    case TdFunctionKind::LeaveChat:
        return "leaveChat";
    case TdFunctionKind::LogOut:
        return "logOut";
    case TdFunctionKind::Close:
        return "close";
    }
    return "other";
}

enum class TdRedactedValue { Credential, InviteLink };

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

    template <typename T, typename Wiper>
    static TdValue sensitive_function(T value, TdFunctionData function, Wiper wiper) {
        TdValue result;
        result.value_ = std::make_unique<Holder<T>>(std::move(value),
                                                    std::function<void(T&)>{std::move(wiper)});
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

    void set_receive_event_metadata(std::uint64_t sequence, TdEventClock::time_point observed_at) {
        receive_event_sequence_ = sequence;
        receive_observed_at_ = observed_at;
    }

    [[nodiscard]] std::uint64_t receive_event_sequence() const {
        return receive_event_sequence_;
    }

    [[nodiscard]] std::optional<TdEventClock::time_point> receive_observed_at() const {
        return receive_observed_at_;
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
        Holder(T stored, std::function<void(T&)> stored_wiper)
            : value(std::move(stored)), wiper(std::move(stored_wiper)) {}
        Holder(const Holder&) = delete;
        Holder& operator=(const Holder&) = delete;
        Holder(Holder&&) = delete;
        Holder& operator=(Holder&&) = delete;
        ~Holder() override {
            if (wiper) {
                try {
                    wiper(value);
                } catch (...) {
                    // A sensitive-value destructor cannot allow cleanup failures to escape.
                    return;
                }
            }
        }
        T value;
        std::function<void(T&)> wiper;
    };

    std::unique_ptr<ValueBase> value_;
    std::optional<TdFunctionData> function_;
    std::uint64_t receive_event_sequence_ = 0;
    std::optional<TdEventClock::time_point> receive_observed_at_;
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
    std::optional<TdEventClock::time_point> receive_observed_at;
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

    bool operator==(const TdError&) const = default;
};

enum class TdUserPresence { Online, Offline, Hidden };

struct TdUserSummary {
    std::int64_t id = 0;
    std::string first_name;
    std::string last_name;
    std::vector<std::string> usernames;
    std::string phone_number;
    bool is_bot = false;
    bool is_premium = false;
    TdUserPresence presence = TdUserPresence::Hidden;

    bool operator==(const TdUserSummary&) const = default;
};

enum class TdReactionKind { Emoji, CustomEmoji, Paid, Unknown };

struct TdReactionType {
    TdReactionKind kind = TdReactionKind::Unknown;
    std::string emoji;
    std::int64_t custom_emoji_id = 0;
    std::int32_t tdlib_type_id = 0;

    bool operator==(const TdReactionType&) const = default;
};

struct TdSavedMessagesTag {
    TdReactionType tag;
    std::string label;
    std::int32_t count = 0;

    bool operator==(const TdSavedMessagesTag&) const = default;
};

struct TdSavedMessagesTags {
    std::vector<TdSavedMessagesTag> tags;

    bool operator==(const TdSavedMessagesTags&) const = default;
};

struct TdSavedMessageSummary {
    std::int64_t id = 0;
    std::int64_t chat_id = 0;
    std::int32_t date = 0;
    std::string text;

    bool operator==(const TdSavedMessageSummary&) const = default;
};

struct TdFoundSavedMessages {
    std::vector<TdSavedMessageSummary> messages;
    std::int64_t next_from_message_id = 0;

    bool operator==(const TdFoundSavedMessages&) const = default;
};

struct TdSearchSavedMessagesRequest {
    std::int64_t saved_messages_topic_id = 0;
    TdReactionType tag;
    std::string query;
    std::int64_t from_message_id = 0;
    std::int32_t offset = 0;
    std::int32_t limit = 0;
};

enum class TdSessionDeviceType {
    Android,
    Apple,
    Brave,
    Chrome,
    Edge,
    Firefox,
    Ipad,
    Iphone,
    Linux,
    Mac,
    Opera,
    Safari,
    Ubuntu,
    Unknown,
    Vivaldi,
    Windows,
    Xbox,
};

constexpr std::string_view td_session_device_type_name(TdSessionDeviceType type) {
    switch (type) {
    case TdSessionDeviceType::Android:
        return "android";
    case TdSessionDeviceType::Apple:
        return "apple";
    case TdSessionDeviceType::Brave:
        return "brave";
    case TdSessionDeviceType::Chrome:
        return "chrome";
    case TdSessionDeviceType::Edge:
        return "edge";
    case TdSessionDeviceType::Firefox:
        return "firefox";
    case TdSessionDeviceType::Ipad:
        return "ipad";
    case TdSessionDeviceType::Iphone:
        return "iphone";
    case TdSessionDeviceType::Linux:
        return "linux";
    case TdSessionDeviceType::Mac:
        return "mac";
    case TdSessionDeviceType::Opera:
        return "opera";
    case TdSessionDeviceType::Safari:
        return "safari";
    case TdSessionDeviceType::Ubuntu:
        return "ubuntu";
    case TdSessionDeviceType::Unknown:
        return "unknown";
    case TdSessionDeviceType::Vivaldi:
        return "vivaldi";
    case TdSessionDeviceType::Windows:
        return "windows";
    case TdSessionDeviceType::Xbox:
        return "xbox";
    }
    return "unknown";
}

struct TdSession {
    std::string id;
    bool is_current = false;
    bool is_password_pending = false;
    bool is_unconfirmed = false;
    bool can_accept_secret_chats = false;
    bool can_accept_calls = false;
    TdSessionDeviceType device_type = TdSessionDeviceType::Unknown;
    std::int32_t api_id = 0;
    std::string application_name;
    std::string application_version;
    bool is_official_application = false;
    std::string device_model;
    std::string platform;
    std::string system_version;
    std::optional<std::string> log_in_date;
    std::optional<std::string> last_active_date;
    std::string ip_address;
    std::string location;

    bool operator==(const TdSession&) const = default;
};

struct TdSessions {
    std::vector<TdSession> items;
    std::int32_t inactive_session_ttl_days = 0;

    bool operator==(const TdSessions&) const = default;
};

struct TdSessionConversionError {
    std::optional<std::int32_t> tdlib_type_id;

    bool operator==(const TdSessionConversionError&) const = default;
};

enum class TdChatListKind { Main, Archive, Folder, Unknown };

struct TdChatList {
    TdChatListKind kind = TdChatListKind::Unknown;
    std::int32_t folder_id = 0;
    std::int32_t tdlib_type_id = 0;

    bool operator==(const TdChatList&) const = default;
};

enum class TdChatKind { Private, BasicGroup, Supergroup, Channel, Secret, Unknown };

enum class TdTopicKind { Forum, Thread, Direct, Saved, Unknown };

struct TdTopic {
    TdTopicKind kind = TdTopicKind::Unknown;
    std::int64_t id = 0;
    std::int32_t tdlib_type_id = 0;

    bool operator==(const TdTopic&) const = default;
};

enum class TdMessageSenderKind { User, Chat, Unknown };

struct TdMessageSender {
    TdMessageSenderKind kind = TdMessageSenderKind::Unknown;
    std::int64_t id = 0;
    std::int32_t tdlib_type_id = 0;

    bool operator==(const TdMessageSender&) const = default;
};

enum class TdMessageContentKind { Text, Photo, Video, Document, Voice, Other };

struct TdMessageSummary {
    std::int64_t id = 0;
    std::int64_t chat_id = 0;
    std::int32_t date = 0;
    TdMessageSender sender;
    bool is_outgoing = false;
    std::optional<TdTopic> topic;
    TdMessageContentKind content_kind = TdMessageContentKind::Other;
    std::string text;

    bool operator==(const TdMessageSummary&) const = default;
};

enum class TdMessageSendingStateKind { Stable, Pending, Failed, Unknown };

struct TdMessageSendingState {
    TdMessageSendingStateKind kind = TdMessageSendingStateKind::Stable;
    std::int32_t sending_id = 0;
    std::optional<TdError> error;
    bool can_retry = false;
    bool need_another_sender = false;
    bool need_another_reply_quote = false;
    bool need_drop_reply = false;
    std::int64_t required_paid_message_star_count = 0;
    double retry_after = 0;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const TdMessageSendingState&) const = default;
};

enum class TdMessageSchedulingStateKind {
    None,
    SendAtDate,
    SendWhenOnline,
    SendWhenVideoProcessed,
    Unknown
};

struct TdMessageSchedulingState {
    TdMessageSchedulingStateKind kind = TdMessageSchedulingStateKind::None;
    std::int32_t send_date = 0;
    std::int32_t repeat_period = 0;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const TdMessageSchedulingState&) const = default;
};

struct TdWriteMessage {
    TdMessageSummary message;
    TdMessageSendingState sending_state;
    TdMessageSchedulingState scheduling_state;
    bool has_reply_markup = false;

    bool operator==(const TdWriteMessage&) const = default;
};

inline constexpr std::int64_t kTdInt53Max = 9'007'199'254'740'991LL;

constexpr bool valid_td_chat_id(std::int64_t value) noexcept {
    return value != 0 && value >= -kTdInt53Max && value <= kTdInt53Max;
}

constexpr bool valid_td_nonzero_int53(std::int64_t value) noexcept {
    return valid_td_chat_id(value);
}

constexpr bool valid_td_message_id(std::int64_t value) noexcept {
    return value > 0 && value <= kTdInt53Max;
}

struct TdPlanningMessage {
    std::int64_t id = 0;
    std::int64_t chat_id = 0;
    std::int32_t date = 0;
    TdMessageSender sender;
    bool is_outgoing = false;
    std::optional<TdTopic> topic;
    TdMessageContentKind content_kind = TdMessageContentKind::Other;
    std::string text;
    bool has_scheduling_state = false;
    bool has_reply_markup = false;

    bool operator==(const TdPlanningMessage&) const = default;
};

struct TdMessageWriteResult {
    std::int64_t id = 0;
    std::int64_t chat_id = 0;
    std::optional<std::int32_t> date;
    TdMessageSender sender;
    bool is_outgoing = false;
    std::optional<TdTopic> topic;
    TdMessageContentKind content_kind = TdMessageContentKind::Other;
    std::string text;
    bool scheduled = false;

    bool operator==(const TdMessageWriteResult&) const = default;
};

struct TdMessageProperties {
    bool can_add_offer = false;
    bool can_add_tasks = false;
    bool can_be_approved = false;
    bool can_be_copied = false;
    bool can_be_copied_to_secret_chat = false;
    bool can_be_declined = false;
    bool can_be_deleted_only_for_self = false;
    bool can_be_deleted_for_all_users = false;
    bool can_be_edited = false;
    bool can_be_forwarded = false;
    bool can_be_paid = false;
    bool can_be_pinned = false;
    bool can_be_replied = false;
    bool can_be_replied_in_another_chat = false;
    bool can_be_saved = false;
    bool can_be_shared_in_story = false;
    bool can_delete_reactions = false;
    bool can_edit_media = false;
    bool can_edit_scheduling_state = false;
    bool can_edit_suggested_post_info = false;
    bool can_get_author = false;
    bool can_get_embedding_code = false;
    bool can_get_link = false;
    bool can_get_media_timestamp_links = false;
    bool can_get_message_thread = false;
    bool can_get_poll_vote_statistics = false;
    bool can_get_read_date = false;
    bool can_get_statistics = false;
    bool can_get_video_advertisements = false;
    bool can_get_viewers = false;
    bool can_mark_tasks_as_done = false;
    bool can_recognize_speech = false;
    bool can_report_chat = false;
    bool can_report_reactions = false;
    bool can_report_supergroup_spam = false;
    bool can_set_fact_check = false;
    bool has_protected_content_by_current_user = false;
    bool has_protected_content_by_other_user = false;
    bool need_show_statistics = false;

    bool operator==(const TdMessageProperties&) const = default;
};

struct TdAvailableReaction {
    TdReactionType type;
    bool needs_premium = false;

    bool operator==(const TdAvailableReaction&) const = default;
};

enum class TdReactionUnavailabilityReason {
    None,
    AnonymousAdministrator,
    Guest,
    Restricted,
    Unknown
};

struct TdMessageAvailableReactions {
    std::vector<TdAvailableReaction> top;
    std::vector<TdAvailableReaction> recent;
    std::vector<TdAvailableReaction> popular;
    bool allow_custom_emoji = false;
    bool are_tags = false;
    TdReactionUnavailabilityReason unavailability_reason = TdReactionUnavailabilityReason::None;
    std::optional<std::int32_t> unsupported_unavailability_tdlib_type_id;

    bool operator==(const TdMessageAvailableReactions&) const = default;
};

enum class TdTextParseMode { MarkdownV2, Html };

enum class TdTextEntityKind {
    Mention,
    Hashtag,
    Cashtag,
    BotCommand,
    Url,
    EmailAddress,
    PhoneNumber,
    BankCardNumber,
    Bold,
    Italic,
    Underline,
    Strikethrough,
    Spoiler,
    Code,
    Pre,
    PreCode,
    BlockQuote,
    ExpandableBlockQuote,
    TextUrl,
    MentionName,
    CustomEmoji,
    MediaTimestamp,
    DateTime,
    Unknown,
};

enum class TdDateTimePartPrecision { None, Short, Long };

struct TdDateTimeFormattingRelative {
    bool operator==(const TdDateTimeFormattingRelative&) const = default;
};

struct TdDateTimeFormattingAbsolute {
    TdDateTimePartPrecision time_precision = TdDateTimePartPrecision::None;
    TdDateTimePartPrecision date_precision = TdDateTimePartPrecision::None;
    bool show_day_of_week = false;

    bool operator==(const TdDateTimeFormattingAbsolute&) const = default;
};

using TdDateTimeFormatting =
    std::variant<TdDateTimeFormattingRelative, TdDateTimeFormattingAbsolute>;

struct TdTextEntity {
    std::int32_t offset = 0;
    std::int32_t length = 0;
    TdTextEntityKind kind = TdTextEntityKind::Unknown;
    std::string value;
    std::int64_t numeric_value = 0;
    std::int32_t tdlib_type_id = 0;
    std::optional<TdDateTimeFormatting> date_time_formatting;

    bool operator==(const TdTextEntity&) const = default;
};

class TdFormattedTextCapability {
  public:
    TdFormattedTextCapability() = default;
    TdFormattedTextCapability(const TdFormattedTextCapability&) = delete;
    TdFormattedTextCapability& operator=(const TdFormattedTextCapability&) = delete;
    TdFormattedTextCapability(TdFormattedTextCapability&&) noexcept = default;
    TdFormattedTextCapability& operator=(TdFormattedTextCapability&&) noexcept = default;
    ~TdFormattedTextCapability() = default;

    template <typename T>
    static TdFormattedTextCapability from(T value, std::uint64_t client_generation) {
        TdFormattedTextCapability result;
        result.value_ = std::make_unique<Holder<T>>(std::move(value));
        result.client_generation_ = client_generation;
        return result;
    }

    template <typename T> std::optional<T> consume(std::uint64_t client_generation) {
        if (client_generation == 0 || client_generation != client_generation_) {
            return std::nullopt;
        }
        auto* holder = dynamic_cast<Holder<T>*>(value_.get());
        if (holder == nullptr) {
            return std::nullopt;
        }
        std::optional<T> result{std::move(holder->value)};
        value_.reset();
        client_generation_ = 0;
        return result;
    }

    [[nodiscard]] bool valid_for(std::uint64_t client_generation) const noexcept {
        return value_ != nullptr && client_generation != 0 &&
               client_generation == client_generation_;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return value_ != nullptr;
    }

    [[nodiscard]] std::uint64_t client_generation() const noexcept {
        return client_generation_;
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
    std::uint64_t client_generation_ = 0;
};

struct TdScriptedFormattedTextCapability {
    std::string text;
    std::vector<TdTextEntity> entities;

    bool operator==(const TdScriptedFormattedTextCapability&) const = default;
};

struct TdFormattedText {
    std::string text;
    std::vector<TdTextEntity> entities;
    TdFormattedTextCapability capability;

    bool operator==(const TdFormattedText& other) const {
        return text == other.text && entities == other.entities;
    }
};

[[nodiscard]] bool valid_td_formatted_text_facts(const TdFormattedText& formatted) noexcept;

enum class TdSendScheduleKind { Immediate, AtDate, WhenOnline };

struct TdSendSchedule {
    TdSendScheduleKind kind = TdSendScheduleKind::Immediate;
    std::int32_t send_date = 0;

    bool operator==(const TdSendSchedule&) const = default;
};

struct TdMessageSendOptions {
    bool disable_notification = false;
    TdSendSchedule schedule;
    std::int32_t sending_id = 0;

    bool operator==(const TdMessageSendOptions&) const = default;
};

struct TdSendTextContent {
    TdFormattedText formatted_text;
    bool parsed = false;
};

struct TdSendDocumentContent {
    std::string local_path;
    bool disable_content_type_detection = true;

    bool operator==(const TdSendDocumentContent&) const = default;
};

struct TdSendMessageRequest {
    std::int64_t chat_id = 0;
    std::optional<TdTopic> topic;
    std::optional<std::int64_t> reply_to_message_id;
    TdMessageSendOptions options;
    TdSendTextContent content;
    std::optional<TdSendDocumentContent> document;
};

[[nodiscard]] bool valid_td_send_message_request(const TdSendMessageRequest& request) noexcept;
TdFunctionData describe_td_send_message_request(const TdSendMessageRequest& request);

struct TdForwardMessagesRequest {
    std::int64_t from_chat_id = 0;
    std::int64_t to_chat_id = 0;
    std::vector<std::int64_t> message_ids;
    std::int32_t sending_id = 0;
    bool drop_author = false;

    bool operator==(const TdForwardMessagesRequest&) const = default;
};

[[nodiscard]] bool
valid_td_forward_messages_request(const TdForwardMessagesRequest& request) noexcept;
TdFunctionData describe_td_forward_messages_request(const TdForwardMessagesRequest& request);

struct TdForwardMessages {
    std::vector<std::optional<TdWriteMessage>> messages;

    bool operator==(const TdForwardMessages&) const = default;
};

struct TdUpdateMessageSendSucceeded {
    std::uint64_t client_generation = 0;
    std::int64_t old_message_id = 0;
    std::optional<TdWriteMessage> message;

    bool operator==(const TdUpdateMessageSendSucceeded&) const = default;
};

struct TdUpdateMessageSendFailed {
    std::uint64_t client_generation = 0;
    std::int64_t old_message_id = 0;
    std::optional<TdWriteMessage> message;
    std::optional<TdError> error;

    bool operator==(const TdUpdateMessageSendFailed&) const = default;
};

struct TdUpdateDeleteMessages {
    std::uint64_t client_generation = 0;
    std::int64_t chat_id = 0;
    std::vector<std::int64_t> message_ids;
    bool is_permanent = false;
    bool from_cache = false;

    bool operator==(const TdUpdateDeleteMessages&) const = default;
};

struct TdOptionInteger {
    std::int64_t value = 0;

    bool operator==(const TdOptionInteger&) const = default;
};

struct TdChatNotificationSettings {
    bool use_default_mute_for = false;
    std::int32_t mute_for = 0;
    bool use_default_sound = false;
    std::int64_t sound_id = 0;
    bool use_default_show_preview = false;
    bool show_preview = false;
    bool use_default_mute_stories = false;
    bool mute_stories = false;
    bool use_default_story_sound = false;
    std::int64_t story_sound_id = 0;
    bool use_default_show_story_poster = false;
    bool show_story_poster = false;
    bool use_default_disable_pinned_message_notifications = false;
    bool disable_pinned_message_notifications = false;
    bool use_default_disable_mention_notifications = false;
    bool disable_mention_notifications = false;

    bool operator==(const TdChatNotificationSettings&) const = default;
};

enum class TdDirectChatList { Main, Archive };

struct TdEditMessageTextRequest {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::string text;

    bool operator==(const TdEditMessageTextRequest&) const = default;
};

struct TdDeleteMessagesRequest {
    std::int64_t chat_id = 0;
    std::vector<std::int64_t> message_ids;
    bool revoke = false;

    bool operator==(const TdDeleteMessagesRequest&) const = default;
};

struct TdMessageReactionRequest {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::string reaction;
    bool remove = false;
    bool big = false;

    bool operator==(const TdMessageReactionRequest&) const = default;
};

struct TdPinMessageRequest {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    bool pinned = false;

    bool operator==(const TdPinMessageRequest&) const = default;
};

struct TdViewMessagesRequest {
    std::int64_t chat_id = 0;
    std::vector<std::int64_t> message_ids;

    bool operator==(const TdViewMessagesRequest&) const = default;
};

struct TdSetChatNotificationSettingsRequest {
    std::int64_t chat_id = 0;
    TdChatNotificationSettings settings;

    bool operator==(const TdSetChatNotificationSettingsRequest&) const = default;
};

struct TdToggleChatIsPinnedRequest {
    std::int64_t chat_id = 0;
    TdDirectChatList list = TdDirectChatList::Main;
    bool pinned = false;

    bool operator==(const TdToggleChatIsPinnedRequest&) const = default;
};

struct TdAddChatToListRequest {
    std::int64_t chat_id = 0;
    TdDirectChatList list = TdDirectChatList::Main;

    bool operator==(const TdAddChatToListRequest&) const = default;
};

struct TdJoinChatRequest {
    TdJoinChatRequest(std::optional<std::int64_t> chat_id_value = {},
                      std::optional<secure::SensitiveString> invite_link_value = {},
                      std::optional<std::int64_t> expected_invite_chat_id_value = {});
    ~TdJoinChatRequest() = default;
    TdJoinChatRequest(const TdJoinChatRequest& other);
    TdJoinChatRequest& operator=(const TdJoinChatRequest& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    TdJoinChatRequest(TdJoinChatRequest&& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    TdJoinChatRequest& operator=(TdJoinChatRequest&& other);

    std::optional<std::int64_t>
        chat_id; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
    std::optional<std::int64_t> expected_invite_chat_id;

    [[nodiscard]] bool is_invite_request() const noexcept;
    [[nodiscard]] bool has_invite_link() const noexcept;
    [[nodiscard]] std::optional<std::string_view> invite_link() const noexcept;
    [[nodiscard]] const secure::WipeObserver& wipe_observer() const noexcept;
    void clear_invite_link();

    bool operator==(const TdJoinChatRequest& other) const {
        return chat_id == other.chat_id && invite_link() == other.invite_link() &&
               expected_invite_chat_id == other.expected_invite_chat_id;
    }

  private:
    std::optional<secure::SensitiveString> invite_link_;
    bool invite_request_ = false;
    secure::WipeObserver wipe_observer_;
};

struct TdLeaveChatRequest {
    std::int64_t chat_id = 0;

    bool operator==(const TdLeaveChatRequest&) const = default;
};

using TdDirectRequest =
    std::variant<TdEditMessageTextRequest, TdDeleteMessagesRequest, TdMessageReactionRequest,
                 TdPinMessageRequest, TdViewMessagesRequest, TdSetChatNotificationSettingsRequest,
                 TdToggleChatIsPinnedRequest, TdAddChatToListRequest, TdJoinChatRequest,
                 TdLeaveChatRequest, TdM6Request>;

[[nodiscard]] bool valid_td_message_locator(std::int64_t chat_id, std::int64_t message_id) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdEditMessageTextRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdDeleteMessagesRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdMessageReactionRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdPinMessageRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdViewMessagesRequest& request) noexcept;
[[nodiscard]] bool
valid_td_direct_request(const TdSetChatNotificationSettingsRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdToggleChatIsPinnedRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdAddChatToListRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdJoinChatRequest& request);
[[nodiscard]] bool valid_td_direct_request(const TdLeaveChatRequest& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdM6Request& request) noexcept;
[[nodiscard]] bool valid_td_direct_request(const TdDirectRequest& request);

enum class TdChatJoinResultKind {
    Success,
    RequestSent,
    GuardBotApprovalRequired,
    Declined,
    Unknown
};

struct TdChatJoinResult {
    TdChatJoinResultKind kind = TdChatJoinResultKind::Unknown;
    std::optional<std::int64_t> chat_id;
    std::optional<std::int64_t> guard_bot_user_id;
    std::optional<std::int64_t> guard_query_id;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const TdChatJoinResult&) const = default;
};

struct TdDirectConversionError {
    std::optional<std::int32_t> tdlib_type_id;

    bool operator==(const TdDirectConversionError&) const = default;
};

struct TdChatPosition {
    TdChatList list;
    std::int64_t order = 0;

    bool operator==(const TdChatPosition&) const = default;
};

struct TdChat {
    std::int64_t id = 0;
    std::string title;
    TdChatKind kind = TdChatKind::Unknown;
    std::int64_t related_id = 0;
    std::int32_t tdlib_type_id = 0;
    std::vector<TdChatPosition> positions;
    std::vector<TdChatList> chat_lists;
    bool is_marked_unread = false;
    std::int32_t unread_count = 0;
    std::int32_t unread_mention_count = 0;
    std::int32_t unread_reaction_count = 0;
    std::int32_t unread_poll_vote_count = 0;
    std::optional<TdMessageSummary> last_message;
    std::optional<TdChatNotificationSettings> notification_settings;

    bool operator==(const TdChat&) const = default;
};

struct TdChats {
    std::vector<std::int64_t> chat_ids;

    bool operator==(const TdChats&) const = default;
};

struct TdMessages {
    std::int32_t total_count = 0;
    std::vector<std::optional<TdMessageSummary>> messages;

    bool operator==(const TdMessages&) const = default;
};

struct TdMessageThreadInfo {
    std::int64_t history_chat_id = 0;
    std::int64_t history_thread_id = 0;
    std::vector<std::optional<TdMessageSummary>> starting_messages;

    bool operator==(const TdMessageThreadInfo&) const = default;
};

struct TdMessageLink {
    std::string link;
    bool is_public = false;

    bool operator==(const TdMessageLink&) const = default;
};

struct TdSupergroup {
    std::int64_t id = 0;
    std::vector<std::string> usernames;
    bool is_channel = false;
    bool is_forum = false;

    bool operator==(const TdSupergroup&) const = default;
};

struct TdBasicGroup {
    std::int64_t id = 0;
    std::int32_t member_count = 0;
    bool is_active = false;
    std::int64_t upgraded_to_supergroup_id = 0;

    bool operator==(const TdBasicGroup&) const = default;
};

struct TdUsers {
    std::int32_t total_count = 0;
    std::vector<std::int64_t> user_ids;

    bool operator==(const TdUsers&) const = default;
};

enum class TdChatMemberStatusKind {
    Creator,
    Administrator,
    Member,
    Restricted,
    Left,
    Banned,
    Unknown
};

struct TdChatMemberStatus {
    TdChatMemberStatusKind kind = TdChatMemberStatusKind::Unknown;
    bool is_member = false;
    std::optional<std::int32_t> unsupported_tdlib_type_id;

    bool operator==(const TdChatMemberStatus&) const = default;
};

struct TdChatMember {
    TdMessageSender member;
    std::string tag;
    std::int64_t inviter_user_id = 0;
    std::int32_t joined_chat_date = 0;
    TdChatMemberStatus status;

    bool operator==(const TdChatMember&) const = default;
};

struct TdBasicGroupFullInfo {
    std::string description;
    std::int64_t creator_user_id = 0;
    std::vector<TdChatMember> members;

    bool operator==(const TdBasicGroupFullInfo&) const = default;
};

struct TdChatMembers {
    std::int32_t total_count = 0;
    std::vector<TdChatMember> members;

    bool operator==(const TdChatMembers&) const = default;
};

struct TdMessageContent {
    TdMessageContentKind kind = TdMessageContentKind::Other;
    std::string text;
    std::int32_t tdlib_type_id = 0;

    bool operator==(const TdMessageContent&) const = default;
};

struct TdReactionSummary {
    TdReactionType reaction;
    std::int32_t total_count = 0;
    bool is_chosen = false;
    std::optional<TdMessageSender> used_sender;
    std::vector<TdMessageSender> recent_senders;

    bool operator==(const TdReactionSummary&) const = default;
};

struct TdReactionSnapshot {
    std::vector<TdReactionSummary> items;
    bool are_tags = false;
    bool can_get_added_reactions = false;

    bool operator==(const TdReactionSnapshot&) const = default;
};

struct TdReactionCount {
    TdReactionType reaction;
    std::int32_t total_count = 0;

    bool operator==(const TdReactionCount&) const = default;
};

enum class TdSupportedUpdateKind {
    CurrentStateEntry,
    NewMessage,
    MessageContent,
    MessageEdited,
    MessageInteractionInfo,
    MessageReaction,
    MessageReactions,
    DeleteMessages,
    User,
    BasicGroup,
    Supergroup,
    NewChat,
    ChatTitle,
    ChatLastMessage,
    ChatAddedToList,
    ChatRemovedFromList,
    ChatReadInbox,
    MessageMentionRead,
    MessageUnreadReactions,
    MessageContainsUnreadPollVotes,
    ChatUnreadMentionCount,
    ChatUnreadReactionCount,
    ChatUnreadPollVoteCount,
    ChatIsMarkedAsUnread,
};

enum class TdMalformedUpdateReason {
    MissingObject,
    InvalidIdentifier,
    InvalidCount,
    InvalidDate,
    InvalidContent,
    InvalidReaction,
    InvalidSender,
    InvalidChatList,
    InvalidEntity,
};

struct TdMalformedSupportedUpdate {
    TdSupportedUpdateKind kind = TdSupportedUpdateKind::NewMessage;
    TdMalformedUpdateReason reason = TdMalformedUpdateReason::MissingObject;
    std::int32_t tdlib_type_id = 0;

    bool operator==(const TdMalformedSupportedUpdate&) const = default;
};

struct TdUpdateNewMessage {
    TdMessageSummary message;

    bool operator==(const TdUpdateNewMessage&) const = default;
};

struct TdUpdateMessageContent {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    TdMessageContent content;

    bool operator==(const TdUpdateMessageContent&) const = default;
};

struct TdUpdateMessageEdited {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::int32_t edit_date = 0;
    bool has_reply_markup = false;

    bool operator==(const TdUpdateMessageEdited&) const = default;
};

struct TdUpdateMessageInteractionInfo {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::optional<TdReactionSnapshot> reactions;

    bool operator==(const TdUpdateMessageInteractionInfo&) const = default;
};

struct TdUpdateMessageReaction {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    TdMessageSender actor;
    std::int32_t date = 0;
    std::vector<TdReactionType> old_reactions;
    std::vector<TdReactionType> new_reactions;

    bool operator==(const TdUpdateMessageReaction&) const = default;
};

struct TdUpdateMessageReactions {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::int32_t date = 0;
    std::vector<TdReactionCount> reactions;

    bool operator==(const TdUpdateMessageReactions&) const = default;
};

struct TdUpdateUser {
    TdUserSummary user;

    bool operator==(const TdUpdateUser&) const = default;
};

struct TdUpdateBasicGroup {
    TdBasicGroup basic_group;

    bool operator==(const TdUpdateBasicGroup&) const = default;
};

struct TdUpdateSupergroup {
    TdSupergroup supergroup;

    bool operator==(const TdUpdateSupergroup&) const = default;
};

struct TdUpdateNewChat {
    TdChat chat;

    bool operator==(const TdUpdateNewChat&) const = default;
};

struct TdUpdateChatTitle {
    std::int64_t chat_id = 0;
    std::string title;

    bool operator==(const TdUpdateChatTitle&) const = default;
};

struct TdUpdateChatLastMessage {
    std::int64_t chat_id = 0;
    std::optional<TdMessageSummary> last_message;

    bool operator==(const TdUpdateChatLastMessage&) const = default;
};

struct TdUpdateChatAddedToList {
    std::int64_t chat_id = 0;
    TdChatList list;

    bool operator==(const TdUpdateChatAddedToList&) const = default;
};

struct TdUpdateChatRemovedFromList {
    std::int64_t chat_id = 0;
    TdChatList list;

    bool operator==(const TdUpdateChatRemovedFromList&) const = default;
};

struct TdUpdateChatReadInbox {
    std::int64_t chat_id = 0;
    std::int64_t last_read_inbox_message_id = 0;
    std::int32_t unread_count = 0;

    bool operator==(const TdUpdateChatReadInbox&) const = default;
};

struct TdUpdateMessageMentionRead {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::int32_t unread_mention_count = 0;

    bool operator==(const TdUpdateMessageMentionRead&) const = default;
};

struct TdUpdateMessageUnreadReactions {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::int32_t unread_reaction_count = 0;

    bool operator==(const TdUpdateMessageUnreadReactions&) const = default;
};

struct TdUpdateMessageContainsUnreadPollVotes {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    bool contains_unread_poll_votes = false;
    std::int32_t unread_poll_vote_count = 0;

    bool operator==(const TdUpdateMessageContainsUnreadPollVotes&) const = default;
};

struct TdUpdateChatUnreadMentionCount {
    std::int64_t chat_id = 0;
    std::int32_t unread_mention_count = 0;

    bool operator==(const TdUpdateChatUnreadMentionCount&) const = default;
};

struct TdUpdateChatUnreadReactionCount {
    std::int64_t chat_id = 0;
    std::int32_t unread_reaction_count = 0;

    bool operator==(const TdUpdateChatUnreadReactionCount&) const = default;
};

struct TdUpdateChatUnreadPollVoteCount {
    std::int64_t chat_id = 0;
    std::int32_t unread_poll_vote_count = 0;

    bool operator==(const TdUpdateChatUnreadPollVoteCount&) const = default;
};

struct TdUpdateChatIsMarkedAsUnread {
    std::int64_t chat_id = 0;
    bool is_marked_unread = false;

    bool operator==(const TdUpdateChatIsMarkedAsUnread&) const = default;
};

struct TdCurrentState {
    std::vector<TdValue> updates;
};

enum class TdInternalLinkKind {
    PublicChat,
    BotStart,
    Message,
    ChatInvite,
    DirectMessagesChat,
    SavedMessages,
    Unsupported,
};

struct TdInternalLink {
    TdInternalLinkKind kind = TdInternalLinkKind::Unsupported;
    std::string username;
    std::string url;
    std::int32_t tdlib_type_id = 0;

    bool operator==(const TdInternalLink&) const = default;
};

struct TdMessageLinkInfo {
    bool is_public = false;
    std::int64_t chat_id = 0;
    std::optional<std::int64_t> message_id;
    std::optional<TdTopic> topic;

    bool operator==(const TdMessageLinkInfo&) const = default;
};

struct TdChatInviteLinkInfo {
    std::int64_t chat_id = 0;
    bool is_public = false;

    bool operator==(const TdChatInviteLinkInfo&) const = default;
};

struct TdSupergroupFullInfo {
    std::int64_t direct_messages_chat_id = 0;

    bool operator==(const TdSupergroupFullInfo&) const = default;
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
    virtual TdValue make_get_current_state() = 0;
    virtual TdValue make_get_contacts() = 0;
    virtual TdValue make_m6_function(TdM6Request request) = 0;
    virtual TdValue make_set_tdlib_parameters(TdlibParameters parameters) = 0;
    virtual TdValue make_auth_function(TdAuthRequest request) = 0;
    virtual TdValue make_get_saved_messages_tags(std::int64_t saved_messages_topic_id) = 0;
    virtual TdValue make_search_saved_messages(TdSearchSavedMessagesRequest request) = 0;
    virtual TdValue make_get_active_sessions() = 0;
    virtual TdValue make_terminate_session(std::int64_t session_id) = 0;
    virtual TdValue make_get_chat(std::int64_t chat_id) = 0;
    virtual TdValue make_get_chat_history(std::int64_t chat_id, std::int64_t from_message_id,
                                          std::int32_t offset, std::int32_t limit,
                                          bool only_local) = 0;
    virtual TdValue make_get_chat_message_by_date(std::int64_t chat_id, std::int32_t date) = 0;
    virtual TdValue make_get_message_thread(std::int64_t chat_id, std::int64_t message_id) = 0;
    virtual TdValue make_get_forum_topic_history(std::int64_t chat_id, std::int32_t forum_topic_id,
                                                 std::int64_t from_message_id, std::int32_t offset,
                                                 std::int32_t limit) = 0;
    virtual TdValue make_get_message_thread_history(std::int64_t chat_id, std::int64_t message_id,
                                                    std::int64_t from_message_id,
                                                    std::int32_t offset, std::int32_t limit) = 0;
    virtual TdValue make_get_direct_messages_chat_topic_history(std::int64_t chat_id,
                                                                std::int64_t topic_id,
                                                                std::int64_t from_message_id,
                                                                std::int32_t offset,
                                                                std::int32_t limit) = 0;
    virtual TdValue make_get_saved_messages_topic_history(std::int64_t topic_id,
                                                          std::int64_t from_message_id,
                                                          std::int32_t offset,
                                                          std::int32_t limit) = 0;
    virtual TdValue make_get_messages(std::int64_t chat_id,
                                      std::vector<std::int64_t> message_ids) = 0;
    virtual TdValue make_get_message_link(std::int64_t chat_id, std::int64_t message_id,
                                          std::int32_t media_timestamp,
                                          std::int32_t checklist_task_id,
                                          std::string poll_option_id, bool for_album,
                                          bool in_message_thread) = 0;
    virtual TdValue make_get_chats(TdChatList list, std::int32_t limit) = 0;
    virtual TdValue make_load_chats(TdChatList list, std::int32_t limit) = 0;
    virtual TdValue make_search_public_chat(std::string username) = 0;
    virtual TdValue make_get_internal_link_type(std::string_view link, bool sensitive = false,
                                                const secure::WipeObserver& wipe_observer = {}) = 0;
    virtual TdValue make_get_message_link_info(std::string url) = 0;
    virtual TdValue make_check_chat_invite_link(std::string_view link,
                                                const secure::WipeObserver& wipe_observer = {}) = 0;
    virtual TdValue make_get_user(std::int64_t user_id) = 0;
    virtual TdValue make_get_basic_group_full_info(std::int64_t basic_group_id) = 0;
    virtual TdValue make_get_supergroup(std::int64_t supergroup_id) = 0;
    virtual TdValue make_get_supergroup_full_info(std::int64_t supergroup_id) = 0;
    virtual TdValue make_get_supergroup_members(std::int64_t supergroup_id, std::string query,
                                                std::int32_t offset, std::int32_t limit) = 0;
    virtual TdValue make_create_private_chat(std::int64_t user_id, bool force) = 0;
    virtual TdValue make_get_message(std::int64_t chat_id, std::int64_t message_id) = 0;
    virtual TdValue make_get_message_properties(std::int64_t chat_id, std::int64_t message_id) = 0;
    virtual TdValue make_get_message_available_reactions(std::int64_t chat_id,
                                                         std::int64_t message_id) = 0;
    virtual TdValue make_get_unix_time() = 0;
    virtual TdValue make_parse_text_entities(std::string text, TdTextParseMode mode) = 0;
    virtual TdValue make_send_message(TdSendMessageRequest request,
                                      std::uint64_t client_generation) = 0;
    virtual TdValue make_forward_messages(TdForwardMessagesRequest request) = 0;
    virtual TdValue make_edit_message_text(TdEditMessageTextRequest request) = 0;
    virtual TdValue make_delete_messages(TdDeleteMessagesRequest request) = 0;
    virtual TdValue make_message_reaction(TdMessageReactionRequest request) = 0;
    virtual TdValue make_pin_message(TdPinMessageRequest request) = 0;
    virtual TdValue make_view_messages(TdViewMessagesRequest request) = 0;
    virtual TdValue
    make_set_chat_notification_settings(TdSetChatNotificationSettingsRequest request) = 0;
    virtual TdValue make_toggle_chat_is_pinned(TdToggleChatIsPinnedRequest request) = 0;
    virtual TdValue make_add_chat_to_list(TdAddChatToListRequest request) = 0;
    virtual TdValue make_join_chat(TdJoinChatRequest request) = 0;
    virtual TdValue make_leave_chat(TdLeaveChatRequest request) = 0;
    virtual void send(std::int32_t client_id, std::uint64_t client_generation,
                      std::uint64_t query_id, TdValue& function) = 0;
    virtual std::optional<TdRuntimeEvent> receive(std::chrono::milliseconds timeout) = 0;
};

std::unique_ptr<TdRuntime> make_production_td_runtime();
std::string production_tdlib_version();

} // namespace tgcli::core
