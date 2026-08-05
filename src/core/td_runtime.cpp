#include "core/td_runtime.hpp"

#include "core/td_runtime_test_adapter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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

TdValue session_conversion_error(std::optional<std::int32_t> tdlib_type_id = std::nullopt) {
    return TdValue::from(TdSessionConversionError{tdlib_type_id});
}

TdValue convert_sessions(td_api::object_ptr<td_api::sessions> sessions) {
    if (sessions == nullptr || sessions->inactive_session_ttl_days_ < 1 ||
        sessions->inactive_session_ttl_days_ > 366) {
        return session_conversion_error();
    }

    TdSessions converted;
    converted.inactive_session_ttl_days = sessions->inactive_session_ttl_days_;
    converted.items.reserve(sessions->sessions_.size());
    std::unordered_set<std::int64_t> ids;
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
        if (!valid_utf8(item->application_name_) || !valid_utf8(item->application_version_) ||
            !valid_utf8(item->device_model_) || !valid_utf8(item->platform_) ||
            !valid_utf8(item->system_version_) || !valid_utf8(item->ip_address_) ||
            !valid_utf8(item->location_)) {
            return session_conversion_error();
        }
        converted.items.push_back({.id = std::to_string(item->id_),
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

td_api::object_ptr<td_api::ChatList> make_chat_list(TdChatListKind list) {
    switch (list) {
    case TdChatListKind::Main:
        return td_api::make_object<td_api::chatListMain>();
    case TdChatListKind::Archive:
        return td_api::make_object<td_api::chatListArchive>();
    }
    throw std::invalid_argument("unsupported chat list");
}

std::string_view chat_list_name(TdChatListKind list) {
    switch (list) {
    case TdChatListKind::Main:
        return "main";
    case TdChatListKind::Archive:
        return "archive";
    }
    return "unknown";
}

TdChat convert_chat(td_api::chat& chat) {
    TdChat converted{.id = chat.id_,
                     .title = std::move(chat.title_),
                     .kind = TdChatKind::Unknown,
                     .related_id = 0,
                     .tdlib_type_id = chat.type_ == nullptr ? 0 : chat.type_->get_id()};
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

TdValue convert_response(NativeObjectPtr object) {
    if (object == nullptr) {
        return {};
    }
    switch (object->get_id()) {
    case td_api::ok::ID:
        return TdValue::from(TdOk{});
    case td_api::error::ID: {
        auto& error = static_cast<td_api::error&>(*object);
        return TdValue::from(TdError{error.code_, std::move(error.message_)});
    }
    case td_api::user::ID: {
        auto& user = static_cast<td_api::user&>(*object);
        TdUserSummary summary{.id = user.id_,
                              .first_name = std::move(user.first_name_),
                              .last_name = std::move(user.last_name_),
                              .usernames = {},
                              .phone_number = std::move(user.phone_number_),
                              .is_bot = user.type_ != nullptr &&
                                        user.type_->get_id() == td_api::userTypeBot::ID,
                              .is_premium = user.is_premium_};
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
    case TdFunctionKind::LogOut:
        return function.get_id() == td_api::logOut::ID;
    case TdFunctionKind::Close:
        return function.get_id() == td_api::close::ID;
    }
    return false;
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

    TdValue make_get_chats(TdChatListKind list, std::int32_t limit) override {
        NativeFunctionPtr native =
            td_api::make_object<td_api::getChats>(make_chat_list(list), limit);
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::GetChats,
                                                {{"list", std::string(chat_list_name(list))},
                                                 {"limit", static_cast<std::int64_t>(limit)}}});
    }

    TdValue make_load_chats(TdChatListKind list, std::int32_t limit) override {
        NativeFunctionPtr native =
            td_api::make_object<td_api::loadChats>(make_chat_list(list), limit);
        return TdValue::function(std::move(native),
                                 TdFunctionData{TdFunctionKind::LoadChats,
                                                {{"list", std::string(chat_list_name(list))},
                                                 {"limit", static_cast<std::int64_t>(limit)}}});
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
        if (is_authorization_state_query) {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            authorization_queries_[client_id].insert(query_id);
        }
        manager_->send(client_id, query_id, std::move(*native_function));
    }

    std::optional<TdRuntimeEvent> receive(std::chrono::milliseconds timeout) override {
        auto response = manager_->receive(static_cast<double>(timeout.count()) / 1000.0);
        if (response.object == nullptr) {
            return std::nullopt;
        }

        std::uint64_t generation = 0;
        bool authorization_state_response = false;
        {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            const auto it = generations_.find(response.client_id);
            if (it == generations_.end()) {
                return std::nullopt;
            }
            generation = it->second;
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
        }
        return TdRuntimeEvent{.client_id = response.client_id,
                              .client_generation = generation,
                              .query_id = response.request_id,
                              .object = convert_response(std::move(response.object)),
                              .authorization_state = std::move(authorization_state)};
    }

  private:
    std::unique_ptr<td::ClientManager> manager_;
    std::shared_ptr<TdLogSink> log_sink_;
    std::mutex generations_mutex_;
    std::unordered_map<std::int32_t, std::uint64_t> generations_;
    std::unordered_map<std::int32_t, std::unordered_set<std::uint64_t>> authorization_queries_;
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

bool production_function_matches_for_test(const TdValue& function, TdFunctionKind kind) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    return native != nullptr && *native != nullptr && native_function_matches(**native, kind);
}

std::optional<std::int64_t> production_terminate_session_id_for_test(const TdValue& function) {
    const auto* native = function.get_if<NativeFunctionPtr>();
    if (native == nullptr || *native == nullptr ||
        (*native)->get_id() != td_api::terminateSession::ID) {
        return std::nullopt;
    }
    return static_cast<const td_api::terminateSession&>(**native).session_id_;
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
