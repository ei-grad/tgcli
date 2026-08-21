#include "core/td_runtime.hpp"

#include "core/td_runtime_test_adapter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <climits>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#include <td/telegram/Client.h>
#include <td/telegram/Log.h>
#include <td/telegram/td_api.h>

namespace tgcli::core {

namespace td_api = td::td_api;

namespace {

using NativeFunctionPtr = td_api::object_ptr<td_api::Function>;
using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

void wipe(std::string& value) {
    volatile char* bytes = value.data();
    for (std::size_t index = 0; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
    value.clear();
}

void transfer(std::string& source, std::string& destination) {
    class SourceWiper {
      public:
        explicit SourceWiper(std::string* value) : value_(value) {}
        ~SourceWiper() {
            wipe(*value_);
        }
        SourceWiper(const SourceWiper&) = delete;
        SourceWiper& operator=(const SourceWiper&) = delete;
        SourceWiper(SourceWiper&&) = delete;
        SourceWiper& operator=(SourceWiper&&) = delete;

      private:
        std::string* value_;
    };
    const SourceWiper source_wiper{&source};
    wipe(destination);
    destination.assign(source);
}

TdValue make_native_auth_function(TdAuthRequest request) {
    NativeFunctionPtr native;
    std::vector<TdFunctionField> fields;
    switch (request.function) {
    case TdFunctionKind::SetAuthenticationPhoneNumber: {
        auto settings = td_api::make_object<td_api::phoneNumberAuthenticationSettings>();
        settings->allow_flash_call_ = false;
        settings->allow_missed_call_ = false;
        settings->is_current_phone_number_ = false;
        settings->has_unknown_phone_number_ = false;
        settings->allow_sms_retriever_api_ = false;
        settings->firebase_authentication_settings_ = nullptr;
        settings->authentication_tokens_.clear();
        native = td_api::make_object<td_api::setAuthenticationPhoneNumber>(request.value,
                                                                           std::move(settings));
        wipe(request.value);
        fields = {{"phone_number", TdRedactedValue::Credential},
                  {"settings_allow_flash_call", false},
                  {"settings_allow_missed_call", false},
                  {"settings_is_current_phone_number", false},
                  {"settings_has_unknown_phone_number", false},
                  {"settings_allow_sms_retriever_api", false},
                  {"settings_has_firebase", false},
                  {"settings_token_count", std::int64_t{0}}};
        break;
    }
    case TdFunctionKind::RequestQrCodeAuthentication:
        native =
            td_api::make_object<td_api::requestQrCodeAuthentication>(std::vector<std::int64_t>{});
        fields = {{"other_user_ids", std::vector<std::int64_t>{}}};
        break;
    case TdFunctionKind::CheckAuthenticationBotToken:
        native = td_api::make_object<td_api::checkAuthenticationBotToken>(request.value);
        wipe(request.value);
        fields = {{"credential", TdRedactedValue::Credential}};
        break;
    case TdFunctionKind::SetAuthenticationEmailAddress:
        native = td_api::make_object<td_api::setAuthenticationEmailAddress>(request.value);
        wipe(request.value);
        fields = {{"credential", TdRedactedValue::Credential}};
        break;
    case TdFunctionKind::CheckAuthenticationEmailCode: {
        auto code = td_api::make_object<td_api::emailAddressAuthenticationCode>();
        code->code_ = request.value;
        wipe(request.value);
        native = td_api::make_object<td_api::checkAuthenticationEmailCode>(std::move(code));
        fields = {{"credential", TdRedactedValue::Credential}};
        break;
    }
    case TdFunctionKind::CheckAuthenticationCode:
        native = td_api::make_object<td_api::checkAuthenticationCode>(request.value);
        wipe(request.value);
        fields = {{"credential", TdRedactedValue::Credential}};
        break;
    case TdFunctionKind::RegisterUser:
        native = td_api::make_object<td_api::registerUser>(request.value, request.secondary, false);
        wipe(request.value);
        wipe(request.secondary);
        fields = {{"first_name", TdRedactedValue::Credential},
                  {"last_name", TdRedactedValue::Credential},
                  {"disable_notification", false}};
        break;
    case TdFunctionKind::CheckAuthenticationPassword:
        native = td_api::make_object<td_api::checkAuthenticationPassword>(request.value);
        wipe(request.value);
        fields = {{"credential", TdRedactedValue::Credential}};
        break;
    case TdFunctionKind::GetMe:
        native = td_api::make_object<td_api::getMe>();
        break;
    default:
        throw std::invalid_argument("unsupported authentication function factory request");
    }
    return TdValue::function(std::move(native),
                             TdFunctionData{request.function, std::move(fields)});
}

td_api::object_ptr<td_api::ReactionType> make_native_reaction(const TdReactionType& reaction) {
    switch (reaction.kind) {
    case TdReactionKind::Emoji:
        return td_api::make_object<td_api::reactionTypeEmoji>(reaction.emoji);
    case TdReactionKind::CustomEmoji:
        return td_api::make_object<td_api::reactionTypeCustomEmoji>(reaction.custom_emoji_id);
    case TdReactionKind::Paid:
    case TdReactionKind::Unknown:
        throw std::invalid_argument("unsupported Saved Messages reaction selector");
    }
    throw std::invalid_argument("unsupported Saved Messages reaction selector");
}

TdReactionType convert_reaction(const td_api::ReactionType* reaction) {
    if (reaction == nullptr) {
        return {
            .kind = TdReactionKind::Unknown, .emoji = {}, .custom_emoji_id = 0, .tdlib_type_id = 0};
    }
    switch (reaction->get_id()) {
    case td_api::reactionTypeEmoji::ID:
        return {.kind = TdReactionKind::Emoji,
                .emoji = static_cast<const td_api::reactionTypeEmoji&>(*reaction).emoji_,
                .custom_emoji_id = 0,
                .tdlib_type_id = reaction->get_id()};
    case td_api::reactionTypeCustomEmoji::ID:
        return {.kind = TdReactionKind::CustomEmoji,
                .emoji = {},
                .custom_emoji_id =
                    static_cast<const td_api::reactionTypeCustomEmoji&>(*reaction).custom_emoji_id_,
                .tdlib_type_id = reaction->get_id()};
    case td_api::reactionTypePaid::ID:
        return {.kind = TdReactionKind::Paid,
                .emoji = {},
                .custom_emoji_id = 0,
                .tdlib_type_id = reaction->get_id()};
    default:
        return {.kind = TdReactionKind::Unknown,
                .emoji = {},
                .custom_emoji_id = 0,
                .tdlib_type_id = reaction->get_id()};
    }
}

TdSavedMessageSummary convert_saved_message(const td_api::message& message) {
    std::string text;
    if (message.content_ != nullptr && message.content_->get_id() == td_api::messageText::ID) {
        const auto& content = static_cast<const td_api::messageText&>(*message.content_);
        if (content.text_ != nullptr) {
            text = content.text_->text_;
        }
    }
    return {.id = message.id_,
            .chat_id = message.chat_id_,
            .date = message.date_,
            .text = std::move(text)};
}

bool valid_utf8(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7F) {
            length = 1;
            codepoint = first;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return false;
        }
        index += length;
    }
    return true;
}

bool valid_persistable_message_text(std::string_view value) {
    if (value.size() > 16'384 || !valid_utf8(value)) {
        return false;
    }
    std::size_t scalar_count = 0;
    for (const auto byte : value) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U && ++scalar_count > 4'096) {
            return false;
        }
    }
    return true;
}

constexpr std::size_t kMaxSessionCount = 4'096;
constexpr std::size_t kMaxSessionStringBytes = 1'048'576;
constexpr std::size_t kMaxSessionListResultBytes = 16'842'751;

bool add_session_result_bytes(std::size_t& size, std::size_t amount) {
    if (amount > kMaxSessionListResultBytes - size) {
        return false;
    }
    size += amount;
    return true;
}

bool add_session_result_literal(std::size_t& size, std::string_view literal) {
    return add_session_result_bytes(size, literal.size());
}

bool add_session_result_json_string(std::size_t& size, std::string_view value,
                                    bool enforce_td_string_limit) {
    if ((enforce_td_string_limit && value.size() > kMaxSessionStringBytes) || !valid_utf8(value) ||
        !add_session_result_bytes(size, 2)) {
        return false;
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        std::size_t encoded_size = 1;
        switch (byte) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            encoded_size = 2;
            break;
        default:
            if (byte <= 0x1FU) {
                encoded_size = 6;
            }
            break;
        }
        if (!add_session_result_bytes(size, encoded_size)) {
            return false;
        }
    }
    return true;
}

template <typename Integer> bool add_session_result_integer(std::size_t& size, Integer value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return false;
    }
    return add_session_result_bytes(size, static_cast<std::size_t>(result.ptr - buffer.data()));
}

bool add_session_result_boolean(std::size_t& size, bool value) {
    return add_session_result_literal(size, value ? "true" : "false");
}

bool add_session_result_timestamp(std::size_t& size, const std::optional<std::string>& timestamp) {
    if (!timestamp) {
        return add_session_result_literal(size, "null");
    }
    return add_session_result_json_string(size, *timestamp, false);
}

std::optional<TdSessionDeviceType>
convert_session_device_type(const td_api::SessionDeviceType* device,
                            std::optional<std::int32_t>& unsupported_type_id) {
    if (device == nullptr) {
        return std::nullopt;
    }
    switch (device->get_id()) {
    case td_api::sessionDeviceTypeAndroid::ID:
        return TdSessionDeviceType::Android;
    case td_api::sessionDeviceTypeApple::ID:
        return TdSessionDeviceType::Apple;
    case td_api::sessionDeviceTypeBrave::ID:
        return TdSessionDeviceType::Brave;
    case td_api::sessionDeviceTypeChrome::ID:
        return TdSessionDeviceType::Chrome;
    case td_api::sessionDeviceTypeEdge::ID:
        return TdSessionDeviceType::Edge;
    case td_api::sessionDeviceTypeFirefox::ID:
        return TdSessionDeviceType::Firefox;
    case td_api::sessionDeviceTypeIpad::ID:
        return TdSessionDeviceType::Ipad;
    case td_api::sessionDeviceTypeIphone::ID:
        return TdSessionDeviceType::Iphone;
    case td_api::sessionDeviceTypeLinux::ID:
        return TdSessionDeviceType::Linux;
    case td_api::sessionDeviceTypeMac::ID:
        return TdSessionDeviceType::Mac;
    case td_api::sessionDeviceTypeOpera::ID:
        return TdSessionDeviceType::Opera;
    case td_api::sessionDeviceTypeSafari::ID:
        return TdSessionDeviceType::Safari;
    case td_api::sessionDeviceTypeUbuntu::ID:
        return TdSessionDeviceType::Ubuntu;
    case td_api::sessionDeviceTypeUnknown::ID:
        return TdSessionDeviceType::Unknown;
    case td_api::sessionDeviceTypeVivaldi::ID:
        return TdSessionDeviceType::Vivaldi;
    case td_api::sessionDeviceTypeWindows::ID:
        return TdSessionDeviceType::Windows;
    case td_api::sessionDeviceTypeXbox::ID:
        return TdSessionDeviceType::Xbox;
    default:
        unsupported_type_id = device->get_id();
        return std::nullopt;
    }
}

bool convert_session_timestamp(std::int32_t seconds, std::optional<std::string>& output) {
    if (seconds < 0) {
        return false;
    }
    if (seconds == 0) {
        output.reset();
        return true;
    }
    const auto value = static_cast<std::time_t>(seconds);
    std::tm utc{};
    if (::gmtime_r(&value, &utc) == nullptr) {
        return false;
    }
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return false;
    }
    output = buffer.data();
    return true;
}

bool add_compact_session(std::size_t& size, const td_api::session& session,
                         TdSessionDeviceType device_type, std::string_view id,
                         const std::optional<std::string>& log_in_date,
                         const std::optional<std::string>& last_active_date) {
    return add_session_result_literal(size, R"({"id":)") &&
           add_session_result_json_string(size, id, false) &&
           add_session_result_literal(size, R"(,"is_current":)") &&
           add_session_result_boolean(size, session.is_current_) &&
           add_session_result_literal(size, R"(,"is_password_pending":)") &&
           add_session_result_boolean(size, session.is_password_pending_) &&
           add_session_result_literal(size, R"(,"is_unconfirmed":)") &&
           add_session_result_boolean(size, session.is_unconfirmed_) &&
           add_session_result_literal(size, R"(,"can_accept_secret_chats":)") &&
           add_session_result_boolean(size, session.can_accept_secret_chats_) &&
           add_session_result_literal(size, R"(,"can_accept_calls":)") &&
           add_session_result_boolean(size, session.can_accept_calls_) &&
           add_session_result_literal(size, R"(,"device_type":)") &&
           add_session_result_json_string(size, td_session_device_type_name(device_type), false) &&
           add_session_result_literal(size, R"(,"api_id":)") &&
           add_session_result_integer(size, session.api_id_) &&
           add_session_result_literal(size, R"(,"application_name":)") &&
           add_session_result_json_string(size, session.application_name_, true) &&
           add_session_result_literal(size, R"(,"application_version":)") &&
           add_session_result_json_string(size, session.application_version_, true) &&
           add_session_result_literal(size, R"(,"is_official_application":)") &&
           add_session_result_boolean(size, session.is_official_application_) &&
           add_session_result_literal(size, R"(,"device_model":)") &&
           add_session_result_json_string(size, session.device_model_, true) &&
           add_session_result_literal(size, R"(,"platform":)") &&
           add_session_result_json_string(size, session.platform_, true) &&
           add_session_result_literal(size, R"(,"system_version":)") &&
           add_session_result_json_string(size, session.system_version_, true) &&
           add_session_result_literal(size, R"(,"log_in_date":)") &&
           add_session_result_timestamp(size, log_in_date) &&
           add_session_result_literal(size, R"(,"last_active_date":)") &&
           add_session_result_timestamp(size, last_active_date) &&
           add_session_result_literal(size, R"(,"ip_address":)") &&
           add_session_result_json_string(size, session.ip_address_, true) &&
           add_session_result_literal(size, R"(,"location":)") &&
           add_session_result_json_string(size, session.location_, true) &&
           add_session_result_literal(size, "}");
}

TdValue session_conversion_error(std::optional<std::int32_t> tdlib_type_id = std::nullopt) {
    return TdValue::from(TdSessionConversionError{tdlib_type_id});
}

TdValue convert_sessions(td_api::object_ptr<td_api::sessions> sessions) {
    if (sessions == nullptr || sessions->inactive_session_ttl_days_ < 1 ||
        sessions->inactive_session_ttl_days_ > 366 ||
        sessions->sessions_.size() > kMaxSessionCount) {
        return session_conversion_error();
    }

    TdSessions converted;
    converted.inactive_session_ttl_days = sessions->inactive_session_ttl_days_;
    converted.items.reserve(sessions->sessions_.size());
    std::unordered_set<std::int64_t> ids;
    ids.reserve(sessions->sessions_.size());
    std::size_t compact_result_size = 0;
    if (!add_session_result_literal(compact_result_size, R"({"items":[)")) {
        return session_conversion_error();
    }
    for (auto& item : sessions->sessions_) {
        if (item == nullptr || !ids.emplace(item->id_).second) {
            return session_conversion_error();
        }
        std::optional<std::int32_t> unsupported_type_id;
        const auto device_type =
            convert_session_device_type(item->device_type_.get(), unsupported_type_id);
        if (!device_type) {
            return session_conversion_error(unsupported_type_id);
        }
        std::optional<std::string> log_in_date;
        std::optional<std::string> last_active_date;
        if (!convert_session_timestamp(item->log_in_date_, log_in_date) ||
            !convert_session_timestamp(item->last_active_date_, last_active_date)) {
            return session_conversion_error();
        }
        const auto id = std::to_string(item->id_);
        if ((!converted.items.empty() && !add_session_result_literal(compact_result_size, ",")) ||
            !add_compact_session(compact_result_size, *item, *device_type, id, log_in_date,
                                 last_active_date)) {
            return session_conversion_error();
        }
        converted.items.push_back({.id = id,
                                   .is_current = item->is_current_,
                                   .is_password_pending = item->is_password_pending_,
                                   .is_unconfirmed = item->is_unconfirmed_,
                                   .can_accept_secret_chats = item->can_accept_secret_chats_,
                                   .can_accept_calls = item->can_accept_calls_,
                                   .device_type = *device_type,
                                   .api_id = item->api_id_,
                                   .application_name = std::move(item->application_name_),
                                   .application_version = std::move(item->application_version_),
                                   .is_official_application = item->is_official_application_,
                                   .device_model = std::move(item->device_model_),
                                   .platform = std::move(item->platform_),
                                   .system_version = std::move(item->system_version_),
                                   .log_in_date = std::move(log_in_date),
                                   .last_active_date = std::move(last_active_date),
                                   .ip_address = std::move(item->ip_address_),
                                   .location = std::move(item->location_)});
    }
    if (!add_session_result_literal(compact_result_size, R"(],"inactive_session_ttl_days":)") ||
        !add_session_result_integer(compact_result_size, sessions->inactive_session_ttl_days_) ||
        !add_session_result_literal(compact_result_size, R"(,"next":null})")) {
        return session_conversion_error();
    }
    return TdValue::from(std::move(converted));
}

TdValue make_native_get_active_sessions() {
    NativeFunctionPtr native = td_api::make_object<td_api::getActiveSessions>();
    return TdValue::function(std::move(native), TdFunctionData{TdFunctionKind::GetActiveSessions});
}

TdValue make_native_terminate_session(std::int64_t session_id) {
    NativeFunctionPtr native = td_api::make_object<td_api::terminateSession>(session_id);
    return TdValue::function(std::move(native), TdFunctionData{TdFunctionKind::TerminateSession,
                                                               {{"session_id", session_id}}});
}

TdValue make_native_get_messages(std::int64_t chat_id, std::vector<std::int64_t> message_ids) {
    const auto descriptor_ids = message_ids;
    NativeFunctionPtr native =
        td_api::make_object<td_api::getMessages>(chat_id, std::move(message_ids));
    return TdValue::function(
        std::move(native), TdFunctionData{TdFunctionKind::GetMessages,
                                          {{"chat_id", chat_id}, {"message_ids", descriptor_ids}}});
}

