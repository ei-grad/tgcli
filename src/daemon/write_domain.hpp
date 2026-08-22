#pragma once

#include "daemon/message_summary.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace tgcli::daemon {

enum class SendScheduleKind { At, Online };

struct SendSchedule {
    SendScheduleKind kind = SendScheduleKind::At;
    std::int32_t send_date = 0;

    bool operator==(const SendSchedule&) const = default;
};

[[nodiscard]] bool valid_send_text(std::string_view text);
[[nodiscard]] bool valid_saved_attach_caption(std::string_view text);
[[nodiscard]] bool valid_message_reaction(std::string_view reaction);
[[nodiscard]] std::optional<std::int32_t> parse_mute_duration(std::string_view duration);
[[nodiscard]] std::optional<TopicRef> parse_send_topic(std::string_view value);
[[nodiscard]] std::optional<SendSchedule> parse_send_schedule(std::string_view value);

} // namespace tgcli::daemon
