#pragma once

#include "core/td_runtime.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

enum class TopicKind { Forum, Thread, Direct, Saved };

struct TopicRef {
    TopicKind kind = TopicKind::Forum;
    std::int64_t id = 0;

    bool operator==(const TopicRef&) const = default;
};

enum class MessageSenderKind { User, Chat };

struct MessageSenderRef {
    MessageSenderKind kind = MessageSenderKind::User;
    std::int64_t id = 0;

    bool operator==(const MessageSenderRef&) const = default;
};

enum class MessageContentKind { Text, Photo, Video, Document, Voice, Other };

struct MessageSummary {
    std::int64_t id = 0;
    std::int64_t chat_id = 0;
    std::optional<std::string> date;
    MessageSenderRef sender;
    bool is_outgoing = false;
    std::optional<TopicRef> topic;
    MessageContentKind type = MessageContentKind::Other;
    std::string text;

    bool operator==(const MessageSummary&) const = default;
};

std::optional<TopicRef> materialize_topic_ref(const core::TdTopic& topic);
std::optional<MessageSummary> materialize_message_summary(const core::TdMessageSummary& message);
nlohmann::json topic_ref_json(const TopicRef& topic);
nlohmann::json message_summary_json(const MessageSummary& message);
bool persistable_message_summary(const MessageSummary& message);

} // namespace tgcli::daemon