TdValue make_native_get_chat_history(std::int64_t chat_id, std::int64_t from_message_id,
                                     std::int32_t offset, std::int32_t limit, bool only_local) {
    NativeFunctionPtr native = td_api::make_object<td_api::getChatHistory>(
        chat_id, from_message_id, offset, limit, only_local);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::GetChatHistory,
                                            {{"chat_id", chat_id},
                                             {"from_message_id", from_message_id},
                                             {"offset", static_cast<std::int64_t>(offset)},
                                             {"limit", static_cast<std::int64_t>(limit)},
                                             {"only_local", only_local}}});
}

TdValue make_native_get_chat_message_by_date(std::int64_t chat_id, std::int32_t date) {
    NativeFunctionPtr native = td_api::make_object<td_api::getChatMessageByDate>(chat_id, date);
    return TdValue::function(
        std::move(native),
        TdFunctionData{TdFunctionKind::GetChatMessageByDate,
                       {{"chat_id", chat_id}, {"date", static_cast<std::int64_t>(date)}}});
}

TdValue make_native_get_message_thread(std::int64_t chat_id, std::int64_t message_id) {
    NativeFunctionPtr native = td_api::make_object<td_api::getMessageThread>(chat_id, message_id);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::GetMessageThread,
                                            {{"chat_id", chat_id}, {"message_id", message_id}}});
}

TdValue make_native_get_forum_topic_history(std::int64_t chat_id, std::int32_t forum_topic_id,
                                            std::int64_t from_message_id, std::int32_t offset,
                                            std::int32_t limit) {
    NativeFunctionPtr native = td_api::make_object<td_api::getForumTopicHistory>(
        chat_id, forum_topic_id, from_message_id, offset, limit);
    return TdValue::function(
        std::move(native),
        TdFunctionData{TdFunctionKind::GetForumTopicHistory,
                       {{"chat_id", chat_id},
                        {"forum_topic_id", static_cast<std::int64_t>(forum_topic_id)},
                        {"from_message_id", from_message_id},
                        {"offset", static_cast<std::int64_t>(offset)},
                        {"limit", static_cast<std::int64_t>(limit)}}});
}

TdValue make_native_get_message_thread_history(std::int64_t chat_id, std::int64_t message_id,
                                               std::int64_t from_message_id, std::int32_t offset,
                                               std::int32_t limit) {
    NativeFunctionPtr native = td_api::make_object<td_api::getMessageThreadHistory>(
        chat_id, message_id, from_message_id, offset, limit);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::GetMessageThreadHistory,
                                            {{"chat_id", chat_id},
                                             {"message_id", message_id},
                                             {"from_message_id", from_message_id},
                                             {"offset", static_cast<std::int64_t>(offset)},
                                             {"limit", static_cast<std::int64_t>(limit)}}});
}

TdValue make_native_get_direct_messages_chat_topic_history(std::int64_t chat_id,
                                                           std::int64_t topic_id,
                                                           std::int64_t from_message_id,
                                                           std::int32_t offset,
                                                           std::int32_t limit) {
    NativeFunctionPtr native = td_api::make_object<td_api::getDirectMessagesChatTopicHistory>(
        chat_id, topic_id, from_message_id, offset, limit);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::GetDirectMessagesChatTopicHistory,
                                            {{"chat_id", chat_id},
                                             {"topic_id", topic_id},
                                             {"from_message_id", from_message_id},
                                             {"offset", static_cast<std::int64_t>(offset)},
                                             {"limit", static_cast<std::int64_t>(limit)}}});
}

TdValue make_native_get_saved_messages_topic_history(std::int64_t topic_id,
                                                     std::int64_t from_message_id,
                                                     std::int32_t offset, std::int32_t limit) {
    NativeFunctionPtr native = td_api::make_object<td_api::getSavedMessagesTopicHistory>(
        topic_id, from_message_id, offset, limit);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::GetSavedMessagesTopicHistory,
                                            {{"saved_messages_topic_id", topic_id},
                                             {"from_message_id", from_message_id},
                                             {"offset", static_cast<std::int64_t>(offset)},
                                             {"limit", static_cast<std::int64_t>(limit)}}});
}

TdValue make_native_get_message_link(std::int64_t chat_id, std::int64_t message_id,
                                     std::int32_t media_timestamp, std::int32_t checklist_task_id,
                                     std::string poll_option_id, bool for_album,
                                     bool in_message_thread) {
    NativeFunctionPtr native = td_api::make_object<td_api::getMessageLink>(
        chat_id, message_id, media_timestamp, checklist_task_id, poll_option_id, for_album,
        in_message_thread);
    return TdValue::function(
        std::move(native),
        TdFunctionData{TdFunctionKind::GetMessageLink,
                       {{"chat_id", chat_id},
                        {"message_id", message_id},
                        {"media_timestamp", static_cast<std::int64_t>(media_timestamp)},
                        {"checklist_task_id", static_cast<std::int64_t>(checklist_task_id)},
                        {"poll_option_id", std::move(poll_option_id)},
                        {"for_album", for_album},
                        {"in_message_thread", in_message_thread}}});
}

td_api::object_ptr<td_api::ChatList> make_chat_list(const TdChatList& list) {
    switch (list.kind) {
    case TdChatListKind::Main:
        return td_api::make_object<td_api::chatListMain>();
    case TdChatListKind::Archive:
        return td_api::make_object<td_api::chatListArchive>();
    case TdChatListKind::Folder:
        if (list.folder_id <= 0) {
            throw std::invalid_argument("chat folder id must be positive");
        }
        return td_api::make_object<td_api::chatListFolder>(list.folder_id);
    case TdChatListKind::Unknown:
        break;
    }
    throw std::invalid_argument("unsupported chat list");
}

