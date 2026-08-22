#include "common/utf8.hpp"
#include "core/td_runtime.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>

namespace tgcli::core {

namespace {

bool valid_message_ids(const std::vector<std::int64_t>& values, std::size_t maximum,
                       bool strictly_increasing) {
    if (values.empty() || values.size() > maximum ||
        !std::ranges::all_of(values, valid_td_nonzero_int53)) {
        return false;
    }
    return !strictly_increasing ||
           std::adjacent_find(values.begin(), values.end(), std::greater_equal<>{}) == values.end();
}

bool valid_send_text(std::string_view text) {
    if (text.empty() || text.find('\0') != std::string_view::npos || !common::valid_utf8(text)) {
        return false;
    }
    std::size_t scalar_count = 0;
    for (const auto byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U && ++scalar_count > 4'096) {
            return false;
        }
    }
    return true;
}

std::optional<std::size_t> utf16_code_units(std::string_view text) {
    if (!common::valid_utf8(text)) {
        return std::nullopt;
    }
    std::size_t units = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        std::uint32_t codepoint = first;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0U) {
            length = 4;
            codepoint = first & 0x07U;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[index + continuation]);
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        units += codepoint > 0xFFFFU ? 2 : 1;
        index += length;
    }
    return units;
}

bool is_utf16_boundary(std::string_view text, std::size_t target) {
    if (target == 0) {
        return true;
    }
    std::size_t units = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        if (first >= 0xF0U) {
            length = 4;
        } else if (first >= 0xE0U) {
            length = 3;
        } else if (first >= 0xC2U) {
            length = 2;
        }
        units += length == 4 ? 2 : 1;
        if (units >= target) {
            return units == target;
        }
        index += length;
    }
    return false;
}

bool valid_send_topic(const std::optional<TdTopic>& topic) {
    if (!topic) {
        return true;
    }
    if (topic->kind == TdTopicKind::Forum) {
        return topic->id > 0 && topic->id <= std::numeric_limits<std::int32_t>::max();
    }
    return topic->kind == TdTopicKind::Saved && valid_td_message_id(topic->id);
}

std::string send_topic_name(const std::optional<TdTopic>& topic) {
    if (!topic) {
        return "none";
    }
    return topic->kind == TdTopicKind::Forum ? "forum" : "saved";
}

std::string send_schedule_name(TdSendScheduleKind schedule) {
    switch (schedule) {
    case TdSendScheduleKind::Immediate:
        return "immediate";
    case TdSendScheduleKind::AtDate:
        return "at_date";
    case TdSendScheduleKind::WhenOnline:
        return "when_online";
    }
    return "unknown";
}

} // namespace

bool valid_td_formatted_text_facts(const TdFormattedText& formatted) noexcept {
    const auto units = utf16_code_units(formatted.text);
    if (!units) {
        return false;
    }
    return std::ranges::all_of(formatted.entities, [&](const TdTextEntity& entity) {
        if (entity.kind == TdTextEntityKind::Unknown || entity.offset < 0 || entity.length <= 0) {
            return false;
        }
        const auto offset = static_cast<std::size_t>(entity.offset);
        const auto length = static_cast<std::size_t>(entity.length);
        if (offset > *units || length > *units - offset) {
            return false;
        }
        return is_utf16_boundary(formatted.text, offset) &&
               is_utf16_boundary(formatted.text, offset + length);
    });
}

bool valid_td_message_locator(std::int64_t chat_id, std::int64_t message_id) noexcept {
    return valid_td_chat_id(chat_id) && valid_td_nonzero_int53(message_id);
}

bool valid_td_direct_request(const TdEditMessageTextRequest& request) noexcept {
    return valid_td_message_locator(request.chat_id, request.message_id) &&
           common::valid_utf8(request.text);
}

bool valid_td_direct_request(const TdDeleteMessagesRequest& request) noexcept {
    return valid_td_chat_id(request.chat_id) && valid_message_ids(request.message_ids, 100, true);
}

bool valid_td_direct_request(const TdMessageReactionRequest& request) noexcept {
    return valid_td_message_locator(request.chat_id, request.message_id) &&
           !request.reaction.empty() && request.reaction.size() <= 64 &&
           common::valid_utf8(request.reaction) && !(request.remove && request.big);
}

bool valid_td_direct_request(const TdPinMessageRequest& request) noexcept {
    return valid_td_message_locator(request.chat_id, request.message_id);
}

