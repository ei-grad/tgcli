#include "daemon/message_summary.hpp"

#include "common/utf8.hpp"
#include "daemon/account_audit_limits.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <limits>
#include <string_view>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_user_id(std::int64_t value) {
    return value > 0 && value <= kMaximumInt53;
}

std::optional<std::string> timestamp(std::int32_t seconds) {
    if (seconds == 0) {
        return std::nullopt;
    }
    const std::time_t value = seconds;
    std::tm utc{};
    if (gmtime_r(&value, &utc) == nullptr) {
        return std::nullopt;
    }
    std::array<char, 21> rendered{};
    if (std::strftime(rendered.data(), rendered.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return std::nullopt;
    }
    return std::string(rendered.data());
}

std::string_view topic_kind_name(TopicKind kind) {
    switch (kind) {
    case TopicKind::Forum:
        return "forum";
    case TopicKind::Thread:
        return "thread";
    case TopicKind::Direct:
        return "direct";
    case TopicKind::Saved:
        return "saved";
    }
    return "forum";
}

std::string_view sender_kind_name(MessageSenderKind kind) {
    switch (kind) {
    case MessageSenderKind::User:
        return "user";
    case MessageSenderKind::Chat:
        return "chat";
    }
    return "user";
}

std::string_view content_kind_name(MessageContentKind kind) {
    switch (kind) {
    case MessageContentKind::Text:
        return "text";
    case MessageContentKind::Photo:
        return "photo";
    case MessageContentKind::Video:
        return "video";
    case MessageContentKind::Document:
        return "doc";
    case MessageContentKind::Voice:
        return "voice";
    case MessageContentKind::Other:
        return "other";
    }
    return "other";
}

} // namespace

std::optional<TopicRef> materialize_topic_ref(const core::TdTopic& topic) {
    TopicKind kind = TopicKind::Forum;
    switch (topic.kind) {
    case core::TdTopicKind::Forum:
        if (topic.id <= 0 || topic.id > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        kind = TopicKind::Forum;
        break;
    case core::TdTopicKind::Thread:
        kind = TopicKind::Thread;
        break;
    case core::TdTopicKind::Direct:
        kind = TopicKind::Direct;
        break;
    case core::TdTopicKind::Saved:
        kind = TopicKind::Saved;
        break;
    case core::TdTopicKind::Unknown:
        return std::nullopt;
    }
    if (!valid_user_id(topic.id)) {
        return std::nullopt;
    }
    return TopicRef{.kind = kind, .id = topic.id};
}

std::optional<MessageSummary> materialize_message_summary(const core::TdMessageSummary& message) {
    if (!valid_int53(message.id) || !valid_int53(message.chat_id) ||
        !common::valid_utf8(message.text)) {
        return std::nullopt;
    }
    MessageSenderRef sender;
    switch (message.sender.kind) {
    case core::TdMessageSenderKind::User:
        if (!valid_user_id(message.sender.id)) {
            return std::nullopt;
        }
        sender = {.kind = MessageSenderKind::User, .id = message.sender.id};
        break;
    case core::TdMessageSenderKind::Chat:
        if (!valid_int53(message.sender.id)) {
            return std::nullopt;
        }
        sender = {.kind = MessageSenderKind::Chat, .id = message.sender.id};
        break;
    case core::TdMessageSenderKind::Unknown:
        return std::nullopt;
    }
    std::optional<TopicRef> topic;
    if (message.topic) {
        topic = materialize_topic_ref(*message.topic);
        if (!topic) {
            return std::nullopt;
        }
    }
    std::optional<std::string> date;
    if (message.date != 0) {
        date = timestamp(message.date);
        if (!date) {
            return std::nullopt;
        }
    }
    MessageContentKind type = MessageContentKind::Other;
    switch (message.content_kind) {
    case core::TdMessageContentKind::Text:
        type = MessageContentKind::Text;
        break;
    case core::TdMessageContentKind::Photo:
        type = MessageContentKind::Photo;
        break;
    case core::TdMessageContentKind::Video:
        type = MessageContentKind::Video;
        break;
    case core::TdMessageContentKind::Document:
        type = MessageContentKind::Document;
        break;
    case core::TdMessageContentKind::Voice:
        type = MessageContentKind::Voice;
        break;
    case core::TdMessageContentKind::Other:
        type = MessageContentKind::Other;
        break;
    }
    return MessageSummary{.id = message.id,
                          .chat_id = message.chat_id,
                          .date = std::move(date),
                          .sender = sender,
                          .is_outgoing = message.is_outgoing,
                          .topic = topic,
                          .type = type,
                          .text = message.text};
}

nlohmann::json topic_ref_json(const TopicRef& topic) {
    return {{"kind", topic_kind_name(topic.kind)}, {"id", topic.id}};
}

nlohmann::json message_summary_json(const MessageSummary& message) {
    return {
        {"id", message.id},
        {"chat_id", message.chat_id},
        {"date", message.date ? nlohmann::json(*message.date) : nlohmann::json(nullptr)},
        {"sender", {{"type", sender_kind_name(message.sender.kind)}, {"id", message.sender.id}}},
        {"is_outgoing", message.is_outgoing},
        {"topic", message.topic ? topic_ref_json(*message.topic) : nlohmann::json(nullptr)},
        {"type", content_kind_name(message.type)},
        {"text", message.text}};
}

bool persistable_message_summary(const MessageSummary& message) {
    if (!valid_int53(message.id) || !valid_int53(message.chat_id) ||
        !common::valid_utf8(message.text) ||
        message.text.size() > account_audit_limits::kMessageTextBytes) {
        return false;
    }
    const auto scalars = static_cast<std::size_t>(
        std::count_if(message.text.begin(), message.text.end(),
                      [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }));
    return scalars <= account_audit_limits::kMessageTextScalars;
}

} // namespace tgcli::daemon