std::string_view chat_list_name(TdChatListKind list) {
    switch (list) {
    case TdChatListKind::Main:
        return "main";
    case TdChatListKind::Archive:
        return "archive";
    case TdChatListKind::Folder:
        return "folder";
    case TdChatListKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

TdChatList convert_chat_list(const td_api::ChatList* list) {
    if (list == nullptr) {
        return {};
    }
    switch (list->get_id()) {
    case td_api::chatListMain::ID:
        return {.kind = TdChatListKind::Main, .folder_id = 0, .tdlib_type_id = list->get_id()};
    case td_api::chatListArchive::ID:
        return {.kind = TdChatListKind::Archive, .folder_id = 0, .tdlib_type_id = list->get_id()};
    case td_api::chatListFolder::ID:
        return {.kind = TdChatListKind::Folder,
                .folder_id = static_cast<const td_api::chatListFolder&>(*list).chat_folder_id_,
                .tdlib_type_id = list->get_id()};
    default:
        return {.kind = TdChatListKind::Unknown, .folder_id = 0, .tdlib_type_id = list->get_id()};
    }
}

TdTopic convert_topic(const td_api::MessageTopic& topic);

TdMessageSender convert_message_sender(const td_api::MessageSender* sender) {
    if (sender == nullptr) {
        return {};
    }
    switch (sender->get_id()) {
    case td_api::messageSenderUser::ID:
        return {.kind = TdMessageSenderKind::User,
                .id = static_cast<const td_api::messageSenderUser&>(*sender).user_id_,
                .tdlib_type_id = sender->get_id()};
    case td_api::messageSenderChat::ID:
        return {.kind = TdMessageSenderKind::Chat,
                .id = static_cast<const td_api::messageSenderChat&>(*sender).chat_id_,
                .tdlib_type_id = sender->get_id()};
    default:
        return {.kind = TdMessageSenderKind::Unknown, .id = 0, .tdlib_type_id = sender->get_id()};
    }
}

std::string formatted_text(const td_api::formattedText* text) {
    return text == nullptr ? std::string{} : text->text_;
}

TdMessageSummary convert_message(const td_api::message& message) {
    TdMessageSummary converted{.id = message.id_,
                               .chat_id = message.chat_id_,
                               .date = message.date_,
                               .sender = convert_message_sender(message.sender_id_.get()),
                               .is_outgoing = message.is_outgoing_,
                               .topic = std::nullopt,
                               .content_kind = TdMessageContentKind::Other,
                               .text = {}};
    if (message.topic_id_ != nullptr) {
        converted.topic = convert_topic(*message.topic_id_);
    }
    if (message.content_ == nullptr) {
        return converted;
    }
    switch (message.content_->get_id()) {
    case td_api::messageText::ID: {
        const auto& content = static_cast<const td_api::messageText&>(*message.content_);
        converted.content_kind = TdMessageContentKind::Text;
        converted.text = formatted_text(content.text_.get());
        break;
    }
    case td_api::messageAnimatedEmoji::ID:
        converted.content_kind = TdMessageContentKind::Text;
        converted.text = static_cast<const td_api::messageAnimatedEmoji&>(*message.content_).emoji_;
        break;
    case td_api::messagePhoto::ID: {
        const auto& content = static_cast<const td_api::messagePhoto&>(*message.content_);
        converted.content_kind = TdMessageContentKind::Photo;
        converted.text = formatted_text(content.caption_.get());
        break;
    }
    case td_api::messageVideo::ID: {
        const auto& content = static_cast<const td_api::messageVideo&>(*message.content_);
        converted.content_kind = TdMessageContentKind::Video;
        converted.text = formatted_text(content.caption_.get());
        break;
    }
    case td_api::messageDocument::ID: {
        const auto& content = static_cast<const td_api::messageDocument&>(*message.content_);
        converted.content_kind = TdMessageContentKind::Document;
        converted.text = formatted_text(content.caption_.get());
        break;
    }
    case td_api::messageVoiceNote::ID: {
        const auto& content = static_cast<const td_api::messageVoiceNote&>(*message.content_);
        converted.content_kind = TdMessageContentKind::Voice;
        converted.text = formatted_text(content.caption_.get());
        break;
    }
    default:
        break;
    }
    return converted;
}

TdMessageSendingState convert_message_sending_state(const td_api::MessageSendingState* state) {
    if (state == nullptr) {
        return {};
    }
    switch (state->get_id()) {
    case td_api::messageSendingStatePending::ID:
        return {.kind = TdMessageSendingStateKind::Pending,
                .sending_id =
                    static_cast<const td_api::messageSendingStatePending&>(*state).sending_id_,
                .error = std::nullopt,
                .can_retry = false,
                .need_another_sender = false,
                .need_another_reply_quote = false,
                .need_drop_reply = false,
                .required_paid_message_star_count = 0,
                .retry_after = 0,
                .unsupported_tdlib_type_id = std::nullopt};
    case td_api::messageSendingStateFailed::ID: {
        const auto& failed = static_cast<const td_api::messageSendingStateFailed&>(*state);
        std::optional<TdError> error;
        if (failed.error_ != nullptr) {
            error = TdError{.code = failed.error_->code_, .message = failed.error_->message_};
        }
        return {.kind = TdMessageSendingStateKind::Failed,
                .sending_id = 0,
                .error = std::move(error),
                .can_retry = failed.can_retry_,
                .need_another_sender = failed.need_another_sender_,
                .need_another_reply_quote = failed.need_another_reply_quote_,
                .need_drop_reply = failed.need_drop_reply_,
                .required_paid_message_star_count = failed.required_paid_message_star_count_,
                .retry_after = failed.retry_after_,
                .unsupported_tdlib_type_id = std::nullopt};
    }
    default:
        return {.kind = TdMessageSendingStateKind::Unknown,
                .sending_id = 0,
                .error = std::nullopt,
                .can_retry = false,
                .need_another_sender = false,
                .need_another_reply_quote = false,
                .need_drop_reply = false,
                .required_paid_message_star_count = 0,
                .retry_after = 0,
                .unsupported_tdlib_type_id = state->get_id()};
    }
}

TdMessageSchedulingState
convert_message_scheduling_state(const td_api::MessageSchedulingState* state) {
    if (state == nullptr) {
        return {};
    }
    switch (state->get_id()) {
    case td_api::messageSchedulingStateSendAtDate::ID: {
        const auto& at = static_cast<const td_api::messageSchedulingStateSendAtDate&>(*state);
        return {.kind = TdMessageSchedulingStateKind::SendAtDate,
                .send_date = at.send_date_,
                .repeat_period = at.repeat_period_,
                .unsupported_tdlib_type_id = std::nullopt};
    }
    case td_api::messageSchedulingStateSendWhenOnline::ID:
        return {.kind = TdMessageSchedulingStateKind::SendWhenOnline,
                .send_date = 0,
                .repeat_period = 0,
                .unsupported_tdlib_type_id = std::nullopt};
    case td_api::messageSchedulingStateSendWhenVideoProcessed::ID:
        return {.kind = TdMessageSchedulingStateKind::SendWhenVideoProcessed,
                .send_date =
                    static_cast<const td_api::messageSchedulingStateSendWhenVideoProcessed&>(*state)
                        .send_date_,
                .repeat_period = 0,
                .unsupported_tdlib_type_id = std::nullopt};
    default:
        return {.kind = TdMessageSchedulingStateKind::Unknown,
                .send_date = 0,
                .repeat_period = 0,
                .unsupported_tdlib_type_id = state->get_id()};
    }
}

TdWriteMessage convert_write_message_details(const td_api::message& message) {
    return {.message = convert_message(message),
            .sending_state = convert_message_sending_state(message.sending_state_.get()),
            .scheduling_state = convert_message_scheduling_state(message.scheduling_state_.get()),
            .has_reply_markup = message.reply_markup_ != nullptr};
}

TdPlanningMessage convert_planning_message(const td_api::message& message) {
    auto summary = convert_message(message);
    return {.id = summary.id,
            .chat_id = summary.chat_id,
            .date = summary.date,
            .sender = summary.sender,
            .is_outgoing = summary.is_outgoing,
            .topic = summary.topic,
            .content_kind = summary.content_kind,
            .text = std::move(summary.text),
            .has_scheduling_state = message.scheduling_state_ != nullptr,
            .has_reply_markup = message.reply_markup_ != nullptr};
}

TdMessageWriteResult convert_write_message(const td_api::message& message) {
    auto summary = convert_message(message);
    const bool scheduled = message.scheduling_state_ != nullptr;
    return {.id = summary.id,
            .chat_id = summary.chat_id,
            .date = scheduled ? std::nullopt : std::optional<std::int32_t>{summary.date},
            .sender = summary.sender,
            .is_outgoing = summary.is_outgoing,
            .topic = summary.topic,
            .content_kind = summary.content_kind,
            .text = std::move(summary.text),
            .scheduled = scheduled};
}

TdMessageProperties convert_message_properties(const td_api::messageProperties& value) {
    return {.can_add_offer = value.can_add_offer_,
            .can_add_tasks = value.can_add_tasks_,
            .can_be_approved = value.can_be_approved_,
            .can_be_copied = value.can_be_copied_,
            .can_be_copied_to_secret_chat = value.can_be_copied_to_secret_chat_,
            .can_be_declined = value.can_be_declined_,
            .can_be_deleted_only_for_self = value.can_be_deleted_only_for_self_,
            .can_be_deleted_for_all_users = value.can_be_deleted_for_all_users_,
            .can_be_edited = value.can_be_edited_,
            .can_be_forwarded = value.can_be_forwarded_,
            .can_be_paid = value.can_be_paid_,
            .can_be_pinned = value.can_be_pinned_,
            .can_be_replied = value.can_be_replied_,
            .can_be_replied_in_another_chat = value.can_be_replied_in_another_chat_,
            .can_be_saved = value.can_be_saved_,
            .can_be_shared_in_story = value.can_be_shared_in_story_,
            .can_delete_reactions = value.can_delete_reactions_,
            .can_edit_media = value.can_edit_media_,
            .can_edit_scheduling_state = value.can_edit_scheduling_state_,
            .can_edit_suggested_post_info = value.can_edit_suggested_post_info_,
            .can_get_author = value.can_get_author_,
            .can_get_embedding_code = value.can_get_embedding_code_,
            .can_get_link = value.can_get_link_,
            .can_get_media_timestamp_links = value.can_get_media_timestamp_links_,
            .can_get_message_thread = value.can_get_message_thread_,
            .can_get_poll_vote_statistics = value.can_get_poll_vote_statistics_,
            .can_get_read_date = value.can_get_read_date_,
            .can_get_statistics = value.can_get_statistics_,
            .can_get_video_advertisements = value.can_get_video_advertisements_,
            .can_get_viewers = value.can_get_viewers_,
            .can_mark_tasks_as_done = value.can_mark_tasks_as_done_,
            .can_recognize_speech = value.can_recognize_speech_,
            .can_report_chat = value.can_report_chat_,
            .can_report_reactions = value.can_report_reactions_,
            .can_report_supergroup_spam = value.can_report_supergroup_spam_,
            .can_set_fact_check = value.can_set_fact_check_,
            .has_protected_content_by_current_user = value.has_protected_content_by_current_user_,
            .has_protected_content_by_other_user = value.has_protected_content_by_other_user_,
            .need_show_statistics = value.need_show_statistics_};
}

TdReactionUnavailabilityReason
convert_reaction_unavailability(const td_api::ReactionUnavailabilityReason* reason,
                                std::optional<std::int32_t>& unsupported) {
    if (reason == nullptr) {
        return TdReactionUnavailabilityReason::None;
    }
    switch (reason->get_id()) {
    case td_api::reactionUnavailabilityReasonAnonymousAdministrator::ID:
        return TdReactionUnavailabilityReason::AnonymousAdministrator;
    case td_api::reactionUnavailabilityReasonGuest::ID:
        return TdReactionUnavailabilityReason::Guest;
    case td_api::reactionUnavailabilityReasonRestricted::ID:
        return TdReactionUnavailabilityReason::Restricted;
    default:
        unsupported = reason->get_id();
        return TdReactionUnavailabilityReason::Unknown;
    }
}

std::optional<std::vector<TdAvailableReaction>> convert_available_reactions(
    const std::vector<td_api::object_ptr<td_api::availableReaction>>& values) {
    std::vector<TdAvailableReaction> converted;
    converted.reserve(values.size());
    for (const auto& value : values) {
        if (value == nullptr || value->type_ == nullptr) {
            return std::nullopt;
        }
        converted.push_back(
            {.type = convert_reaction(value->type_.get()), .needs_premium = value->needs_premium_});
    }
    return converted;
}

TdValue convert_message_available_reactions(const td_api::availableReactions& value) {
    auto top = convert_available_reactions(value.top_reactions_);
    auto recent = convert_available_reactions(value.recent_reactions_);
    auto popular = convert_available_reactions(value.popular_reactions_);
    if (!top || !recent || !popular) {
        return TdValue::from(TdDirectConversionError{});
    }
    TdMessageAvailableReactions converted;
    converted.top = std::move(*top);
    converted.recent = std::move(*recent);
    converted.popular = std::move(*popular);
    converted.allow_custom_emoji = value.allow_custom_emoji_;
    converted.are_tags = value.are_tags_;
    converted.unavailability_reason = convert_reaction_unavailability(
        value.unavailability_reason_.get(), converted.unsupported_unavailability_tdlib_type_id);
    return TdValue::from(std::move(converted));
}

std::optional<TdDateTimePartPrecision>
convert_date_time_precision(const td_api::DateTimePartPrecision* precision) {
    if (precision == nullptr) {
        return std::nullopt;
    }
    switch (precision->get_id()) {
    case td_api::dateTimePartPrecisionNone::ID:
        return TdDateTimePartPrecision::None;
    case td_api::dateTimePartPrecisionShort::ID:
        return TdDateTimePartPrecision::Short;
    case td_api::dateTimePartPrecisionLong::ID:
        return TdDateTimePartPrecision::Long;
    default:
        return std::nullopt;
    }
}

std::optional<TdDateTimeFormatting>
convert_date_time_formatting(const td_api::DateTimeFormattingType* formatting) {
    if (formatting == nullptr) {
        return std::nullopt;
    }
    switch (formatting->get_id()) {
    case td_api::dateTimeFormattingTypeRelative::ID:
        return TdDateTimeFormattingRelative{};
    case td_api::dateTimeFormattingTypeAbsolute::ID: {
        const auto& absolute =
            static_cast<const td_api::dateTimeFormattingTypeAbsolute&>(*formatting);
        const auto time_precision = convert_date_time_precision(absolute.time_precision_.get());
        const auto date_precision = convert_date_time_precision(absolute.date_precision_.get());
        if (!time_precision || !date_precision) {
            return std::nullopt;
        }
        return TdDateTimeFormattingAbsolute{
            .time_precision = time_precision.value_or(TdDateTimePartPrecision::None),
            .date_precision = date_precision.value_or(TdDateTimePartPrecision::None),
            .show_day_of_week = absolute.show_day_of_week_};
    }
    default:
        return std::nullopt;
    }
}

std::optional<TdTextEntity> convert_text_entity(const td_api::textEntity& entity) {
    TdTextEntity result{.offset = entity.offset_,
                        .length = entity.length_,
                        .kind = TdTextEntityKind::Unknown,
                        .value = {},
                        .numeric_value = 0,
                        .tdlib_type_id = entity.type_ == nullptr ? 0 : entity.type_->get_id(),
                        .date_time_formatting = std::nullopt};
    if (entity.type_ == nullptr) {
        return std::nullopt;
    }
    switch (entity.type_->get_id()) {
    case td_api::textEntityTypeMention::ID:
        result.kind = TdTextEntityKind::Mention;
        break;
    case td_api::textEntityTypeHashtag::ID:
        result.kind = TdTextEntityKind::Hashtag;
        break;
    case td_api::textEntityTypeCashtag::ID:
        result.kind = TdTextEntityKind::Cashtag;
        break;
    case td_api::textEntityTypeBotCommand::ID:
        result.kind = TdTextEntityKind::BotCommand;
        break;
    case td_api::textEntityTypeUrl::ID:
        result.kind = TdTextEntityKind::Url;
        break;
    case td_api::textEntityTypeEmailAddress::ID:
        result.kind = TdTextEntityKind::EmailAddress;
        break;
    case td_api::textEntityTypePhoneNumber::ID:
        result.kind = TdTextEntityKind::PhoneNumber;
        break;
    case td_api::textEntityTypeBankCardNumber::ID:
        result.kind = TdTextEntityKind::BankCardNumber;
        break;
    case td_api::textEntityTypeBold::ID:
        result.kind = TdTextEntityKind::Bold;
        break;
    case td_api::textEntityTypeItalic::ID:
        result.kind = TdTextEntityKind::Italic;
        break;
    case td_api::textEntityTypeUnderline::ID:
        result.kind = TdTextEntityKind::Underline;
        break;
    case td_api::textEntityTypeStrikethrough::ID:
        result.kind = TdTextEntityKind::Strikethrough;
        break;
    case td_api::textEntityTypeSpoiler::ID:
        result.kind = TdTextEntityKind::Spoiler;
        break;
    case td_api::textEntityTypeCode::ID:
        result.kind = TdTextEntityKind::Code;
        break;
    case td_api::textEntityTypePre::ID:
        result.kind = TdTextEntityKind::Pre;
        break;
    case td_api::textEntityTypePreCode::ID:
        result.kind = TdTextEntityKind::PreCode;
        result.value = static_cast<const td_api::textEntityTypePreCode&>(*entity.type_).language_;
        break;
    case td_api::textEntityTypeBlockQuote::ID:
        result.kind = TdTextEntityKind::BlockQuote;
        break;
    case td_api::textEntityTypeExpandableBlockQuote::ID:
        result.kind = TdTextEntityKind::ExpandableBlockQuote;
        break;
    case td_api::textEntityTypeTextUrl::ID:
        result.kind = TdTextEntityKind::TextUrl;
        result.value = static_cast<const td_api::textEntityTypeTextUrl&>(*entity.type_).url_;
        break;
    case td_api::textEntityTypeMentionName::ID:
        result.kind = TdTextEntityKind::MentionName;
        result.numeric_value =
            static_cast<const td_api::textEntityTypeMentionName&>(*entity.type_).user_id_;
        break;
    case td_api::textEntityTypeCustomEmoji::ID:
        result.kind = TdTextEntityKind::CustomEmoji;
        result.numeric_value =
            static_cast<const td_api::textEntityTypeCustomEmoji&>(*entity.type_).custom_emoji_id_;
        break;
    case td_api::textEntityTypeMediaTimestamp::ID:
        result.kind = TdTextEntityKind::MediaTimestamp;
        result.numeric_value =
            static_cast<const td_api::textEntityTypeMediaTimestamp&>(*entity.type_)
                .media_timestamp_;
        break;
    case td_api::textEntityTypeDateTime::ID: {
        const auto& date_time = static_cast<const td_api::textEntityTypeDateTime&>(*entity.type_);
        auto formatting = convert_date_time_formatting(date_time.formatting_type_.get());
        if (!formatting) {
            return std::nullopt;
        }
        result.kind = TdTextEntityKind::DateTime;
        result.numeric_value = date_time.unix_time_;
        result.date_time_formatting = formatting;
        break;
    }
    default:
        return std::nullopt;
    }
    return result;
}

std::optional<TdFormattedText> neutral_formatted_text(const td_api::formattedText& value) {
    TdFormattedText converted{.text = value.text_, .entities = {}, .capability = {}};
    converted.entities.reserve(value.entities_.size());
    for (const auto& entity : value.entities_) {
        if (entity == nullptr || entity->type_ == nullptr) {
            return std::nullopt;
        }
        auto converted_entity = convert_text_entity(*entity);
        if (!converted_entity) {
            return std::nullopt;
        }
        converted.entities.push_back(std::move(*converted_entity));
    }
    if (!valid_td_formatted_text_facts(converted)) {
        return std::nullopt;
    }
    return converted;
}

TdValue convert_formatted_text(td_api::object_ptr<td_api::formattedText> value,
                               std::uint64_t client_generation) {
    if (value == nullptr) {
        return TdValue::from(TdDirectConversionError{});
    }
    auto converted = neutral_formatted_text(*value);
    if (!converted) {
        return TdValue::from(TdDirectConversionError{});
    }
    converted->capability = TdFormattedTextCapability::from(std::move(value), client_generation);
    return TdValue::from(std::move(*converted));
}

TdValue convert_chat_join_result(const td_api::ChatJoinResult& value) {
    switch (value.get_id()) {
    case td_api::chatJoinResultSuccess::ID:
        return TdValue::from(TdChatJoinResult{
            .kind = TdChatJoinResultKind::Success,
            .chat_id = static_cast<const td_api::chatJoinResultSuccess&>(value).chat_id_,
            .guard_bot_user_id = std::nullopt,
            .guard_query_id = std::nullopt,
            .unsupported_tdlib_type_id = std::nullopt});
    case td_api::chatJoinResultRequestSent::ID:
        return TdValue::from(TdChatJoinResult{.kind = TdChatJoinResultKind::RequestSent,
                                              .chat_id = std::nullopt,
                                              .guard_bot_user_id = std::nullopt,
                                              .guard_query_id = std::nullopt,
                                              .unsupported_tdlib_type_id = std::nullopt});
    case td_api::chatJoinResultGuardBotApprovalRequired::ID: {
        const auto& guard =
            static_cast<const td_api::chatJoinResultGuardBotApprovalRequired&>(value);
        return TdValue::from(
            TdChatJoinResult{.kind = TdChatJoinResultKind::GuardBotApprovalRequired,
                             .chat_id = std::nullopt,
                             .guard_bot_user_id = guard.bot_user_id_,
                             .guard_query_id = guard.query_id_,
                             .unsupported_tdlib_type_id = std::nullopt});
    }
    case td_api::chatJoinResultDeclined::ID:
        return TdValue::from(TdChatJoinResult{.kind = TdChatJoinResultKind::Declined,
                                              .chat_id = std::nullopt,
                                              .guard_bot_user_id = std::nullopt,
                                              .guard_query_id = std::nullopt,
                                              .unsupported_tdlib_type_id = std::nullopt});
    default:
        return TdValue::from(TdChatJoinResult{.kind = TdChatJoinResultKind::Unknown,
                                              .chat_id = std::nullopt,
                                              .guard_bot_user_id = std::nullopt,
                                              .guard_query_id = std::nullopt,
                                              .unsupported_tdlib_type_id = value.get_id()});
    }
}

TdMessages convert_messages(const td_api::messages& messages) {
    TdMessages converted{.total_count = messages.total_count_, .messages = {}};
    converted.messages.reserve(messages.messages_.size());
    for (const auto& message : messages.messages_) {
        converted.messages.push_back(
            message == nullptr ? std::nullopt
                               : std::optional<TdMessageSummary>{convert_message(*message)});
    }
    return converted;
}

TdMessageThreadInfo convert_message_thread_info(const td_api::messageThreadInfo& info) {
    TdMessageThreadInfo converted{.history_chat_id = info.chat_id_,
                                  .history_thread_id = info.message_thread_id_,
                                  .starting_messages = {}};
    converted.starting_messages.reserve(info.messages_.size());
    for (const auto& message : info.messages_) {
        converted.starting_messages.push_back(
            message == nullptr ? std::nullopt
                               : std::optional<TdMessageSummary>{convert_message(*message)});
    }
    return converted;
}

TdMessageLink convert_message_link(td_api::messageLink& link) {
    return {.link = std::move(link.link_), .is_public = link.is_public_};
}

TdChatNotificationSettings
convert_chat_notification_settings(const td_api::chatNotificationSettings& value) {
    return {.use_default_mute_for = value.use_default_mute_for_,
            .mute_for = value.mute_for_,
            .use_default_sound = value.use_default_sound_,
            .sound_id = value.sound_id_,
            .use_default_show_preview = value.use_default_show_preview_,
            .show_preview = value.show_preview_,
            .use_default_mute_stories = value.use_default_mute_stories_,
            .mute_stories = value.mute_stories_,
            .use_default_story_sound = value.use_default_story_sound_,
            .story_sound_id = value.story_sound_id_,
            .use_default_show_story_poster = value.use_default_show_story_poster_,
            .show_story_poster = value.show_story_poster_,
            .use_default_disable_pinned_message_notifications =
                value.use_default_disable_pinned_message_notifications_,
            .disable_pinned_message_notifications = value.disable_pinned_message_notifications_,
            .use_default_disable_mention_notifications =
                value.use_default_disable_mention_notifications_,
            .disable_mention_notifications = value.disable_mention_notifications_};
}

TdChat convert_chat(td_api::chat& chat) {
    TdChat converted{.id = chat.id_,
                     .title = std::move(chat.title_),
                     .kind = TdChatKind::Unknown,
                     .related_id = 0,
                     .tdlib_type_id = chat.type_ == nullptr ? 0 : chat.type_->get_id(),
                     .positions = {},
                     .chat_lists = {},
                     .is_marked_unread = chat.is_marked_as_unread_,
                     .unread_count = chat.unread_count_,
                     .unread_mention_count = chat.unread_mention_count_,
                     .unread_reaction_count = chat.unread_reaction_count_,
                     .unread_poll_vote_count = chat.unread_poll_vote_count_,
                     .last_message = std::nullopt,
                     .notification_settings = std::nullopt};
    converted.positions.reserve(chat.positions_.size());
    for (const auto& position : chat.positions_) {
        if (position == nullptr) {
            converted.positions.push_back({});
            continue;
        }
        converted.positions.push_back(
            {.list = convert_chat_list(position->list_.get()), .order = position->order_});
    }
    converted.chat_lists.reserve(chat.chat_lists_.size());
    for (const auto& list : chat.chat_lists_) {
        converted.chat_lists.push_back(convert_chat_list(list.get()));
    }
    if (chat.last_message_ != nullptr) {
        converted.last_message = convert_message(*chat.last_message_);
    }
    if (chat.notification_settings_ != nullptr) {
        converted.notification_settings =
            convert_chat_notification_settings(*chat.notification_settings_);
    }
    if (chat.type_ == nullptr) {
        return converted;
    }
    switch (chat.type_->get_id()) {
    case td_api::chatTypePrivate::ID:
        converted.kind = TdChatKind::Private;
        converted.related_id = static_cast<const td_api::chatTypePrivate&>(*chat.type_).user_id_;
        break;
    case td_api::chatTypeBasicGroup::ID:
        converted.kind = TdChatKind::BasicGroup;
        converted.related_id =
            static_cast<const td_api::chatTypeBasicGroup&>(*chat.type_).basic_group_id_;
        break;
    case td_api::chatTypeSupergroup::ID: {
        const auto& type = static_cast<const td_api::chatTypeSupergroup&>(*chat.type_);
        converted.kind = type.is_channel_ ? TdChatKind::Channel : TdChatKind::Supergroup;
        converted.related_id = type.supergroup_id_;
        break;
    }
    case td_api::chatTypeSecret::ID:
        converted.kind = TdChatKind::Secret;
        converted.related_id = static_cast<const td_api::chatTypeSecret&>(*chat.type_).user_id_;
        break;
    default:
        break;
    }
    return converted;
}

TdTopic convert_topic(const td_api::MessageTopic& topic) {
    switch (topic.get_id()) {
    case td_api::messageTopicForum::ID:
        return {.kind = TdTopicKind::Forum,
                .id = static_cast<const td_api::messageTopicForum&>(topic).forum_topic_id_,
                .tdlib_type_id = topic.get_id()};
    case td_api::messageTopicThread::ID:
        return {.kind = TdTopicKind::Thread,
                .id = static_cast<const td_api::messageTopicThread&>(topic).message_thread_id_,
                .tdlib_type_id = topic.get_id()};
    case td_api::messageTopicDirectMessages::ID:
        return {.kind = TdTopicKind::Direct,
                .id = static_cast<const td_api::messageTopicDirectMessages&>(topic)
                          .direct_messages_chat_topic_id_,
                .tdlib_type_id = topic.get_id()};
    case td_api::messageTopicSavedMessages::ID:
        return {.kind = TdTopicKind::Saved,
                .id = static_cast<const td_api::messageTopicSavedMessages&>(topic)
                          .saved_messages_topic_id_,
                .tdlib_type_id = topic.get_id()};
    default:
        return {.kind = TdTopicKind::Unknown, .id = 0, .tdlib_type_id = topic.get_id()};
    }
}

TdInternalLink convert_internal_link(td_api::InternalLinkType& link) {
    TdInternalLink converted{.kind = TdInternalLinkKind::Unsupported,
                             .username = {},
                             .url = {},
                             .tdlib_type_id = link.get_id()};
    switch (link.get_id()) {
    case td_api::internalLinkTypePublicChat::ID:
        converted.kind = TdInternalLinkKind::PublicChat;
        converted.username =
            std::move(static_cast<td_api::internalLinkTypePublicChat&>(link).chat_username_);
        break;
    case td_api::internalLinkTypeBotStart::ID:
        converted.kind = TdInternalLinkKind::BotStart;
        converted.username =
            std::move(static_cast<td_api::internalLinkTypeBotStart&>(link).bot_username_);
        break;
    case td_api::internalLinkTypeMessage::ID:
        converted.kind = TdInternalLinkKind::Message;
        converted.url = std::move(static_cast<td_api::internalLinkTypeMessage&>(link).url_);
        break;
    case td_api::internalLinkTypeChatInvite::ID:
        converted.kind = TdInternalLinkKind::ChatInvite;
        converted.url =
            std::move(static_cast<td_api::internalLinkTypeChatInvite&>(link).invite_link_);
        break;
    case td_api::internalLinkTypeDirectMessagesChat::ID:
        converted.kind = TdInternalLinkKind::DirectMessagesChat;
        converted.username = std::move(
            static_cast<td_api::internalLinkTypeDirectMessagesChat&>(link).channel_username_);
        break;
    case td_api::internalLinkTypeSavedMessages::ID:
        converted.kind = TdInternalLinkKind::SavedMessages;
        break;
    default:
        break;
    }
    return converted;
}

bool is_internal_link_type(std::int32_t type_id) {
    switch (type_id) {
    case td_api::internalLinkTypeAttachmentMenuBot::ID:
    case td_api::internalLinkTypeAuthenticationCode::ID:
    case td_api::internalLinkTypeBackground::ID:
    case td_api::internalLinkTypeBotAddToChannel::ID:
    case td_api::internalLinkTypeBotStart::ID:
    case td_api::internalLinkTypeBotStartInGroup::ID:
    case td_api::internalLinkTypeBusinessChat::ID:
    case td_api::internalLinkTypeCallsPage::ID:
    case td_api::internalLinkTypeChatAffiliateProgram::ID:
    case td_api::internalLinkTypeChatBoost::ID:
    case td_api::internalLinkTypeChatFolderInvite::ID:
    case td_api::internalLinkTypeChatInvite::ID:
    case td_api::internalLinkTypeChatSelection::ID:
    case td_api::internalLinkTypeContactsPage::ID:
    case td_api::internalLinkTypeDirectMessagesChat::ID:
    case td_api::internalLinkTypeGame::ID:
    case td_api::internalLinkTypeGiftAuction::ID:
    case td_api::internalLinkTypeGiftCollection::ID:
    case td_api::internalLinkTypeGroupCall::ID:
    case td_api::internalLinkTypeInstantView::ID:
    case td_api::internalLinkTypeInvoice::ID:
    case td_api::internalLinkTypeLanguagePack::ID:
    case td_api::internalLinkTypeLiveStory::ID:
    case td_api::internalLinkTypeMainWebApp::ID:
    case td_api::internalLinkTypeMessage::ID:
    case td_api::internalLinkTypeMessageDraft::ID:
    case td_api::internalLinkTypeMyProfilePage::ID:
    case td_api::internalLinkTypeNewChannelChat::ID:
    case td_api::internalLinkTypeNewGroupChat::ID:
    case td_api::internalLinkTypeNewPrivateChat::ID:
    case td_api::internalLinkTypeNewStory::ID:
    case td_api::internalLinkTypeOauth::ID:
    case td_api::internalLinkTypePassportDataRequest::ID:
    case td_api::internalLinkTypePhoneNumberConfirmation::ID:
    case td_api::internalLinkTypePremiumFeaturesPage::ID:
    case td_api::internalLinkTypePremiumGiftCode::ID:
    case td_api::internalLinkTypePremiumGiftPurchase::ID:
    case td_api::internalLinkTypeProxy::ID:
    case td_api::internalLinkTypePublicChat::ID:
    case td_api::internalLinkTypeQrCodeAuthentication::ID:
    case td_api::internalLinkTypeRequestManagedBot::ID:
    case td_api::internalLinkTypeRestorePurchases::ID:
    case td_api::internalLinkTypeSavedMessages::ID:
    case td_api::internalLinkTypeSearch::ID:
    case td_api::internalLinkTypeSettings::ID:
    case td_api::internalLinkTypeStarPurchase::ID:
    case td_api::internalLinkTypeStickerSet::ID:
    case td_api::internalLinkTypeStory::ID:
    case td_api::internalLinkTypeStoryAlbum::ID:
    case td_api::internalLinkTypeTextCompositionStyle::ID:
    case td_api::internalLinkTypeTheme::ID:
    case td_api::internalLinkTypeUnknownDeepLink::ID:
    case td_api::internalLinkTypeUpgradedGift::ID:
    case td_api::internalLinkTypeUserPhoneNumber::ID:
    case td_api::internalLinkTypeUserToken::ID:
    case td_api::internalLinkTypeVideoChat::ID:
    case td_api::internalLinkTypeWebApp::ID:
        return true;
    default:
        return false;
    }
}

TdValue convert_message_send_succeeded(const td_api::updateMessageSendSucceeded& update,
                                       std::uint64_t client_generation) {
    std::optional<TdWriteMessage> message;
    if (update.message_ != nullptr) {
        message = convert_write_message_details(*update.message_);
    }
    return TdValue::from(TdUpdateMessageSendSucceeded{.client_generation = client_generation,
                                                      .old_message_id = update.old_message_id_,
                                                      .message = std::move(message)});
}

TdValue convert_message_send_failed(const td_api::updateMessageSendFailed& update,
                                    std::uint64_t client_generation) {
    std::optional<TdWriteMessage> message;
    std::optional<TdError> error;
    if (update.message_ != nullptr) {
        message = convert_write_message_details(*update.message_);
    }
    if (update.error_ != nullptr) {
        error = TdError{.code = update.error_->code_, .message = update.error_->message_};
    }
    return TdValue::from(TdUpdateMessageSendFailed{.client_generation = client_generation,
                                                   .old_message_id = update.old_message_id_,
                                                   .message = std::move(message),
                                                   .error = std::move(error)});
}

TdValue convert_delete_messages_update(td_api::updateDeleteMessages& update,
                                       std::uint64_t client_generation) {
    return TdValue::from(TdUpdateDeleteMessages{.client_generation = client_generation,
                                                .chat_id = update.chat_id_,
                                                .message_ids = std::move(update.message_ids_),
                                                .is_permanent = update.is_permanent_,
                                                .from_cache = update.from_cache_});
}

TdValue convert_response(NativeObjectPtr object, std::uint64_t client_generation = 0) {
    if (object == nullptr) {
        return {};
    }
    switch (object->get_id()) {
    case td_api::updateMessageSendSucceeded::ID:
        return convert_message_send_succeeded(
            static_cast<const td_api::updateMessageSendSucceeded&>(*object), client_generation);
    case td_api::updateMessageSendFailed::ID:
        return convert_message_send_failed(
            static_cast<const td_api::updateMessageSendFailed&>(*object), client_generation);
    case td_api::updateDeleteMessages::ID:
        return convert_delete_messages_update(static_cast<td_api::updateDeleteMessages&>(*object),
                                              client_generation);
    case td_api::ok::ID:
        return TdValue::from(TdOk{});
    case td_api::error::ID: {
        auto& error = static_cast<td_api::error&>(*object);
        return TdValue::from(TdError{error.code_, std::move(error.message_)});
    }
    case td_api::user::ID: {
        auto& user = static_cast<td_api::user&>(*object);
        TdUserPresence presence = TdUserPresence::Hidden;
        if (user.status_ != nullptr) {
            if (user.status_->get_id() == td_api::userStatusOnline::ID) {
                presence = TdUserPresence::Online;
            } else if (user.status_->get_id() == td_api::userStatusOffline::ID) {
                presence = TdUserPresence::Offline;
            }
        }
        TdUserSummary summary{.id = user.id_,
                              .first_name = std::move(user.first_name_),
                              .last_name = std::move(user.last_name_),
                              .usernames = {},
                              .phone_number = std::move(user.phone_number_),
                              .is_bot = user.type_ != nullptr &&
                                        user.type_->get_id() == td_api::userTypeBot::ID,
                              .is_premium = user.is_premium_,
                              .presence = presence};
        if (user.usernames_ != nullptr) {
            summary.usernames = std::move(user.usernames_->active_usernames_);
        }
        return TdValue::from(std::move(summary));
    }
    case td_api::chat::ID:
        return TdValue::from(convert_chat(static_cast<td_api::chat&>(*object)));
    case td_api::chats::ID: {
        auto& chats = static_cast<td_api::chats&>(*object);
        return TdValue::from(TdChats{.chat_ids = std::move(chats.chat_ids_)});
    }
    case td_api::messages::ID: {
        return TdValue::from(convert_messages(static_cast<const td_api::messages&>(*object)));
    }
    case td_api::message::ID:
        return TdValue::from(convert_message(static_cast<const td_api::message&>(*object)));
    case td_api::messageThreadInfo::ID:
        return TdValue::from(
            convert_message_thread_info(static_cast<const td_api::messageThreadInfo&>(*object)));
    case td_api::messageLink::ID: {
        return TdValue::from(convert_message_link(static_cast<td_api::messageLink&>(*object)));
    }
    case td_api::supergroup::ID: {
        auto& supergroup = static_cast<td_api::supergroup&>(*object);
        TdSupergroup converted{
            .id = supergroup.id_, .usernames = {}, .is_channel = supergroup.is_channel_};
        if (supergroup.usernames_ != nullptr) {
            converted.usernames = std::move(supergroup.usernames_->active_usernames_);
        }
        return TdValue::from(std::move(converted));
    }
    case td_api::internalLinkTypePublicChat::ID:
    case td_api::internalLinkTypeBotStart::ID:
    case td_api::internalLinkTypeMessage::ID:
    case td_api::internalLinkTypeChatInvite::ID:
    case td_api::internalLinkTypeDirectMessagesChat::ID:
    case td_api::internalLinkTypeSavedMessages::ID:
        return TdValue::from(
            convert_internal_link(static_cast<td_api::InternalLinkType&>(*object)));
    case td_api::messageLinkInfo::ID: {
        auto& info = static_cast<td_api::messageLinkInfo&>(*object);
        TdMessageLinkInfo converted{.is_public = info.is_public_,
                                    .chat_id = info.chat_id_,
                                    .message_id = std::nullopt,
                                    .topic = std::nullopt};
        if (info.message_ != nullptr) {
            converted.message_id = info.message_->id_;
        }
        if (info.topic_id_ != nullptr) {
            converted.topic = convert_topic(*info.topic_id_);
        }
        return TdValue::from(converted);
    }
    case td_api::chatInviteLinkInfo::ID: {
        const auto& info = static_cast<const td_api::chatInviteLinkInfo&>(*object);
        return TdValue::from(
            TdChatInviteLinkInfo{.chat_id = info.chat_id_, .is_public = info.is_public_});
    }
    case td_api::supergroupFullInfo::ID: {
        const auto& info = static_cast<const td_api::supergroupFullInfo&>(*object);
        return TdValue::from(
            TdSupergroupFullInfo{.direct_messages_chat_id = info.direct_messages_chat_id_});
    }
    case td_api::savedMessagesTags::ID: {
        auto& tags = static_cast<td_api::savedMessagesTags&>(*object);
        TdSavedMessagesTags converted;
        converted.tags.reserve(tags.tags_.size());
        for (auto& item : tags.tags_) {
            if (item == nullptr) {
                converted.tags.push_back({.tag = {.kind = TdReactionKind::Unknown,
                                                  .emoji = {},
                                                  .custom_emoji_id = 0,
                                                  .tdlib_type_id = 0},
                                          .label = {},
                                          .count = 0});
                continue;
            }
            converted.tags.push_back({.tag = convert_reaction(item->tag_.get()),
                                      .label = std::move(item->label_),
                                      .count = item->count_});
        }
        return TdValue::from(std::move(converted));
    }
    case td_api::foundChatMessages::ID: {
        auto& found = static_cast<td_api::foundChatMessages&>(*object);
        TdFoundSavedMessages converted;
        converted.next_from_message_id = found.next_from_message_id_;
        converted.messages.reserve(found.messages_.size());
        for (const auto& message : found.messages_) {
            if (message != nullptr) {
                converted.messages.push_back(convert_saved_message(*message));
            }
        }
        return TdValue::from(std::move(converted));
    }
    case td_api::sessions::ID:
        return convert_sessions(td_api::move_object_as<td_api::sessions>(object));
    default:
        if (is_internal_link_type(object->get_id())) {
            return TdValue::from(
                convert_internal_link(static_cast<td_api::InternalLinkType&>(*object)));
        }
        return TdValue::from(std::move(object));
    }
}

TdValue unexpected_direct_response(const NativeObjectPtr& object) {
    return TdValue::from(TdDirectConversionError{
        .tdlib_type_id =
            object == nullptr ? std::nullopt : std::optional<std::int32_t>{object->get_id()}});
}

TdValue convert_planning_message_response(const NativeObjectPtr& object) {
    if (object->get_id() != td_api::message::ID) {
        return unexpected_direct_response(object);
    }
    auto converted = convert_planning_message(static_cast<const td_api::message&>(*object));
    if (!valid_persistable_message_text(converted.text)) {
        return unexpected_direct_response(object);
    }
    return TdValue::from(std::move(converted));
}

TdValue convert_write_message_response(const NativeObjectPtr& object) {
    if (object->get_id() != td_api::message::ID) {
        return unexpected_direct_response(object);
    }
    auto converted = convert_write_message(static_cast<const td_api::message&>(*object));
    if (!valid_persistable_message_text(converted.text)) {
        return unexpected_direct_response(object);
    }
    return TdValue::from(std::move(converted));
}

TdValue convert_send_message_response(const NativeObjectPtr& object) {
    if (object->get_id() != td_api::message::ID) {
        return unexpected_direct_response(object);
    }
    auto converted = convert_write_message_details(static_cast<const td_api::message&>(*object));
    if (!valid_persistable_message_text(converted.message.text)) {
        return unexpected_direct_response(object);
    }
    return TdValue::from(std::move(converted));
}

TdValue convert_join_response(const NativeObjectPtr& object) {
    switch (object->get_id()) {
    case td_api::chatJoinResultSuccess::ID:
    case td_api::chatJoinResultRequestSent::ID:
    case td_api::chatJoinResultGuardBotApprovalRequired::ID:
    case td_api::chatJoinResultDeclined::ID:
        return convert_chat_join_result(static_cast<const td_api::ChatJoinResult&>(*object));
    default:
        return unexpected_direct_response(object);
    }
}

TdValue convert_response_for(TdFunctionKind function, NativeObjectPtr object,
                             std::uint64_t client_generation = 0) {
    if (object == nullptr) {
        return TdValue::from(TdDirectConversionError{});
    }
    if (object->get_id() == td_api::error::ID) {
        return convert_response(std::move(object));
    }
    switch (function) {
    case TdFunctionKind::GetMessage:
        return convert_planning_message_response(object);
    case TdFunctionKind::EditMessageText:
        return convert_write_message_response(object);
    case TdFunctionKind::SendMessage:
        return convert_send_message_response(object);
    case TdFunctionKind::GetMessageProperties:
        if (object->get_id() == td_api::messageProperties::ID) {
            return TdValue::from(
                convert_message_properties(static_cast<const td_api::messageProperties&>(*object)));
        }
        return unexpected_direct_response(object);
    case TdFunctionKind::GetMessageAvailableReactions:
        if (object->get_id() == td_api::availableReactions::ID) {
            return convert_message_available_reactions(
                static_cast<const td_api::availableReactions&>(*object));
        }
        return unexpected_direct_response(object);
    case TdFunctionKind::GetOption:
        if (object->get_id() == td_api::optionValueInteger::ID) {
            return TdValue::from(TdOptionInteger{
                .value = static_cast<const td_api::optionValueInteger&>(*object).value_});
        }
        return unexpected_direct_response(object);
    case TdFunctionKind::ParseTextEntities:
        if (object->get_id() == td_api::formattedText::ID) {
            return convert_formatted_text(
                td_api::move_object_as<td_api::formattedText>(std::move(object)),
                client_generation);
        }
        return unexpected_direct_response(object);
    case TdFunctionKind::JoinChat:
    case TdFunctionKind::JoinChatByInviteLink:
        return convert_join_response(object);
    case TdFunctionKind::DeleteMessages:
    case TdFunctionKind::AddMessageReaction:
    case TdFunctionKind::RemoveMessageReaction:
    case TdFunctionKind::PinChatMessage:
    case TdFunctionKind::UnpinChatMessage:
    case TdFunctionKind::ViewMessages:
    case TdFunctionKind::SetChatNotificationSettings:
    case TdFunctionKind::ToggleChatIsPinned:
    case TdFunctionKind::AddChatToList:
    case TdFunctionKind::LeaveChat:
        if (object->get_id() == td_api::ok::ID) {
            return TdValue::from(TdOk{});
        }
        return unexpected_direct_response(object);
    default:
        return convert_response(std::move(object));
    }
}

bool native_function_matches(const td_api::Function& function, TdFunctionKind kind) {
    switch (kind) {
    case TdFunctionKind::GetAuthorizationState:
        return function.get_id() == td_api::getAuthorizationState::ID;
    case TdFunctionKind::SetTdlibParameters:
        return function.get_id() == td_api::setTdlibParameters::ID;
    case TdFunctionKind::SetAuthenticationPhoneNumber:
        return function.get_id() == td_api::setAuthenticationPhoneNumber::ID;
    case TdFunctionKind::RequestQrCodeAuthentication:
        return function.get_id() == td_api::requestQrCodeAuthentication::ID;
    case TdFunctionKind::CheckAuthenticationBotToken:
        return function.get_id() == td_api::checkAuthenticationBotToken::ID;
    case TdFunctionKind::SetAuthenticationEmailAddress:
        return function.get_id() == td_api::setAuthenticationEmailAddress::ID;
    case TdFunctionKind::CheckAuthenticationEmailCode:
        return function.get_id() == td_api::checkAuthenticationEmailCode::ID;
    case TdFunctionKind::CheckAuthenticationCode:
        return function.get_id() == td_api::checkAuthenticationCode::ID;
    case TdFunctionKind::RegisterUser:
        return function.get_id() == td_api::registerUser::ID;
    case TdFunctionKind::CheckAuthenticationPassword:
        return function.get_id() == td_api::checkAuthenticationPassword::ID;
    case TdFunctionKind::GetOption:
        return function.get_id() == td_api::getOption::ID;
    case TdFunctionKind::GetMe:
        return function.get_id() == td_api::getMe::ID;
    case TdFunctionKind::GetSavedMessagesTags:
        return function.get_id() == td_api::getSavedMessagesTags::ID;
    case TdFunctionKind::SearchSavedMessages:
        return function.get_id() == td_api::searchSavedMessages::ID;
    case TdFunctionKind::GetActiveSessions:
        return function.get_id() == td_api::getActiveSessions::ID;
    case TdFunctionKind::TerminateSession:
        return function.get_id() == td_api::terminateSession::ID;
    case TdFunctionKind::GetChat:
        return function.get_id() == td_api::getChat::ID;
    case TdFunctionKind::GetChatHistory:
        return function.get_id() == td_api::getChatHistory::ID;
    case TdFunctionKind::GetChatMessageByDate:
        return function.get_id() == td_api::getChatMessageByDate::ID;
    case TdFunctionKind::GetMessageThread:
        return function.get_id() == td_api::getMessageThread::ID;
    case TdFunctionKind::GetForumTopicHistory:
        return function.get_id() == td_api::getForumTopicHistory::ID;
    case TdFunctionKind::GetMessageThreadHistory:
        return function.get_id() == td_api::getMessageThreadHistory::ID;
    case TdFunctionKind::GetDirectMessagesChatTopicHistory:
        return function.get_id() == td_api::getDirectMessagesChatTopicHistory::ID;
    case TdFunctionKind::GetSavedMessagesTopicHistory:
        return function.get_id() == td_api::getSavedMessagesTopicHistory::ID;
    case TdFunctionKind::GetMessages:
        return function.get_id() == td_api::getMessages::ID;
    case TdFunctionKind::GetMessageLink:
        return function.get_id() == td_api::getMessageLink::ID;
    case TdFunctionKind::GetChats:
        return function.get_id() == td_api::getChats::ID;
    case TdFunctionKind::LoadChats:
        return function.get_id() == td_api::loadChats::ID;
    case TdFunctionKind::SearchPublicChat:
        return function.get_id() == td_api::searchPublicChat::ID;
    case TdFunctionKind::GetInternalLinkType:
        return function.get_id() == td_api::getInternalLinkType::ID;
    case TdFunctionKind::GetMessageLinkInfo:
        return function.get_id() == td_api::getMessageLinkInfo::ID;
    case TdFunctionKind::CheckChatInviteLink:
        return function.get_id() == td_api::checkChatInviteLink::ID;
    case TdFunctionKind::GetUser:
        return function.get_id() == td_api::getUser::ID;
    case TdFunctionKind::GetSupergroup:
        return function.get_id() == td_api::getSupergroup::ID;
    case TdFunctionKind::GetSupergroupFullInfo:
        return function.get_id() == td_api::getSupergroupFullInfo::ID;
    case TdFunctionKind::CreatePrivateChat:
        return function.get_id() == td_api::createPrivateChat::ID;
    case TdFunctionKind::GetMessage:
        return function.get_id() == td_api::getMessage::ID;
    case TdFunctionKind::GetMessageProperties:
        return function.get_id() == td_api::getMessageProperties::ID;
    case TdFunctionKind::GetMessageAvailableReactions:
        return function.get_id() == td_api::getMessageAvailableReactions::ID;
    case TdFunctionKind::ParseTextEntities:
        return function.get_id() == td_api::parseTextEntities::ID;
    case TdFunctionKind::SendMessage:
        return function.get_id() == td_api::sendMessage::ID;
    case TdFunctionKind::EditMessageText:
        return function.get_id() == td_api::editMessageText::ID;
    case TdFunctionKind::DeleteMessages:
        return function.get_id() == td_api::deleteMessages::ID;
    case TdFunctionKind::AddMessageReaction:
        return function.get_id() == td_api::addMessageReaction::ID;
    case TdFunctionKind::RemoveMessageReaction:
        return function.get_id() == td_api::removeMessageReaction::ID;
    case TdFunctionKind::PinChatMessage:
        return function.get_id() == td_api::pinChatMessage::ID;
    case TdFunctionKind::UnpinChatMessage:
        return function.get_id() == td_api::unpinChatMessage::ID;
    case TdFunctionKind::ViewMessages:
        return function.get_id() == td_api::viewMessages::ID;
    case TdFunctionKind::SetChatNotificationSettings:
        return function.get_id() == td_api::setChatNotificationSettings::ID;
    case TdFunctionKind::ToggleChatIsPinned:
        return function.get_id() == td_api::toggleChatIsPinned::ID;
    case TdFunctionKind::AddChatToList:
        return function.get_id() == td_api::addChatToList::ID;
    case TdFunctionKind::JoinChat:
        return function.get_id() == td_api::joinChat::ID;
    case TdFunctionKind::JoinChatByInviteLink:
        return function.get_id() == td_api::joinChatByInviteLink::ID;
    case TdFunctionKind::LeaveChat:
        return function.get_id() == td_api::leaveChat::ID;
    case TdFunctionKind::LogOut:
        return function.get_id() == td_api::logOut::ID;
    case TdFunctionKind::Close:
        return function.get_id() == td_api::close::ID;
    }
    return false;
}

void require_message_locator(std::int64_t chat_id, std::int64_t message_id) {
    if (!valid_td_message_locator(chat_id, message_id)) {
        throw std::invalid_argument("direct TD request contains an invalid message locator");
    }
}

template <typename Request> void require_direct_request(const Request& request) {
    if (!valid_td_direct_request(request)) {
        throw std::invalid_argument("direct TD request is invalid");
    }
}

td_api::object_ptr<td_api::ChatList> make_direct_chat_list(TdDirectChatList list) {
    switch (list) {
    case TdDirectChatList::Main:
        return td_api::make_object<td_api::chatListMain>();
    case TdDirectChatList::Archive:
        return td_api::make_object<td_api::chatListArchive>();
    }
    throw std::invalid_argument("unsupported direct chat list");
}

std::string direct_chat_list_name(TdDirectChatList list) {
    return list == TdDirectChatList::Main ? "main" : "archive";
}

TdValue make_native_get_message(std::int64_t chat_id, std::int64_t message_id) {
    require_message_locator(chat_id, message_id);
    NativeFunctionPtr native = td_api::make_object<td_api::getMessage>(chat_id, message_id);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::GetMessage,
                                            {{"chat_id", chat_id}, {"message_id", message_id}}});
}

