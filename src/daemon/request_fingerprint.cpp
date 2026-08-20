#include "daemon/request_fingerprint.hpp"

#include "common/canonical_json.hpp"
#include "common/paths.hpp"
#include "common/sha256.hpp"
#include "common/utf8.hpp"
#include "daemon/local_selector.hpp"
#include "daemon/resolver.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;
constexpr std::size_t kMaximumMessageScalars = 4'096;
constexpr std::size_t kMaximumCaptionScalars = 1'024;
constexpr std::size_t kMaximumCaptionBytes = 4'096;

bool valid_positive_int53(std::int64_t value) {
    return value > 0 && value <= kMaximumInt53;
}

bool valid_text(std::string_view value, std::size_t maximum_scalars, bool allow_empty,
                std::optional<std::size_t> maximum_bytes = std::nullopt) {
    if ((!allow_empty && value.empty()) || value.find('\0') != std::string_view::npos ||
        !common::valid_utf8(value) || (maximum_bytes && value.size() > *maximum_bytes)) {
        return false;
    }
    const auto scalars = static_cast<std::size_t>(std::ranges::count_if(value, [](char character) {
        return (static_cast<unsigned char>(character) & 0xc0U) != 0x80U;
    }));
    return scalars <= maximum_scalars;
}

bool valid_hash(std::string_view value) {
    constexpr std::string_view prefix = "sha256:";
    return value.size() == prefix.size() + 64 && value.starts_with(prefix) &&
           std::ranges::all_of(value.substr(prefix.size()), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool valid_message_ids(const std::vector<std::int64_t>& values) {
    if (values.empty() || values.size() > 100) {
        return false;
    }
    std::int64_t previous = 0;
    return std::ranges::all_of(values, [&previous](std::int64_t value) {
        if (!valid_positive_int53(value) || value <= previous) {
            return false;
        }
        previous = value;
        return true;
    });
}

bool valid_basename(std::string_view value) {
    if (value.empty() || value.size() > 255 || value == "." || value == ".." ||
        value.find('/') != std::string_view::npos || !common::valid_utf8(value)) {
        return false;
    }
    for (std::size_t offset = 0; offset < value.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        if (byte <= 0x1fU || byte == 0x7fU ||
            (byte == 0xc2U && offset + 1 < value.size() &&
             static_cast<unsigned char>(value[offset + 1]) >= 0x80U &&
             static_cast<unsigned char>(value[offset + 1]) <= 0x9fU)) {
            return false;
        }
    }
    return true;
}

std::optional<json> selector_json(std::string_view selector) {
    auto canonical = canonical_write_selector(selector);
    if (!canonical) {
        return std::nullopt;
    }
    return json(std::move(*canonical));
}

std::optional<json> topic_json(const std::optional<TopicRef>& topic) {
    if (!topic) {
        return json(nullptr);
    }
    if (topic->kind != TopicKind::Forum || topic->id <= 0 ||
        topic->id > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return json{{"kind", "forum"}, {"id", topic->id}};
}

std::optional<json> schedule_json(const std::optional<FingerprintSchedule>& schedule) {
    if (!schedule) {
        return json(nullptr);
    }
    if (std::holds_alternative<FingerprintScheduleOnline>(*schedule)) {
        return json{{"kind", "online"}};
    }
    const auto send_date = std::get<FingerprintScheduleAt>(*schedule).send_date;
    if (send_date <= 0) {
        return std::nullopt;
    }
    return json{{"kind", "at"}, {"send_date", send_date}};
}

std::optional<std::string_view> parse_mode_name(FingerprintParseMode mode) {
    switch (mode) {
    case FingerprintParseMode::Plain:
        return "plain";
    case FingerprintParseMode::MarkdownV2:
        return "markdown_v2";
    case FingerprintParseMode::Html:
        return "html";
    }
    return std::nullopt;
}

std::optional<json> make_payload(const SendFingerprintPayload& value) {
    const auto selector = selector_json(value.chat_selector);
    const auto mode = parse_mode_name(value.parse_mode);
    const auto topic = topic_json(value.requested_topic);
    const auto schedule = schedule_json(value.schedule);
    if (!selector || !mode || !topic || !schedule ||
        !valid_text(value.text, kMaximumMessageScalars, false) ||
        (value.reply_to && !valid_positive_int53(*value.reply_to))) {
        return std::nullopt;
    }
    return json{{"chat_selector", *selector},
                {"text", value.text},
                {"parse_mode", *mode},
                {"reply_to", value.reply_to ? json(*value.reply_to) : json(nullptr)},
                {"requested_topic", *topic},
                {"silent", value.silent},
                {"schedule", *schedule}};
}

std::optional<json> make_payload(const MsgEditFingerprintPayload& value) {
    const auto selector = selector_json(value.chat_selector);
    if (!selector || !valid_positive_int53(value.message_id) ||
        !valid_text(value.text, kMaximumMessageScalars, false)) {
        return std::nullopt;
    }
    return json{
        {"chat_selector", *selector}, {"message_id", value.message_id}, {"text", value.text}};
}

std::optional<json> make_payload(const MsgDeleteFingerprintPayload& value) {
    const auto selector = selector_json(value.chat_selector);
    if (!selector || !valid_message_ids(value.message_ids)) {
        return std::nullopt;
    }
    return json{{"chat_selector", *selector},
                {"message_ids", value.message_ids},
                {"for_all", value.for_all}};
}

std::optional<json> make_payload(const MsgForwardFingerprintPayload& value) {
    const auto from = selector_json(value.from_selector);
    const auto to = selector_json(value.to_selector);
    if (!from || !to || !valid_message_ids(value.message_ids)) {
        return std::nullopt;
    }
    return json{{"from_selector", *from},
                {"to_selector", *to},
                {"message_ids", value.message_ids},
                {"drop_author", value.drop_author}};
}

std::optional<json> make_payload(const MsgReactFingerprintPayload& value) {
    const auto selector = selector_json(value.chat_selector);
    if (!selector || !valid_positive_int53(value.message_id) ||
        !valid_text(value.reaction, 64, false, 64)) {
        return std::nullopt;
    }
    return json{{"chat_selector", *selector},
                {"message_id", value.message_id},
                {"reaction", value.reaction},
                {"remove", value.remove},
                {"big", value.big}};
}

template <typename T> std::optional<json> message_target_payload(const T& value) {
    const auto selector = selector_json(value.chat_selector);
    if (!selector || !valid_positive_int53(value.message_id)) {
        return std::nullopt;
    }
    return json{{"chat_selector", *selector}, {"message_id", value.message_id}};
}

template <typename T> std::optional<json> chat_target_payload(const T& value) {
    const auto selector = selector_json(value.chat_selector);
    if (!selector) {
        return std::nullopt;
    }
    return json{{"chat_selector", *selector}};
}

std::optional<json> make_payload(const MsgPinFingerprintPayload& value) {
    return message_target_payload(value);
}

std::optional<json> make_payload(const MsgUnpinFingerprintPayload& value) {
    return message_target_payload(value);
}

std::optional<json> make_payload(const ChatMarkReadFingerprintPayload& value) {
    return chat_target_payload(value);
}

std::optional<json> make_payload(const ChatMuteFingerprintPayload& value) {
    const auto selector = selector_json(value.chat_selector);
    if (!selector || value.duration_seconds < 1 || value.duration_seconds > 31'622'400) {
        return std::nullopt;
    }
    return json{{"chat_selector", *selector}, {"duration_seconds", value.duration_seconds}};
}

std::optional<json> make_payload(const ChatUnmuteFingerprintPayload& value) {
    const auto selector = selector_json(value.chat_selector);
    if (!selector || value.duration_seconds != 0) {
        return std::nullopt;
    }
    return json{{"chat_selector", *selector}, {"duration_seconds", value.duration_seconds}};
}

std::optional<json> make_payload(const ChatPinFingerprintPayload& value) {
    return chat_target_payload(value);
}

std::optional<json> make_payload(const ChatUnpinFingerprintPayload& value) {
    return chat_target_payload(value);
}

std::optional<json> make_payload(const ChatArchiveFingerprintPayload& value) {
    return chat_target_payload(value);
}

std::optional<json> make_payload(const ChatUnarchiveFingerprintPayload& value) {
    return chat_target_payload(value);
}

std::optional<json> make_payload(const ChatJoinFingerprintPayload& value) {
    if (const auto* username = std::get_if<ChatJoinUsernameFingerprint>(&value.target)) {
        const auto classified = classify_local_selector(username->username);
        if (!classified || classified->kind != LocalSelectorKind::Username ||
            username->username.find('\0') != std::string::npos) {
            return std::nullopt;
        }
        return json{{"source", "username"}, {"username", username->username}};
    }
    const auto& invite = std::get<ChatJoinInviteFingerprint>(value.target);
    if (!valid_hash(invite.invite_link_sha256)) {
        return std::nullopt;
    }
    return json{{"source", "invite_link"}, {"invite_link_sha256", invite.invite_link_sha256}};
}

std::optional<json> make_payload(const ChatLeaveFingerprintPayload& value) {
    return chat_target_payload(value);
}

std::optional<json> make_payload(const SavedAttachFingerprintPayload& value) {
    if (!valid_positive_int53(value.message_id) || !valid_basename(value.name) ||
        !valid_hash(value.sha256) ||
        !valid_text(value.caption, kMaximumCaptionScalars, true, kMaximumCaptionBytes)) {
        return std::nullopt;
    }
    return json{{"message_id", value.message_id},
                {"topic", "inherit_saved"},
                {"name", value.name},
                {"size", value.size},
                {"sha256", value.sha256},
                {"caption", value.caption}};
}

constexpr proto::M3Operation operation_for([[maybe_unused]] const SendFingerprintPayload& value) {
    return proto::M3Operation::Send;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const MsgEditFingerprintPayload& value) {
    return proto::M3Operation::MsgEdit;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const MsgDeleteFingerprintPayload& value) {
    return proto::M3Operation::MsgDelete;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const MsgForwardFingerprintPayload& value) {
    return proto::M3Operation::MsgForward;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const MsgReactFingerprintPayload& value) {
    return proto::M3Operation::MsgReact;
}

constexpr proto::M3Operation operation_for([[maybe_unused]] const MsgPinFingerprintPayload& value) {
    return proto::M3Operation::MsgPin;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const MsgUnpinFingerprintPayload& value) {
    return proto::M3Operation::MsgUnpin;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatMarkReadFingerprintPayload& value) {
    return proto::M3Operation::ChatMarkRead;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatMuteFingerprintPayload& value) {
    return proto::M3Operation::ChatMute;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatUnmuteFingerprintPayload& value) {
    return proto::M3Operation::ChatUnmute;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatPinFingerprintPayload& value) {
    return proto::M3Operation::ChatPin;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatUnpinFingerprintPayload& value) {
    return proto::M3Operation::ChatUnpin;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatArchiveFingerprintPayload& value) {
    return proto::M3Operation::ChatArchive;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatUnarchiveFingerprintPayload& value) {
    return proto::M3Operation::ChatUnarchive;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatJoinFingerprintPayload& value) {
    return proto::M3Operation::ChatJoin;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const ChatLeaveFingerprintPayload& value) {
    return proto::M3Operation::ChatLeave;
}

constexpr proto::M3Operation
operation_for([[maybe_unused]] const SavedAttachFingerprintPayload& value) {
    return proto::M3Operation::SavedAttach;
}

} // namespace

proto::M3Operation fingerprint_operation(const FingerprintPayload& payload) {
    return std::visit([](const auto& value) { return operation_for(value); }, payload);
}

std::optional<std::string> canonical_write_selector(std::string_view selector) {
    if (selector.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto classified = classify_local_selector(selector);
    if (!classified) {
        return std::nullopt;
    }
    switch (classified->kind) {
    case LocalSelectorKind::Numeric:
        return std::to_string(classified->chat_id);
    case LocalSelectorKind::Username:
    case LocalSelectorKind::PublicChatLink:
    case LocalSelectorKind::BotStartLink:
    case LocalSelectorKind::MessageLink:
    case LocalSelectorKind::DirectMessagesChatLink:
        return std::string(selector);
    case LocalSelectorKind::ChatInviteLink:
        return invite_link_hash(selector);
    case LocalSelectorKind::InvalidLink:
    case LocalSelectorKind::UnsupportedLink:
    case LocalSelectorKind::Title:
        return std::nullopt;
    }
    return std::nullopt;
}

std::string idempotency_key_hash(std::string_view raw_key) {
    return common::domain_separated_sha256("tgcli-idempotency-key-v1", raw_key);
}

std::string invite_link_hash(std::string_view raw_invite) {
    return common::domain_separated_sha256("tgcli-invite-link-v1", raw_invite);
}

RequestFingerprintResult request_fingerprint(std::string_view account,
                                             const ResolverPrincipal& principal,
                                             const FingerprintPayload& payload) {
    const std::string account_value(account);
    if (!paths::valid_account_name(account_value)) {
        return FingerprintError::InvalidAccount;
    }
    if (!valid_positive_int53(principal.id)) {
        return FingerprintError::InvalidPrincipal;
    }
    const auto operation = fingerprint_operation(payload);
    const auto* identity = proto::m3_operation_identity(operation);
    if (identity == nullptr) {
        return FingerprintError::InvalidPayload;
    }
    const auto normalized_payload =
        std::visit([](const auto& value) { return make_payload(value); }, payload);
    if (!normalized_payload) {
        return FingerprintError::InvalidPayload;
    }
    const json root{{"version", 1},
                    {"account", account_value},
                    {"principal", {{"id", principal.id}, {"is_bot", principal.is_bot}}},
                    {"operation", identity->canonical_name},
                    {"payload", *normalized_payload}};
    auto canonical = common::canonical_json(root);
    if (const auto* error = std::get_if<common::CanonicalJsonError>(&canonical)) {
        static_cast<void>(error);
        return FingerprintError::CanonicalJson;
    }
    return common::domain_separated_sha256("tgcli-idempotency-request-v1",
                                           std::get<std::string>(canonical));
}

} // namespace tgcli::daemon