bool valid_td_direct_request(const TdViewMessagesRequest& request) noexcept {
    return valid_td_chat_id(request.chat_id) && valid_message_ids(request.message_ids, 1, false);
}

bool valid_td_direct_request(const TdSetChatNotificationSettingsRequest& request) noexcept {
    return valid_td_chat_id(request.chat_id) && request.settings.mute_for >= 0;
}

bool valid_td_direct_request(const TdToggleChatIsPinnedRequest& request) noexcept {
    return valid_td_chat_id(request.chat_id);
}

bool valid_td_direct_request(const TdAddChatToListRequest& request) noexcept {
    return valid_td_chat_id(request.chat_id);
}

bool valid_td_direct_request(const TdJoinChatRequest& request) {
    if (request.chat_id.has_value()) {
        return !request.is_invite_request() && !request.has_invite_link() &&
               !request.expected_invite_chat_id.has_value() &&
               valid_td_chat_id(request.chat_id.value_or(0));
    }
    if (!request.is_invite_request() || !request.has_invite_link()) {
        return false;
    }
    const auto invite_link = request.invite_link().value_or(std::string_view{});
    return !invite_link.empty() && common::valid_utf8(invite_link) &&
           (!request.expected_invite_chat_id ||
            valid_td_chat_id(request.expected_invite_chat_id.value_or(0)));
}

bool valid_td_direct_request(const TdLeaveChatRequest& request) noexcept {
    return valid_td_chat_id(request.chat_id);
}

bool valid_td_direct_request(const TdDirectRequest& request) {
    if (request.valueless_by_exception()) {
        return false;
    }
    return std::visit([](const auto& value) { return valid_td_direct_request(value); }, request);
}

bool valid_td_send_message_request(const TdSendMessageRequest& request) noexcept {
    const auto& formatted = request.content.formatted_text;
    if (!valid_td_chat_id(request.chat_id) || !valid_send_topic(request.topic) ||
        (request.reply_to_message_id &&
         !valid_td_nonzero_int53(request.reply_to_message_id.value_or(0))) ||
        request.options.sending_id == 0 || !valid_send_text(formatted.text)) {
        return false;
    }
    switch (request.options.schedule.kind) {
    case TdSendScheduleKind::Immediate:
    case TdSendScheduleKind::WhenOnline:
        if (request.options.schedule.send_date != 0) {
            return false;
        }
        break;
    case TdSendScheduleKind::AtDate:
        if (request.options.schedule.send_date <= 0) {
            return false;
        }
        break;
    }
    if (request.content.parsed) {
        return formatted.capability.has_value() && valid_td_formatted_text_facts(formatted);
    }
    return formatted.entities.empty() && !formatted.capability.has_value();
}

TdFunctionData describe_td_send_message_request(const TdSendMessageRequest& request) {
    return TdFunctionData{
        TdFunctionKind::SendMessage,
        {{"chat_id", request.chat_id},
         {"topic_kind", send_topic_name(request.topic)},
         {"topic_id", request.topic ? request.topic->id : std::int64_t{0}},
         {"reply_to_message_id", request.reply_to_message_id.value_or(0)},
         {"reply_quote_is_null", true},
         {"reply_checklist_task_id", std::int64_t{0}},
         {"reply_poll_option_id", std::string{}},
         {"suggested_post_info_is_null", true},
         {"disable_notification", request.options.disable_notification},
         {"from_background", false},
         {"protect_content", false},
         {"allow_paid_broadcast", false},
         {"paid_message_star_count", std::int64_t{0}},
         {"update_order_of_installed_sticker_sets", false},
         {"schedule_kind", send_schedule_name(request.options.schedule.kind)},
         {"schedule_send_date", static_cast<std::int64_t>(request.options.schedule.send_date)},
         {"schedule_repeat_period", std::int64_t{0}},
         {"effect_id", std::int64_t{0}},
         {"sending_id", static_cast<std::int64_t>(request.options.sending_id)},
         {"only_preview", false},
         {"reply_markup_is_null", true},
         {"text", request.content.formatted_text.text},
         {"entities_count",
          static_cast<std::int64_t>(request.content.formatted_text.entities.size())},
         {"parsed", request.content.parsed},
         {"link_preview_options_is_null", true},
         {"clear_draft", false}}};
}

} // namespace tgcli::core