TdValue make_native_get_message_properties(std::int64_t chat_id, std::int64_t message_id) {
    require_message_locator(chat_id, message_id);
    NativeFunctionPtr native =
        td_api::make_object<td_api::getMessageProperties>(chat_id, message_id);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::GetMessageProperties,
                                            {{"chat_id", chat_id}, {"message_id", message_id}}});
}

TdValue make_native_get_message_available_reactions(std::int64_t chat_id, std::int64_t message_id) {
    require_message_locator(chat_id, message_id);
    NativeFunctionPtr native =
        td_api::make_object<td_api::getMessageAvailableReactions>(chat_id, message_id, 25);
    return TdValue::function(
        std::move(native),
        TdFunctionData{
            TdFunctionKind::GetMessageAvailableReactions,
            {{"chat_id", chat_id}, {"message_id", message_id}, {"row_size", std::int64_t{25}}}});
}

TdValue make_native_get_unix_time() {
    NativeFunctionPtr native = td_api::make_object<td_api::getOption>("unix_time");
    return TdValue::function(
        std::move(native),
        TdFunctionData{TdFunctionKind::GetOption, {{"name", std::string{"unix_time"}}}});
}

TdValue make_native_parse_text_entities(std::string text, TdTextParseMode mode) {
    td_api::object_ptr<td_api::TextParseMode> parse_mode;
    std::string mode_name;
    switch (mode) {
    case TdTextParseMode::MarkdownV2:
        parse_mode = td_api::make_object<td_api::textParseModeMarkdown>(2);
        mode_name = "markdown_v2";
        break;
    case TdTextParseMode::Html:
        parse_mode = td_api::make_object<td_api::textParseModeHTML>();
        mode_name = "html";
        break;
    }
    NativeFunctionPtr native =
        td_api::make_object<td_api::parseTextEntities>(text, std::move(parse_mode));
    return TdValue::function(
        std::move(native),
        TdFunctionData{TdFunctionKind::ParseTextEntities,
                       {{"text", std::move(text)}, {"parse_mode", std::move(mode_name)}}});
}

td_api::object_ptr<td_api::MessageTopic> make_send_topic(const std::optional<TdTopic>& topic) {
    if (!topic) {
        return nullptr;
    }
    if (topic->kind == TdTopicKind::Forum) {
        return td_api::make_object<td_api::messageTopicForum>(static_cast<std::int32_t>(topic->id));
    }
    return td_api::make_object<td_api::messageTopicSavedMessages>(topic->id);
}

td_api::object_ptr<td_api::InputMessageReplyTo>
make_send_reply(const std::optional<std::int64_t>& reply_to_message_id) {
    if (!reply_to_message_id) {
        return nullptr;
    }
    return td_api::make_object<td_api::inputMessageReplyToMessage>(*reply_to_message_id, nullptr, 0,
                                                                   std::string{});
}

td_api::object_ptr<td_api::MessageSchedulingState>
make_send_schedule(const TdSendSchedule& schedule) {
    switch (schedule.kind) {
    case TdSendScheduleKind::Immediate:
        return nullptr;
    case TdSendScheduleKind::AtDate:
        return td_api::make_object<td_api::messageSchedulingStateSendAtDate>(schedule.send_date, 0);
    case TdSendScheduleKind::WhenOnline:
        return td_api::make_object<td_api::messageSchedulingStateSendWhenOnline>();
    }
    throw std::invalid_argument("unsupported send schedule");
}

TdValue make_native_send_message(TdSendMessageRequest request, std::uint64_t client_generation) {
    if (!valid_td_send_message_request(request) || client_generation == 0) {
        throw std::invalid_argument("sendMessage request is invalid");
    }
    auto description = describe_td_send_message_request(request);
    td_api::object_ptr<td_api::formattedText> text;
    if (request.content.parsed) {
        auto consumed = request.content.formatted_text.capability
                            .consume<td_api::object_ptr<td_api::formattedText>>(client_generation);
        if (!consumed || *consumed == nullptr) {
            throw std::invalid_argument("parsed formattedText capability is unavailable");
        }
        auto neutral = neutral_formatted_text(**consumed);
        if (!neutral || *neutral != request.content.formatted_text) {
            throw std::invalid_argument("parsed formattedText capability does not match its facts");
        }
        text = std::move(*consumed);
    } else {
        text = td_api::make_object<td_api::formattedText>(
            request.content.formatted_text.text,
            std::vector<td_api::object_ptr<td_api::textEntity>>{});
    }
    auto schedule = make_send_schedule(request.options.schedule);
    auto options = td_api::make_object<td_api::messageSendOptions>(
        nullptr, request.options.disable_notification, false, false, false, 0, false,
        std::move(schedule), 0, request.options.sending_id, false);
    auto content = td_api::make_object<td_api::inputMessageText>(std::move(text), nullptr, false);
    auto topic = make_send_topic(request.topic);
    auto reply = make_send_reply(request.reply_to_message_id);
    NativeFunctionPtr native = td_api::make_object<td_api::sendMessage>(
        request.chat_id, std::move(topic), std::move(reply), std::move(options), nullptr,
        std::move(content));
    return TdValue::function(std::move(native), std::move(description));
}

TdValue make_native_edit_message_text(TdEditMessageTextRequest request) {
    require_direct_request(request);
    auto text = td_api::make_object<td_api::formattedText>(
        request.text, std::vector<td_api::object_ptr<td_api::textEntity>>{});
    auto content = td_api::make_object<td_api::inputMessageText>(std::move(text), nullptr, false);
    NativeFunctionPtr native = td_api::make_object<td_api::editMessageText>(
        request.chat_id, request.message_id, nullptr, std::move(content));
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::EditMessageText,
                                            {{"chat_id", request.chat_id},
                                             {"message_id", request.message_id},
                                             {"reply_markup_is_null", true},
                                             {"text", std::move(request.text)},
                                             {"entities_count", std::int64_t{0}},
                                             {"link_preview_options_is_null", true},
                                             {"clear_draft", false}}});
}

TdValue make_native_delete_messages(TdDeleteMessagesRequest request) {
    require_direct_request(request);
    auto descriptor_ids = request.message_ids;
    NativeFunctionPtr native = td_api::make_object<td_api::deleteMessages>(
        request.chat_id, std::move(request.message_ids), request.revoke);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::DeleteMessages,
                                            {{"chat_id", request.chat_id},
                                             {"message_ids", std::move(descriptor_ids)},
                                             {"revoke", request.revoke}}});
}

TdValue make_native_message_reaction(TdMessageReactionRequest request) {
    require_direct_request(request);
    auto reaction = td_api::make_object<td_api::reactionTypeEmoji>(request.reaction);
    const auto function =
        request.remove ? TdFunctionKind::RemoveMessageReaction : TdFunctionKind::AddMessageReaction;
    NativeFunctionPtr native;
    std::vector<TdFunctionField> fields{{"chat_id", request.chat_id},
                                        {"message_id", request.message_id},
                                        {"reaction", std::move(request.reaction)}};
    if (request.remove) {
        native = td_api::make_object<td_api::removeMessageReaction>(
            request.chat_id, request.message_id, std::move(reaction));
    } else {
        native = td_api::make_object<td_api::addMessageReaction>(
            request.chat_id, request.message_id, std::move(reaction), request.big, true);
        fields.emplace_back("is_big", request.big);
        fields.emplace_back("update_recent_reactions", true);
    }
    return TdValue::function(std::move(native), TdFunctionData{function, std::move(fields)});
}

TdValue make_native_pin_message(TdPinMessageRequest request) {
    require_direct_request(request);
    const auto function =
        request.pinned ? TdFunctionKind::PinChatMessage : TdFunctionKind::UnpinChatMessage;
    NativeFunctionPtr native;
    std::vector<TdFunctionField> fields{{"chat_id", request.chat_id},
                                        {"message_id", request.message_id}};
    if (request.pinned) {
        native = td_api::make_object<td_api::pinChatMessage>(request.chat_id, request.message_id,
                                                             false, false);
        fields.emplace_back("disable_notification", false);
        fields.emplace_back("only_for_self", false);
    } else {
        native = td_api::make_object<td_api::unpinChatMessage>(request.chat_id, request.message_id);
    }
    return TdValue::function(std::move(native), TdFunctionData{function, std::move(fields)});
}

TdValue make_native_view_messages(TdViewMessagesRequest request) {
    require_direct_request(request);
    auto descriptor_ids = request.message_ids;
    NativeFunctionPtr native = td_api::make_object<td_api::viewMessages>(
        request.chat_id, std::move(request.message_ids), nullptr, true);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::ViewMessages,
                                            {{"chat_id", request.chat_id},
                                             {"message_ids", std::move(descriptor_ids)},
                                             {"source_is_null", true},
                                             {"force_read", true}}});
}

std::vector<TdFunctionField>
describe_notification_settings(const TdSetChatNotificationSettingsRequest& request) {
    const auto& settings = request.settings;
    return {{"chat_id", request.chat_id},
            {"use_default_mute_for", settings.use_default_mute_for},
            {"mute_for", static_cast<std::int64_t>(settings.mute_for)},
            {"use_default_sound", settings.use_default_sound},
            {"sound_id", settings.sound_id},
            {"use_default_show_preview", settings.use_default_show_preview},
            {"show_preview", settings.show_preview},
            {"use_default_mute_stories", settings.use_default_mute_stories},
            {"mute_stories", settings.mute_stories},
            {"use_default_story_sound", settings.use_default_story_sound},
            {"story_sound_id", settings.story_sound_id},
            {"use_default_show_story_poster", settings.use_default_show_story_poster},
            {"show_story_poster", settings.show_story_poster},
            {"use_default_disable_pinned_message_notifications",
             settings.use_default_disable_pinned_message_notifications},
            {"disable_pinned_message_notifications", settings.disable_pinned_message_notifications},
            {"use_default_disable_mention_notifications",
             settings.use_default_disable_mention_notifications},
            {"disable_mention_notifications", settings.disable_mention_notifications}};
}

TdValue make_native_set_chat_notification_settings(TdSetChatNotificationSettingsRequest request) {
    require_direct_request(request);
    const auto& value = request.settings;
    auto settings = td_api::make_object<td_api::chatNotificationSettings>(
        value.use_default_mute_for, value.mute_for, value.use_default_sound, value.sound_id,
        value.use_default_show_preview, value.show_preview, value.use_default_mute_stories,
        value.mute_stories, value.use_default_story_sound, value.story_sound_id,
        value.use_default_show_story_poster, value.show_story_poster,
        value.use_default_disable_pinned_message_notifications,
        value.disable_pinned_message_notifications, value.use_default_disable_mention_notifications,
        value.disable_mention_notifications);
    NativeFunctionPtr native = td_api::make_object<td_api::setChatNotificationSettings>(
        request.chat_id, std::move(settings));
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::SetChatNotificationSettings,
                                            describe_notification_settings(request)});
}

TdValue make_native_toggle_chat_is_pinned(TdToggleChatIsPinnedRequest request) {
    require_direct_request(request);
    auto list = make_direct_chat_list(request.list);
    NativeFunctionPtr native = td_api::make_object<td_api::toggleChatIsPinned>(
        std::move(list), request.chat_id, request.pinned);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::ToggleChatIsPinned,
                                            {{"chat_list", direct_chat_list_name(request.list)},
                                             {"chat_id", request.chat_id},
                                             {"is_pinned", request.pinned}}});
}

TdValue make_native_add_chat_to_list(TdAddChatToListRequest request) {
    require_direct_request(request);
    auto list = make_direct_chat_list(request.list);
    NativeFunctionPtr native =
        td_api::make_object<td_api::addChatToList>(request.chat_id, std::move(list));
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::AddChatToList,
                                            {{"chat_id", request.chat_id},
                                             {"chat_list", direct_chat_list_name(request.list)}}});
}

TdValue make_native_join_chat(TdJoinChatRequest request) {
    require_direct_request(request);
    if (request.chat_id.has_value()) {
        NativeFunctionPtr native = td_api::make_object<td_api::joinChat>(*request.chat_id);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::JoinChat, {{"chat_id", *request.chat_id}}});
    }
    auto invite_link = std::move(request.invite_link).value_or(std::string{});
    NativeFunctionPtr native = td_api::make_object<td_api::joinChatByInviteLink>(invite_link);
    return TdValue::function(std::move(native),
                             TdFunctionData{TdFunctionKind::JoinChatByInviteLink,
                                            {{"invite_link", std::move(invite_link)}}});
}

TdValue make_native_leave_chat(TdLeaveChatRequest request) {
    require_direct_request(request);
    NativeFunctionPtr native = td_api::make_object<td_api::leaveChat>(request.chat_id);
    return TdValue::function(std::move(native), TdFunctionData{TdFunctionKind::LeaveChat,
                                                               {{"chat_id", request.chat_id}}});
}

void enforce_error_verbosity() {
    td::Log::set_verbosity_level(kTdLogVerbosity);
}

struct ProcessLogState {
    std::mutex mutex;
    std::shared_ptr<TdLogSink> sink;
    std::atomic<bool> failure_reported{false};
    std::atomic<bool> json_diagnostics{false};
};

ProcessLogState& process_log_state() {
    static ProcessLogState state;
    return state;
}

constexpr std::string_view kHumanLogFailureMessage =
    "warning: TDLib log sink failed; further records suppressed\n";
constexpr std::string_view kJsonLogFailureMessage =
    "{\"warning\":\"TDLib log sink failed; further records suppressed\"}\n";
static_assert(kHumanLogFailureMessage.size() <= PIPE_BUF);
static_assert(kJsonLogFailureMessage.size() <= PIPE_BUF);

std::string_view process_log_failure_message(bool json) {
    return json ? kJsonLogFailureMessage : kHumanLogFailureMessage;
}

ssize_t write_process_log_message(void* /*context*/, int fd, const void* bytes,
                                  std::size_t size) noexcept {
    return ::write(fd, bytes, size);
}

void write_process_log_message_best_effort(std::string_view message,
                                           detail::ProcessLogWriteFunction writer,
                                           void* context) noexcept {
    std::size_t offset = 0;
    while (offset < message.size()) {
        const auto count =
            writer(context, STDERR_FILENO, message.data() + offset, message.size() - offset);
        if (count > 0) {
            const auto progress = static_cast<std::size_t>(count);
            if (progress >= message.size() - offset) {
                break;
            }
            offset += progress;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

void report_process_log_failure(detail::ProcessLogWriteFunction writer, void* context) noexcept {
    const int incoming_errno = errno;
    if (process_log_state().failure_reported.exchange(true, std::memory_order_relaxed)) {
        errno = incoming_errno;
        return;
    }
    const auto message = process_log_failure_message(
        process_log_state().json_diagnostics.load(std::memory_order_relaxed));
    write_process_log_message_best_effort(message, writer, context);
    errno = incoming_errno;
}

void report_process_log_failure() noexcept {
    report_process_log_failure(write_process_log_message, nullptr);
}

void td_log_callback(int verbosity, const char* message) noexcept {
    try {
        std::shared_ptr<TdLogSink> sink;
        {
            const std::lock_guard<std::mutex> lock(process_log_state().mutex);
            sink = process_log_state().sink;
        }
        if (sink == nullptr || message == nullptr) {
            return;
        }
        std::string error;
        if (!sink->append(verbosity, message, error)) {
            report_process_log_failure();
        }
    } catch (const std::exception&) {
        report_process_log_failure();
    } catch (...) {
        // A C callback cannot propagate any non-standard exception through TDLib.
        report_process_log_failure();
    }
}

AuthCodeDeliveryInfo code_delivery(const td_api::AuthenticationCodeType& type) {
    switch (type.get_id()) {
    case td_api::authenticationCodeTypeTelegramMessage::ID:
        return {AuthCodeDelivery::TelegramMessage,
                static_cast<const td_api::authenticationCodeTypeTelegramMessage&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeSms::ID:
        return {AuthCodeDelivery::Sms,
                static_cast<const td_api::authenticationCodeTypeSms&>(type).length_, std::nullopt};
    case td_api::authenticationCodeTypeSmsWord::ID:
        return {AuthCodeDelivery::SmsWord, std::nullopt, std::nullopt};
    case td_api::authenticationCodeTypeSmsPhrase::ID:
        return {AuthCodeDelivery::SmsPhrase, std::nullopt, std::nullopt};
    case td_api::authenticationCodeTypeCall::ID:
        return {AuthCodeDelivery::Call,
                static_cast<const td_api::authenticationCodeTypeCall&>(type).length_, std::nullopt};
    case td_api::authenticationCodeTypeFlashCall::ID:
        return {AuthCodeDelivery::FlashCall, std::nullopt, std::nullopt};
    case td_api::authenticationCodeTypeMissedCall::ID:
        return {AuthCodeDelivery::MissedCall,
                static_cast<const td_api::authenticationCodeTypeMissedCall&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeFragment::ID:
        return {AuthCodeDelivery::Fragment,
                static_cast<const td_api::authenticationCodeTypeFragment&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeFirebaseAndroid::ID:
        return {AuthCodeDelivery::FirebaseAndroid,
                static_cast<const td_api::authenticationCodeTypeFirebaseAndroid&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeFirebaseIos::ID:
        return {AuthCodeDelivery::FirebaseIos,
                static_cast<const td_api::authenticationCodeTypeFirebaseIos&>(type).length_,
                std::nullopt};
    default:
        return {AuthCodeDelivery::Unknown, std::nullopt, type.get_id()};
    }
}

struct EmailResetData {
    AuthEmailResetState state = AuthEmailResetState::None;
    std::int32_t delay = 0;
    std::optional<std::int32_t> unsupported_tdlib_type_id;
};

EmailResetData email_reset_state(const td_api::EmailAddressResetState* state) {
    if (state == nullptr) {
        return {};
    }
    switch (state->get_id()) {
    case td_api::emailAddressResetStateAvailable::ID:
        return {AuthEmailResetState::Available,
                static_cast<const td_api::emailAddressResetStateAvailable&>(*state).wait_period_,
                std::nullopt};
    case td_api::emailAddressResetStatePending::ID:
        return {AuthEmailResetState::Pending,
                static_cast<const td_api::emailAddressResetStatePending&>(*state).reset_in_,
                std::nullopt};
    default:
        return {AuthEmailResetState::Unknown, 0, state->get_id()};
    }
}

AuthStateData auth_state_data(const td_api::AuthorizationState& state) {
    switch (state.get_id()) {
    case td_api::authorizationStateWaitTdlibParameters::ID:
        return AuthStateData{AuthState::WaitTdlibParameters};
    case td_api::authorizationStateWaitPhoneNumber::ID:
        return AuthStateData{AuthState::WaitPhoneNumber};
    case td_api::authorizationStateWaitPremiumPurchase::ID: {
        const auto& value =
            static_cast<const td_api::authorizationStateWaitPremiumPurchase&>(state);
        return AuthStateData{
            AuthState::WaitPremiumPurchase,
            AuthWaitPremiumPurchase{.store_product_id = value.store_product_id_,
                                    .premium_day_count = value.premium_day_count_,
                                    .support_email_address = value.support_email_address_,
                                    .support_email_subject = value.support_email_subject_}};
    }
    case td_api::authorizationStateWaitEmailAddress::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitEmailAddress&>(state);
        return AuthStateData{AuthState::WaitEmailAddress,
                             AuthWaitEmailAddress{.allow_apple_id = value.allow_apple_id_,
                                                  .allow_google_id = value.allow_google_id_}};
    }
    case td_api::authorizationStateWaitEmailCode::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitEmailCode&>(state);
        const auto reset = email_reset_state(value.email_address_reset_state_.get());
        AuthWaitEmailCode metadata{.allow_apple_id = value.allow_apple_id_,
                                   .allow_google_id = value.allow_google_id_,
                                   .email_address_pattern = {},
                                   .expected_length = 0,
                                   .reset_state = reset.state,
                                   .reset_delay = reset.delay,
                                   .unsupported_reset_tdlib_type_id =
                                       reset.unsupported_tdlib_type_id};
        if (value.code_info_ != nullptr) {
            metadata.email_address_pattern = value.code_info_->email_address_pattern_;
            metadata.expected_length = value.code_info_->length_;
        }
        return AuthStateData{AuthState::WaitEmailCode, std::move(metadata)};
    }
    case td_api::authorizationStateWaitCode::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitCode&>(state);
        AuthWaitCode metadata;
        if (value.code_info_ != nullptr) {
            if (value.code_info_->type_ != nullptr) {
                metadata.delivery = code_delivery(*value.code_info_->type_);
            }
            if (value.code_info_->next_type_ != nullptr) {
                metadata.next_delivery = code_delivery(*value.code_info_->next_type_);
            }
            metadata.resend_timeout = value.code_info_->timeout_;
        }
        return AuthStateData{AuthState::WaitCode, metadata};
    }
    case td_api::authorizationStateWaitOtherDeviceConfirmation::ID: {
        const auto& value =
            static_cast<const td_api::authorizationStateWaitOtherDeviceConfirmation&>(state);
        return AuthStateData{AuthState::WaitOtherDeviceConfirmation,
                             AuthWaitOtherDeviceConfirmation{.link = value.link_}};
    }
    case td_api::authorizationStateWaitRegistration::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitRegistration&>(state);
        AuthWaitRegistration metadata;
        if (value.terms_of_service_ != nullptr) {
            if (value.terms_of_service_->text_ != nullptr) {
                metadata.terms_text = value.terms_of_service_->text_->text_;
            }
            metadata.minimum_user_age = value.terms_of_service_->min_user_age_;
            metadata.show_popup = value.terms_of_service_->show_popup_;
        }
        return AuthStateData{AuthState::WaitRegistration, std::move(metadata)};
    }
    case td_api::authorizationStateWaitPassword::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitPassword&>(state);
        return AuthStateData{
            AuthState::WaitPassword,
            AuthWaitPassword{.hint = value.password_hint_,
                             .has_recovery_email_address = value.has_recovery_email_address_,
                             .has_passport_data = value.has_passport_data_,
                             .recovery_email_address_pattern =
                                 value.recovery_email_address_pattern_}};
    }
    case td_api::authorizationStateReady::ID:
        return AuthStateData{AuthState::Ready};
    case td_api::authorizationStateLoggingOut::ID:
        return AuthStateData{AuthState::LoggingOut};
    case td_api::authorizationStateClosing::ID:
        return AuthStateData{AuthState::Closing};
    case td_api::authorizationStateClosed::ID:
        return AuthStateData{AuthState::Closed};
    default:
        return AuthStateData{AuthState::Unknown, {}, state.get_id()};
    }
}

std::optional<AuthStateData> extract_auth_state(const td_api::Object& object,
                                                bool authorization_state_response) {
    if (object.get_id() == td_api::updateAuthorizationState::ID) {
        const auto& update = static_cast<const td_api::updateAuthorizationState&>(object);
        if (update.authorization_state_ == nullptr) {
            return std::nullopt;
        }
        return auth_state_data(*update.authorization_state_);
    }

    if (authorization_state_response && object.get_id() != td_api::error::ID) {
        return auth_state_data(static_cast<const td_api::AuthorizationState&>(object));
    }
    return std::nullopt;
}

class ProductionTdRuntime final : public TdRuntime {
  public:
    ProductionTdRuntime() = default;
    ProductionTdRuntime(const ProductionTdRuntime&) = delete;
    ProductionTdRuntime& operator=(const ProductionTdRuntime&) = delete;
    ProductionTdRuntime(ProductionTdRuntime&&) = delete;
    ProductionTdRuntime& operator=(ProductionTdRuntime&&) = delete;

    ~ProductionTdRuntime() override {
        manager_.reset();
        const std::lock_guard<std::mutex> lock(process_log_state().mutex);
        if (process_log_state().sink == log_sink_) {
            td::ClientManager::set_log_message_callback(0, nullptr);
            process_log_state().sink.reset();
        }
    }

    void initialize_process(const TdLogConfiguration& logging) override {
        std::string error;
        auto sink = TdLogSink::create(logging, ::getuid(), error);
        if (sink == nullptr) {
            throw std::runtime_error("cannot initialize TDLib logging: " + error);
        }

        {
            const std::lock_guard<std::mutex> lock(process_log_state().mutex);
            if (process_log_state().sink != nullptr) {
                throw std::runtime_error("TDLib process logging is already initialized");
            }
            enforce_error_verbosity();
            auto stream_result =
                td::ClientManager::execute(td_api::make_object<td_api::setLogStream>(
                    td_api::make_object<td_api::logStreamEmpty>()));
            if (stream_result == nullptr || stream_result->get_id() != td_api::ok::ID) {
                throw std::runtime_error("cannot disable TDLib's default log stream");
            }
            log_sink_ = std::shared_ptr<TdLogSink>(std::move(sink));
            process_log_state().sink = log_sink_;
            process_log_state().failure_reported.store(false, std::memory_order_relaxed);
            process_log_state().json_diagnostics.store(logging.json_diagnostics,
                                                       std::memory_order_relaxed);
            td::ClientManager::set_log_message_callback(kTdLogVerbosity, td_log_callback);
        }
        try {
            manager_ = std::make_unique<td::ClientManager>();
        } catch (...) {
            // Allocation and TDLib construction failures share the same global cleanup.
            const std::lock_guard<std::mutex> lock(process_log_state().mutex);
            td::ClientManager::set_log_message_callback(0, nullptr);
            process_log_state().sink.reset();
            log_sink_.reset();
            throw;
        }
    }

    std::int32_t create_client(std::uint64_t client_generation) override {
        if (manager_ == nullptr) {
            throw std::logic_error("TDLib process is not initialized");
        }
        const auto client_id = manager_->create_client_id();
        const std::lock_guard<std::mutex> lock(generations_mutex_);
        generations_.insert_or_assign(client_id, client_generation);
        return client_id;
    }

    TdValue make_function(TdBuiltinFunction function) override {
        switch (function) {
        case TdBuiltinFunction::GetAuthorizationState: {
            NativeFunctionPtr native = td_api::make_object<td_api::getAuthorizationState>();
            return TdValue::function(std::move(native),
                                     TdFunctionData{TdFunctionKind::GetAuthorizationState});
        }
        case TdBuiltinFunction::LogOut: {
            NativeFunctionPtr native = td_api::make_object<td_api::logOut>();
            return TdValue::function(std::move(native), TdFunctionData{TdFunctionKind::LogOut});
        }
        case TdBuiltinFunction::Close: {
            NativeFunctionPtr native = td_api::make_object<td_api::close>();
            return TdValue::function(std::move(native), TdFunctionData{TdFunctionKind::Close});
        }
        }
        throw std::logic_error("unknown built-in TDLib function");
    }

    TdValue make_set_tdlib_parameters(TdlibParameters parameters) override {
        auto description = describe_tdlib_parameters(parameters);
        NativeFunctionPtr native = td_api::make_object<td_api::setTdlibParameters>(
            parameters.use_test_dc, parameters.database_directory, parameters.files_directory,
            parameters.database_encryption_key, parameters.use_file_database,
            parameters.use_chat_info_database, parameters.use_message_database,
            parameters.use_secret_chats, parameters.api_id, parameters.api_hash,
            parameters.system_language_code, parameters.device_model, parameters.system_version,
            parameters.application_version);
        wipe(parameters.database_encryption_key);
        wipe(parameters.api_hash);
        return TdValue::function(std::move(native), std::move(description));
    }

    TdValue make_auth_function(TdAuthRequest request) override {
        return make_native_auth_function(std::move(request));
    }

    TdValue make_get_saved_messages_tags(std::int64_t saved_messages_topic_id) override {
        NativeFunctionPtr native =
            td_api::make_object<td_api::getSavedMessagesTags>(saved_messages_topic_id);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::GetSavedMessagesTags,
                           {{"saved_messages_topic_id", saved_messages_topic_id}}});
    }

    TdValue make_search_saved_messages(TdSearchSavedMessagesRequest request) override {
        const std::string selector = request.tag.kind == TdReactionKind::Emoji
                                         ? request.tag.emoji
                                         : "custom:" + std::to_string(request.tag.custom_emoji_id);
        auto tag = make_native_reaction(request.tag);
        NativeFunctionPtr native = td_api::make_object<td_api::searchSavedMessages>(
            request.saved_messages_topic_id, std::move(tag), request.query, request.from_message_id,
            request.offset, request.limit);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::SearchSavedMessages,
                           {{"saved_messages_topic_id", request.saved_messages_topic_id},
                            {"tag", selector},
                            {"query", request.query},
                            {"from_message_id", request.from_message_id},
                            {"offset", static_cast<std::int64_t>(request.offset)},
                            {"limit", static_cast<std::int64_t>(request.limit)}}});
    }

    TdValue make_get_active_sessions() override {
        return make_native_get_active_sessions();
    }

    TdValue make_terminate_session(std::int64_t session_id) override {
        return make_native_terminate_session(session_id);
    }

    TdValue make_get_chat(std::int64_t chat_id) override {
        NativeFunctionPtr native = td_api::make_object<td_api::getChat>(chat_id);
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::GetChat, {{"chat_id", chat_id}}});
    }

    TdValue make_get_chat_history(std::int64_t chat_id, std::int64_t from_message_id,
                                  std::int32_t offset, std::int32_t limit,
                                  bool only_local) override {
        return make_native_get_chat_history(chat_id, from_message_id, offset, limit, only_local);
    }

    TdValue make_get_chat_message_by_date(std::int64_t chat_id, std::int32_t date) override {
        return make_native_get_chat_message_by_date(chat_id, date);
    }

    TdValue make_get_message_thread(std::int64_t chat_id, std::int64_t message_id) override {
        return make_native_get_message_thread(chat_id, message_id);
    }

    TdValue make_get_forum_topic_history(std::int64_t chat_id, std::int32_t forum_topic_id,
                                         std::int64_t from_message_id, std::int32_t offset,
                                         std::int32_t limit) override {
        return make_native_get_forum_topic_history(chat_id, forum_topic_id, from_message_id, offset,
                                                   limit);
    }

    TdValue make_get_message_thread_history(std::int64_t chat_id, std::int64_t message_id,
                                            std::int64_t from_message_id, std::int32_t offset,
                                            std::int32_t limit) override {
        return make_native_get_message_thread_history(chat_id, message_id, from_message_id, offset,
                                                      limit);
    }

    TdValue make_get_direct_messages_chat_topic_history(std::int64_t chat_id, std::int64_t topic_id,
                                                        std::int64_t from_message_id,
                                                        std::int32_t offset,
                                                        std::int32_t limit) override {
        return make_native_get_direct_messages_chat_topic_history(chat_id, topic_id,
                                                                  from_message_id, offset, limit);
    }

    TdValue make_get_saved_messages_topic_history(std::int64_t topic_id,
                                                  std::int64_t from_message_id, std::int32_t offset,
                                                  std::int32_t limit) override {
        return make_native_get_saved_messages_topic_history(topic_id, from_message_id, offset,
                                                            limit);
    }

    TdValue make_get_messages(std::int64_t chat_id,
                              std::vector<std::int64_t> message_ids) override {
        return make_native_get_messages(chat_id, std::move(message_ids));
    }

    TdValue make_get_message_link(std::int64_t chat_id, std::int64_t message_id,
                                  std::int32_t media_timestamp, std::int32_t checklist_task_id,
                                  std::string poll_option_id, bool for_album,
                                  bool in_message_thread) override {
        return make_native_get_message_link(chat_id, message_id, media_timestamp, checklist_task_id,
                                            std::move(poll_option_id), for_album,
                                            in_message_thread);
    }

    TdValue make_get_chats(TdChatList list, std::int32_t limit) override {
        NativeFunctionPtr native =
            td_api::make_object<td_api::getChats>(make_chat_list(list), limit);
        std::vector<TdFunctionField> fields{{"list", std::string(chat_list_name(list.kind))},
                                            {"limit", static_cast<std::int64_t>(limit)}};
        if (list.kind == TdChatListKind::Folder) {
            fields.emplace_back("folder_id", static_cast<std::int64_t>(list.folder_id));
        }
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::GetChats, std::move(fields)});
    }

    TdValue make_load_chats(TdChatList list, std::int32_t limit) override {
        NativeFunctionPtr native =
            td_api::make_object<td_api::loadChats>(make_chat_list(list), limit);
        std::vector<TdFunctionField> fields{{"list", std::string(chat_list_name(list.kind))},
                                            {"limit", static_cast<std::int64_t>(limit)}};
        if (list.kind == TdChatListKind::Folder) {
            fields.emplace_back("folder_id", static_cast<std::int64_t>(list.folder_id));
        }
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::LoadChats, std::move(fields)});
    }

    TdValue make_search_public_chat(std::string username) override {
        NativeFunctionPtr native = td_api::make_object<td_api::searchPublicChat>(username);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::SearchPublicChat, {{"username", std::move(username)}}});
    }

    TdValue make_get_internal_link_type(std::string link) override {
        NativeFunctionPtr native = td_api::make_object<td_api::getInternalLinkType>(link);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::GetInternalLinkType, {{"link", std::move(link)}}});
    }

    TdValue make_get_message_link_info(std::string url) override {
        NativeFunctionPtr native = td_api::make_object<td_api::getMessageLinkInfo>(url);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::GetMessageLinkInfo, {{"url", std::move(url)}}});
    }

    TdValue make_check_chat_invite_link(std::string link) override {
        NativeFunctionPtr native = td_api::make_object<td_api::checkChatInviteLink>(link);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::CheckChatInviteLink, {{"link", std::move(link)}}});
    }

    TdValue make_get_user(std::int64_t user_id) override {
        NativeFunctionPtr native = td_api::make_object<td_api::getUser>(user_id);
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::GetUser, {{"user_id", user_id}}});
    }

    TdValue make_get_supergroup(std::int64_t supergroup_id) override {
        NativeFunctionPtr native = td_api::make_object<td_api::getSupergroup>(supergroup_id);
        return TdValue::function(
            std::move(native),
            TdFunctionData{TdFunctionKind::GetSupergroup, {{"supergroup_id", supergroup_id}}});
    }

    TdValue make_get_supergroup_full_info(std::int64_t supergroup_id) override {
        NativeFunctionPtr native =
            td_api::make_object<td_api::getSupergroupFullInfo>(supergroup_id);
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::GetSupergroupFullInfo,
                                                {{"supergroup_id", supergroup_id}}});
    }

    TdValue make_create_private_chat(std::int64_t user_id, bool force) override {
        NativeFunctionPtr native = td_api::make_object<td_api::createPrivateChat>(user_id, force);
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::CreatePrivateChat,
                                                {{"user_id", user_id}, {"force", force}}});
    }

    TdValue make_get_message(std::int64_t chat_id, std::int64_t message_id) override {
        return make_native_get_message(chat_id, message_id);
    }

    TdValue make_get_message_properties(std::int64_t chat_id, std::int64_t message_id) override {
        return make_native_get_message_properties(chat_id, message_id);
    }

    TdValue make_get_message_available_reactions(std::int64_t chat_id,
                                                 std::int64_t message_id) override {
        return make_native_get_message_available_reactions(chat_id, message_id);
    }

    TdValue make_get_unix_time() override {
        return make_native_get_unix_time();
    }

    TdValue make_parse_text_entities(std::string text, TdTextParseMode mode) override {
        return make_native_parse_text_entities(std::move(text), mode);
    }

    TdValue make_send_message(TdSendMessageRequest request,
                              std::uint64_t client_generation) override {
        return make_native_send_message(std::move(request), client_generation);
    }

    TdValue make_edit_message_text(TdEditMessageTextRequest request) override {
        return make_native_edit_message_text(std::move(request));
    }

    TdValue make_delete_messages(TdDeleteMessagesRequest request) override {
        return make_native_delete_messages(std::move(request));
    }

    TdValue make_message_reaction(TdMessageReactionRequest request) override {
        return make_native_message_reaction(std::move(request));
    }

    TdValue make_pin_message(TdPinMessageRequest request) override {
        return make_native_pin_message(request);
    }

    TdValue make_view_messages(TdViewMessagesRequest request) override {
        return make_native_view_messages(std::move(request));
    }

    TdValue
    make_set_chat_notification_settings(TdSetChatNotificationSettingsRequest request) override {
        return make_native_set_chat_notification_settings(request);
    }

    TdValue make_toggle_chat_is_pinned(TdToggleChatIsPinnedRequest request) override {
        return make_native_toggle_chat_is_pinned(request);
    }

    TdValue make_add_chat_to_list(TdAddChatToListRequest request) override {
        return make_native_add_chat_to_list(request);
    }

    TdValue make_join_chat(TdJoinChatRequest request) override {
        return make_native_join_chat(std::move(request));
    }

    TdValue make_leave_chat(TdLeaveChatRequest request) override {
        return make_native_leave_chat(request);
    }

    void send(std::int32_t client_id, std::uint64_t client_generation, std::uint64_t query_id,
              TdValue function) override {
        static_cast<void>(client_generation);
        auto* native_function = function.get_if<NativeFunctionPtr>();
        if (native_function == nullptr || *native_function == nullptr) {
            throw std::invalid_argument("TdClient request does not contain a native function");
        }
        const auto& function_data = function.function_data();
        const auto function_kind =
            function_data ? function_data->kind() : std::optional<TdFunctionKind>{};
        if (!function_kind || !native_function_matches(**native_function, *function_kind)) {
            throw std::invalid_argument("TdClient native function does not match its descriptor");
        }
        const bool is_authorization_state_query =
            (*native_function)->get_id() == td_api::getAuthorizationState::ID;
        {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            response_functions_[client_id].insert_or_assign(query_id, *function_kind);
            if (is_authorization_state_query) {
                authorization_queries_[client_id].insert(query_id);
            }
        }
        try {
            manager_->send(client_id, query_id, std::move(*native_function));
        } catch (...) {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            const auto functions = response_functions_.find(client_id);
            if (functions != response_functions_.end()) {
                functions->second.erase(query_id);
                if (functions->second.empty()) {
                    response_functions_.erase(functions);
                }
            }
            const auto authorization = authorization_queries_.find(client_id);
            if (authorization != authorization_queries_.end()) {
                authorization->second.erase(query_id);
                if (authorization->second.empty()) {
                    authorization_queries_.erase(authorization);
                }
            }
            throw;
        }
    }

    std::optional<TdRuntimeEvent> receive(std::chrono::milliseconds timeout) override {
        auto response = manager_->receive(static_cast<double>(timeout.count()) / 1000.0);
        if (response.object == nullptr) {
            return std::nullopt;
        }

        std::uint64_t generation = 0;
        bool authorization_state_response = false;
        std::optional<TdFunctionKind> response_function;
        {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            const auto it = generations_.find(response.client_id);
            if (it == generations_.end()) {
                return std::nullopt;
            }
            generation = it->second;
            const auto functions = response_functions_.find(response.client_id);
            if (functions != response_functions_.end()) {
                const auto function = functions->second.find(response.request_id);
                if (function != functions->second.end()) {
                    response_function = function->second;
                    functions->second.erase(function);
                }
                if (functions->second.empty()) {
                    response_functions_.erase(functions);
                }
            }
            const auto queries = authorization_queries_.find(response.client_id);
            if (queries != authorization_queries_.end()) {
                authorization_state_response = queries->second.erase(response.request_id) != 0;
                if (queries->second.empty()) {
                    authorization_queries_.erase(queries);
                }
            }
        }

        auto authorization_state =
            extract_auth_state(*response.object, authorization_state_response);
        if (authorization_state.has_value() && authorization_state->state == AuthState::Closed) {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            generations_.erase(response.client_id);
            authorization_queries_.erase(response.client_id);
            response_functions_.erase(response.client_id);
        }
        return TdRuntimeEvent{.client_id = response.client_id,
                              .client_generation = generation,
                              .query_id = response.request_id,
                              .object =
                                  response_function
                                      ? convert_response_for(*response_function,
                                                             std::move(response.object), generation)
                                      : convert_response(std::move(response.object), generation),
                              .authorization_state = std::move(authorization_state)};
    }

  private:
    std::unique_ptr<td::ClientManager> manager_;
    std::shared_ptr<TdLogSink> log_sink_;
    std::mutex generations_mutex_;
    std::unordered_map<std::int32_t, std::uint64_t> generations_;
    std::unordered_map<std::int32_t, std::unordered_set<std::uint64_t>> authorization_queries_;
    std::unordered_map<std::int32_t, std::unordered_map<std::uint64_t, TdFunctionKind>>
        response_functions_;
};

} // namespace

namespace detail {

TdValue convert_production_response_for_test(TdValue object) {
    auto* native = object.get_if<NativeObjectPtr>();
    if (native == nullptr) {
        return {};
    }
    return convert_response(std::move(*native));
}

TdValue convert_production_sessions_for_test(TdValue object) {
    auto* native = object.get_if<NativeObjectPtr>();
    if (native == nullptr || *native == nullptr) {
        return session_conversion_error();
    }
    if ((*native)->get_id() != td_api::sessions::ID) {
        return session_conversion_error((*native)->get_id());
    }
    return convert_sessions(td_api::move_object_as<td_api::sessions>(std::move(*native)));
}

TdValue make_production_get_active_sessions_for_test() {
    return make_native_get_active_sessions();
}

TdValue make_production_terminate_session_for_test(std::int64_t session_id) {
    return make_native_terminate_session(session_id);
}

TdValue make_production_get_chat_history_for_test(std::int64_t chat_id,
                                                  std::int64_t from_message_id, std::int32_t offset,
                                                  std::int32_t limit, bool only_local) {
    return make_native_get_chat_history(chat_id, from_message_id, offset, limit, only_local);
}

TdValue make_production_get_chat_message_by_date_for_test(std::int64_t chat_id, std::int32_t date) {
    return make_native_get_chat_message_by_date(chat_id, date);
}

TdValue make_production_get_message_thread_for_test(std::int64_t chat_id, std::int64_t message_id) {
    return make_native_get_message_thread(chat_id, message_id);
}

TdValue make_production_get_forum_topic_history_for_test(std::int64_t chat_id,
                                                         std::int32_t forum_topic_id,
                                                         std::int64_t from_message_id,
                                                         std::int32_t offset, std::int32_t limit) {
    return make_native_get_forum_topic_history(chat_id, forum_topic_id, from_message_id, offset,
                                               limit);
}

TdValue make_production_get_message_thread_history_for_test(std::int64_t chat_id,
                                                            std::int64_t message_id,
                                                            std::int64_t from_message_id,
                                                            std::int32_t offset,
                                                            std::int32_t limit) {
    return make_native_get_message_thread_history(chat_id, message_id, from_message_id, offset,
                                                  limit);
}

TdValue make_production_get_direct_messages_chat_topic_history_for_test(
    std::int64_t chat_id, std::int64_t topic_id, std::int64_t from_message_id, std::int32_t offset,
    std::int32_t limit) {
    return make_native_get_direct_messages_chat_topic_history(chat_id, topic_id, from_message_id,
                                                              offset, limit);
}

TdValue make_production_get_saved_messages_topic_history_for_test(std::int64_t topic_id,
                                                                  std::int64_t from_message_id,
                                                                  std::int32_t offset,
                                                                  std::int32_t limit) {
    return make_native_get_saved_messages_topic_history(topic_id, from_message_id, offset, limit);
}

TdValue make_production_get_messages_for_test(std::int64_t chat_id,
                                              std::vector<std::int64_t> message_ids) {
    return make_native_get_messages(chat_id, std::move(message_ids));
}

TdValue make_production_get_message_link_for_test(std::int64_t chat_id, std::int64_t message_id,
                                                  std::int32_t media_timestamp,
                                                  std::int32_t checklist_task_id,
                                                  std::string poll_option_id, bool for_album,
                                                  bool in_message_thread) {
    return make_native_get_message_link(chat_id, message_id, media_timestamp, checklist_task_id,
                                        std::move(poll_option_id), for_album, in_message_thread);
}

TdValue make_production_get_message_for_test(std::int64_t chat_id, std::int64_t message_id) {
    return make_native_get_message(chat_id, message_id);
}

TdValue make_production_get_message_properties_for_test(std::int64_t chat_id,
                                                        std::int64_t message_id) {
    return make_native_get_message_properties(chat_id, message_id);
}

TdValue make_production_get_message_available_reactions_for_test(std::int64_t chat_id,
                                                                 std::int64_t message_id) {
    return make_native_get_message_available_reactions(chat_id, message_id);
}

TdValue make_production_get_unix_time_for_test() {
    return make_native_get_unix_time();
}

TdValue make_production_parse_text_entities_for_test(std::string text, TdTextParseMode mode) {
    return make_native_parse_text_entities(std::move(text), mode);
}

TdValue make_production_send_message_for_test(TdSendMessageRequest request,
                                              std::uint64_t client_generation) {
    return make_native_send_message(std::move(request), client_generation);
}

TdValue make_production_direct_request_for_test(const TdDirectRequest& request) {
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, TdEditMessageTextRequest>) {
                return make_native_edit_message_text(value);
            } else if constexpr (std::is_same_v<Request, TdDeleteMessagesRequest>) {
                return make_native_delete_messages(value);
            } else if constexpr (std::is_same_v<Request, TdMessageReactionRequest>) {
                return make_native_message_reaction(value);
            } else if constexpr (std::is_same_v<Request, TdPinMessageRequest>) {
                return make_native_pin_message(value);
            } else if constexpr (std::is_same_v<Request, TdViewMessagesRequest>) {
                return make_native_view_messages(value);
            } else if constexpr (std::is_same_v<Request, TdSetChatNotificationSettingsRequest>) {
                return make_native_set_chat_notification_settings(value);
            } else if constexpr (std::is_same_v<Request, TdToggleChatIsPinnedRequest>) {
                return make_native_toggle_chat_is_pinned(value);
            } else if constexpr (std::is_same_v<Request, TdAddChatToListRequest>) {
                return make_native_add_chat_to_list(value);
            } else if constexpr (std::is_same_v<Request, TdJoinChatRequest>) {
                return make_native_join_chat(value);
            } else {
                static_assert(std::is_same_v<Request, TdLeaveChatRequest>);
                return make_native_leave_chat(value);
            }
        },
        request);
}

TdValue convert_production_direct_response_for_test(TdFunctionKind function, TdValue object) {
    auto* native = object.get_if<NativeObjectPtr>();
    if (native == nullptr) {
        return TdValue::from(TdDirectConversionError{});
    }
    return convert_response_for(function, std::move(*native), 1);
}

TdValue convert_production_send_response_for_test(TdValue object, std::uint64_t client_generation) {
    auto* native = object.get_if<NativeObjectPtr>();
    if (native == nullptr) {
        return TdValue::from(TdDirectConversionError{});
    }
    return convert_response_for(TdFunctionKind::SendMessage, std::move(*native), client_generation);
}

TdValue convert_production_update_for_test(TdValue object, std::uint64_t client_generation) {
    auto* native = object.get_if<NativeObjectPtr>();
    if (native == nullptr) {
        return {};
    }
    return convert_response(std::move(*native), client_generation);
}

bool production_function_matches_for_test(const TdValue& function, TdFunctionKind kind) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    return native != nullptr && *native != nullptr && native_function_matches(**native, kind);
}

bool production_get_chat_history_matches_for_test(const TdValue& function, std::int64_t chat_id,
                                                  std::int64_t from_message_id, std::int32_t offset,
                                                  std::int32_t limit, bool only_local) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getChatHistory::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getChatHistory&>(**native);
    return request.chat_id_ == chat_id && request.from_message_id_ == from_message_id &&
           request.offset_ == offset && request.limit_ == limit &&
           request.only_local_ == only_local;
}

bool production_get_chat_message_by_date_matches_for_test(const TdValue& function,
                                                          std::int64_t chat_id, std::int32_t date) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getChatMessageByDate::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getChatMessageByDate&>(**native);
    return request.chat_id_ == chat_id && request.date_ == date;
}

bool production_get_message_thread_matches_for_test(const TdValue& function, std::int64_t chat_id,
                                                    std::int64_t message_id) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getMessageThread::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getMessageThread&>(**native);
    return request.chat_id_ == chat_id && request.message_id_ == message_id;
}

bool production_get_forum_topic_history_matches_for_test(const TdValue& function,
                                                         std::int64_t chat_id,
                                                         std::int32_t forum_topic_id,
                                                         std::int64_t from_message_id,
                                                         std::int32_t offset, std::int32_t limit) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getForumTopicHistory::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getForumTopicHistory&>(**native);
    return request.chat_id_ == chat_id && request.forum_topic_id_ == forum_topic_id &&
           request.from_message_id_ == from_message_id && request.offset_ == offset &&
           request.limit_ == limit;
}

bool production_get_message_thread_history_matches_for_test(
    const TdValue& function, std::int64_t chat_id, std::int64_t message_id,
    std::int64_t from_message_id, std::int32_t offset, std::int32_t limit) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getMessageThreadHistory::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getMessageThreadHistory&>(**native);
    return request.chat_id_ == chat_id && request.message_id_ == message_id &&
           request.from_message_id_ == from_message_id && request.offset_ == offset &&
           request.limit_ == limit;
}

bool production_get_direct_messages_chat_topic_history_matches_for_test(
    const TdValue& function, std::int64_t chat_id, std::int64_t topic_id,
    std::int64_t from_message_id, std::int32_t offset, std::int32_t limit) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getDirectMessagesChatTopicHistory::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getDirectMessagesChatTopicHistory&>(**native);
    return request.chat_id_ == chat_id && request.topic_id_ == topic_id &&
           request.from_message_id_ == from_message_id && request.offset_ == offset &&
           request.limit_ == limit;
}

bool production_get_saved_messages_topic_history_matches_for_test(const TdValue& function,
                                                                  std::int64_t topic_id,
                                                                  std::int64_t from_message_id,
                                                                  std::int32_t offset,
                                                                  std::int32_t limit) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getSavedMessagesTopicHistory::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getSavedMessagesTopicHistory&>(**native);
    return request.saved_messages_topic_id_ == topic_id &&
           request.from_message_id_ == from_message_id && request.offset_ == offset &&
           request.limit_ == limit;
}

std::optional<std::int64_t> production_terminate_session_id_for_test(const TdValue& function) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::terminateSession::ID) {
        return std::nullopt;
    }
    return static_cast<const td_api::terminateSession&>(**native).session_id_;
}

bool production_get_messages_matches_for_test(const TdValue& function, std::int64_t chat_id,
                                              const std::vector<std::int64_t>& message_ids) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr || (*native)->get_id() != td_api::getMessages::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getMessages&>(**native);
    return request.chat_id_ == chat_id && request.message_ids_ == message_ids;
}

bool production_get_message_link_matches_for_test(const TdValue& function, std::int64_t chat_id,
                                                  std::int64_t message_id,
                                                  std::int32_t media_timestamp,
                                                  std::int32_t checklist_task_id,
                                                  std::string_view poll_option_id, bool for_album,
                                                  bool in_message_thread) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getMessageLink::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getMessageLink&>(**native);
    return request.chat_id_ == chat_id && request.message_id_ == message_id &&
           request.media_timestamp_ == media_timestamp &&
           request.checklist_task_id_ == checklist_task_id &&
           request.poll_option_id_ == poll_option_id && request.for_album_ == for_album &&
           request.in_message_thread_ == in_message_thread;
}

bool production_get_message_matches_for_test(const TdValue& function, std::int64_t chat_id,
                                             std::int64_t message_id) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr || (*native)->get_id() != td_api::getMessage::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getMessage&>(**native);
    return request.chat_id_ == chat_id && request.message_id_ == message_id;
}

bool production_get_message_properties_matches_for_test(const TdValue& function,
                                                        std::int64_t chat_id,
                                                        std::int64_t message_id) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getMessageProperties::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getMessageProperties&>(**native);
    return request.chat_id_ == chat_id && request.message_id_ == message_id;
}

bool production_get_message_available_reactions_matches_for_test(const TdValue& function,
                                                                 std::int64_t chat_id,
                                                                 std::int64_t message_id) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::getMessageAvailableReactions::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::getMessageAvailableReactions&>(**native);
    return request.chat_id_ == chat_id && request.message_id_ == message_id &&
           request.row_size_ == 25;
}

bool production_get_unix_time_matches_for_test(const TdValue& function) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    return native != nullptr && *native != nullptr &&
           (*native)->get_id() == td_api::getOption::ID &&
           static_cast<const td_api::getOption&>(**native).name_ == "unix_time";
}

bool production_parse_text_entities_matches_for_test(const TdValue& function, std::string_view text,
                                                     TdTextParseMode mode) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::parseTextEntities::ID) {
        return false;
    }
    const auto& request = static_cast<const td_api::parseTextEntities&>(**native);
    if (request.text_ != text || request.parse_mode_ == nullptr) {
        return false;
    }
    if (mode == TdTextParseMode::Html) {
        return request.parse_mode_->get_id() == td_api::textParseModeHTML::ID;
    }
    return request.parse_mode_->get_id() == td_api::textParseModeMarkdown::ID &&
           static_cast<const td_api::textParseModeMarkdown&>(*request.parse_mode_).version_ == 2;
}

bool native_send_topic_matches(const td_api::MessageTopic* actual,
                               const std::optional<TdTopic>& expected) {
    if (!expected) {
        return actual == nullptr;
    }
    if (actual == nullptr) {
        return false;
    }
    if (expected->kind == TdTopicKind::Forum) {
        return actual->get_id() == td_api::messageTopicForum::ID &&
               static_cast<const td_api::messageTopicForum&>(*actual).forum_topic_id_ ==
                   expected->id;
    }
    return actual->get_id() == td_api::messageTopicSavedMessages::ID &&
           static_cast<const td_api::messageTopicSavedMessages&>(*actual)
                   .saved_messages_topic_id_ == expected->id;
}

bool native_send_reply_matches(const td_api::InputMessageReplyTo* actual,
                               const std::optional<std::int64_t>& expected) {
    if (!expected) {
        return actual == nullptr;
    }
    if (actual == nullptr || actual->get_id() != td_api::inputMessageReplyToMessage::ID) {
        return false;
    }
    const auto& reply = static_cast<const td_api::inputMessageReplyToMessage&>(*actual);
    return reply.message_id_ == *expected && reply.quote_ == nullptr &&
           reply.checklist_task_id_ == 0 && reply.poll_option_id_.empty();
}

bool native_send_schedule_matches(const td_api::MessageSchedulingState* actual,
                                  const TdSendSchedule& expected) {
    switch (expected.kind) {
    case TdSendScheduleKind::Immediate:
        return actual == nullptr;
    case TdSendScheduleKind::AtDate:
        return actual != nullptr &&
               actual->get_id() == td_api::messageSchedulingStateSendAtDate::ID &&
               static_cast<const td_api::messageSchedulingStateSendAtDate&>(*actual).send_date_ ==
                   expected.send_date &&
               static_cast<const td_api::messageSchedulingStateSendAtDate&>(*actual)
                       .repeat_period_ == 0;
    case TdSendScheduleKind::WhenOnline:
        return actual != nullptr &&
               actual->get_id() == td_api::messageSchedulingStateSendWhenOnline::ID;
    }
    return false;
}

bool production_send_message_matches_for_test(const TdValue& function,
                                              const TdSendMessageRequest& expected) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr || (*native)->get_id() != td_api::sendMessage::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::sendMessage&>(**native);
    if (actual.chat_id_ != expected.chat_id ||
        !native_send_topic_matches(actual.topic_id_.get(), expected.topic) ||
        !native_send_reply_matches(actual.reply_to_.get(), expected.reply_to_message_id) ||
        actual.options_ == nullptr || actual.reply_markup_ != nullptr ||
        actual.input_message_content_ == nullptr ||
        actual.input_message_content_->get_id() != td_api::inputMessageText::ID) {
        return false;
    }
    const auto& options = *actual.options_;
    if (options.suggested_post_info_ != nullptr ||
        options.disable_notification_ != expected.options.disable_notification ||
        options.from_background_ || options.protect_content_ || options.allow_paid_broadcast_ ||
        options.paid_message_star_count_ != 0 || options.update_order_of_installed_sticker_sets_ ||
        !native_send_schedule_matches(options.scheduling_state_.get(), expected.options.schedule) ||
        options.effect_id_ != 0 || options.sending_id_ != expected.options.sending_id ||
        options.only_preview_) {
        return false;
    }
    const auto& content =
        static_cast<const td_api::inputMessageText&>(*actual.input_message_content_);
    if (content.text_ == nullptr || content.link_preview_options_ != nullptr ||
        content.clear_draft_) {
        return false;
    }
    const auto neutral = neutral_formatted_text(*content.text_);
    return neutral && *neutral == expected.content.formatted_text;
}

bool native_direct_chat_list_matches(const td_api::ChatList* list, TdDirectChatList expected) {
    return list != nullptr &&
           ((expected == TdDirectChatList::Main && list->get_id() == td_api::chatListMain::ID) ||
            (expected == TdDirectChatList::Archive &&
             list->get_id() == td_api::chatListArchive::ID));
}

bool native_emoji_reaction_matches(const td_api::ReactionType* reaction,
                                   std::string_view expected) {
    return reaction != nullptr && reaction->get_id() == td_api::reactionTypeEmoji::ID &&
           static_cast<const td_api::reactionTypeEmoji&>(*reaction).emoji_ == expected;
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdEditMessageTextRequest& expected) {
    if (function.get_id() != td_api::editMessageText::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::editMessageText&>(function);
    if (actual.chat_id_ != expected.chat_id || actual.message_id_ != expected.message_id ||
        actual.reply_markup_ != nullptr || actual.input_message_content_ == nullptr ||
        actual.input_message_content_->get_id() != td_api::inputMessageText::ID) {
        return false;
    }
    const auto& content =
        static_cast<const td_api::inputMessageText&>(*actual.input_message_content_);
    return content.text_ != nullptr && content.text_->text_ == expected.text &&
           content.text_->entities_.empty() && content.link_preview_options_ == nullptr &&
           !content.clear_draft_;
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdDeleteMessagesRequest& expected) {
    if (function.get_id() != td_api::deleteMessages::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::deleteMessages&>(function);
    return actual.chat_id_ == expected.chat_id && actual.message_ids_ == expected.message_ids &&
           actual.revoke_ == expected.revoke;
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdMessageReactionRequest& expected) {
    if (expected.remove) {
        if (function.get_id() != td_api::removeMessageReaction::ID) {
            return false;
        }
        const auto& actual = static_cast<const td_api::removeMessageReaction&>(function);
        return actual.chat_id_ == expected.chat_id && actual.message_id_ == expected.message_id &&
               native_emoji_reaction_matches(actual.reaction_type_.get(), expected.reaction);
    }
    if (function.get_id() != td_api::addMessageReaction::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::addMessageReaction&>(function);
    return actual.chat_id_ == expected.chat_id && actual.message_id_ == expected.message_id &&
           native_emoji_reaction_matches(actual.reaction_type_.get(), expected.reaction) &&
           actual.is_big_ == expected.big && actual.update_recent_reactions_;
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdPinMessageRequest& expected) {
    if (expected.pinned) {
        if (function.get_id() != td_api::pinChatMessage::ID) {
            return false;
        }
        const auto& actual = static_cast<const td_api::pinChatMessage&>(function);
        return actual.chat_id_ == expected.chat_id && actual.message_id_ == expected.message_id &&
               !actual.disable_notification_ && !actual.only_for_self_;
    }
    if (function.get_id() != td_api::unpinChatMessage::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::unpinChatMessage&>(function);
    return actual.chat_id_ == expected.chat_id && actual.message_id_ == expected.message_id;
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdViewMessagesRequest& expected) {
    if (function.get_id() != td_api::viewMessages::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::viewMessages&>(function);
    return actual.chat_id_ == expected.chat_id && actual.message_ids_ == expected.message_ids &&
           actual.source_ == nullptr && actual.force_read_;
}

bool native_notification_settings_match(const td_api::chatNotificationSettings& actual,
                                        const TdChatNotificationSettings& expected) {
    return actual.use_default_mute_for_ == expected.use_default_mute_for &&
           actual.mute_for_ == expected.mute_for &&
           actual.use_default_sound_ == expected.use_default_sound &&
           actual.sound_id_ == expected.sound_id &&
           actual.use_default_show_preview_ == expected.use_default_show_preview &&
           actual.show_preview_ == expected.show_preview &&
           actual.use_default_mute_stories_ == expected.use_default_mute_stories &&
           actual.mute_stories_ == expected.mute_stories &&
           actual.use_default_story_sound_ == expected.use_default_story_sound &&
           actual.story_sound_id_ == expected.story_sound_id &&
           actual.use_default_show_story_poster_ == expected.use_default_show_story_poster &&
           actual.show_story_poster_ == expected.show_story_poster &&
           actual.use_default_disable_pinned_message_notifications_ ==
               expected.use_default_disable_pinned_message_notifications &&
           actual.disable_pinned_message_notifications_ ==
               expected.disable_pinned_message_notifications &&
           actual.use_default_disable_mention_notifications_ ==
               expected.use_default_disable_mention_notifications &&
           actual.disable_mention_notifications_ == expected.disable_mention_notifications;
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdSetChatNotificationSettingsRequest& expected) {
    if (function.get_id() != td_api::setChatNotificationSettings::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::setChatNotificationSettings&>(function);
    return actual.chat_id_ == expected.chat_id && actual.notification_settings_ != nullptr &&
           native_notification_settings_match(*actual.notification_settings_, expected.settings);
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdToggleChatIsPinnedRequest& expected) {
    if (function.get_id() != td_api::toggleChatIsPinned::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::toggleChatIsPinned&>(function);
    return native_direct_chat_list_matches(actual.chat_list_.get(), expected.list) &&
           actual.chat_id_ == expected.chat_id && actual.is_pinned_ == expected.pinned;
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdAddChatToListRequest& expected) {
    if (function.get_id() != td_api::addChatToList::ID) {
        return false;
    }
    const auto& actual = static_cast<const td_api::addChatToList&>(function);
    return actual.chat_id_ == expected.chat_id &&
           native_direct_chat_list_matches(actual.chat_list_.get(), expected.list);
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdJoinChatRequest& expected) {
    if (expected.chat_id.has_value()) {
        return function.get_id() == td_api::joinChat::ID &&
               static_cast<const td_api::joinChat&>(function).chat_id_ ==
                   expected.chat_id.value_or(0);
    }
    return function.get_id() == td_api::joinChatByInviteLink::ID &&
           static_cast<const td_api::joinChatByInviteLink&>(function).invite_link_ ==
               expected.invite_link.value_or(std::string{});
}

bool native_direct_request_matches(const td_api::Function& function,
                                   const TdLeaveChatRequest& expected) {
    return function.get_id() == td_api::leaveChat::ID &&
           static_cast<const td_api::leaveChat&>(function).chat_id_ == expected.chat_id;
}

bool production_direct_request_matches_for_test(const TdValue& function,
                                                const TdDirectRequest& request) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr) {
        return false;
    }
    return std::visit(
        [&](const auto& expected) { return native_direct_request_matches(**native, expected); },
        request);
}

std::optional<AuthStateData>
convert_production_authorization_state_for_test(const TdValue& object,
                                                bool authorization_state_response) {
    const auto* native = object.get_if<NativeObjectPtr>();
    if (native == nullptr || *native == nullptr) {
        return std::nullopt;
    }
    return extract_auth_state(**native, authorization_state_response);
}

std::string_view process_log_failure_message_for_test(bool json) {
    return process_log_failure_message(json);
}

void reset_process_log_failure_for_test(bool json) {
    process_log_state().failure_reported.store(false, std::memory_order_relaxed);
    process_log_state().json_diagnostics.store(json, std::memory_order_relaxed);
}

void report_process_log_failure_for_test() {
    report_process_log_failure();
}

void report_process_log_failure_with_writer_for_test(ProcessLogWriteFunction writer,
                                                     void* context) {
    report_process_log_failure(writer, context);
}

} // namespace detail

TdlibParameters::~TdlibParameters() {
    wipe(database_encryption_key);
    wipe(api_hash);
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
TdlibParameters::TdlibParameters(TdlibParameters&& other)
    : use_test_dc(other.use_test_dc), database_directory(std::move(other.database_directory)),
      files_directory(std::move(other.files_directory)), use_file_database(other.use_file_database),
      use_chat_info_database(other.use_chat_info_database),
      use_message_database(other.use_message_database), use_secret_chats(other.use_secret_chats),
      api_id(other.api_id), system_language_code(std::move(other.system_language_code)),
      device_model(std::move(other.device_model)), system_version(std::move(other.system_version)),
      application_version(std::move(other.application_version)) {
    transfer(other.database_encryption_key, database_encryption_key);
    transfer(other.api_hash, api_hash);
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
TdlibParameters& TdlibParameters::operator=(TdlibParameters&& other) {
    if (this != &other) {
        wipe(database_encryption_key);
        wipe(api_hash);
        use_test_dc = other.use_test_dc;
        database_directory = std::move(other.database_directory);
        files_directory = std::move(other.files_directory);
        use_file_database = other.use_file_database;
        use_chat_info_database = other.use_chat_info_database;
        use_message_database = other.use_message_database;
        use_secret_chats = other.use_secret_chats;
        api_id = other.api_id;
        system_language_code = std::move(other.system_language_code);
        device_model = std::move(other.device_model);
        system_version = std::move(other.system_version);
        application_version = std::move(other.application_version);
        transfer(other.database_encryption_key, database_encryption_key);
        transfer(other.api_hash, api_hash);
    }
    return *this;
}

TdAuthRequest::TdAuthRequest(TdFunctionKind function_value) : function(function_value) {}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
TdAuthRequest::TdAuthRequest(TdFunctionKind function_value, std::string&& value_value)
    : function(function_value) {
    transfer(value_value, value);
}

// NOLINTBEGIN(cppcoreguidelines-rvalue-reference-param-not-moved)
TdAuthRequest::TdAuthRequest(TdFunctionKind function_value, std::string&& value_value,
                             std::string&& secondary_value)
    : function(function_value) {
    transfer(value_value, value);
    transfer(secondary_value, secondary);
}
// NOLINTEND(cppcoreguidelines-rvalue-reference-param-not-moved)

TdAuthRequest::~TdAuthRequest() {
    wipe(value);
    wipe(secondary);
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
TdAuthRequest::TdAuthRequest(TdAuthRequest&& other) : function(other.function) {
    transfer(other.value, value);
    transfer(other.secondary, secondary);
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
TdAuthRequest& TdAuthRequest::operator=(TdAuthRequest&& other) {
    if (this != &other) {
        wipe(value);
        wipe(secondary);
        function = other.function;
        transfer(other.value, value);
        transfer(other.secondary, secondary);
    }
    return *this;
}

TdFunctionData describe_tdlib_parameters(const TdlibParameters& parameters) {
    return TdFunctionData{TdFunctionKind::SetTdlibParameters,
                          {
                              {"use_test_dc", parameters.use_test_dc},
                              {"database_directory", parameters.database_directory},
                              {"files_directory", parameters.files_directory},
                              {"database_encryption_key", TdRedactedValue::Credential},
                              {"use_file_database", parameters.use_file_database},
                              {"use_chat_info_database", parameters.use_chat_info_database},
                              {"use_message_database", parameters.use_message_database},
                              {"use_secret_chats", parameters.use_secret_chats},
                              {"api_id", static_cast<std::int64_t>(parameters.api_id)},
                              {"api_hash", TdRedactedValue::Credential},
                              {"system_language_code", parameters.system_language_code},
                              {"device_model", parameters.device_model},
                              {"system_version", parameters.system_version},
                              {"application_version", parameters.application_version},
                          }};
}

std::unique_ptr<TdRuntime> make_production_td_runtime() {
    return std::make_unique<ProductionTdRuntime>();
}

std::string production_tdlib_version() {
    enforce_error_verbosity();
    auto value = td::ClientManager::execute(td_api::make_object<td_api::getOption>("version"));
    if (value != nullptr && value->get_id() == td_api::optionValueString::ID) {
        return static_cast<td_api::optionValueString&>(*value).value_;
    }
    return "unknown";
}

} // namespace tgcli::core
