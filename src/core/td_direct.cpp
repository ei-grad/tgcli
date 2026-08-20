#include "common/utf8.hpp"
#include "core/td_runtime.hpp"

#include <algorithm>
#include <functional>
#include <type_traits>

namespace tgcli::core {

namespace {

bool valid_message_ids(const std::vector<std::int64_t>& values, std::size_t maximum,
                       bool strictly_increasing) {
    if (values.empty() || values.size() > maximum ||
        !std::ranges::all_of(values, valid_td_message_id)) {
        return false;
    }
    return !strictly_increasing ||
           std::adjacent_find(values.begin(), values.end(), std::greater_equal<>{}) == values.end();
}

} // namespace

bool valid_td_message_locator(std::int64_t chat_id, std::int64_t message_id) noexcept {
    return valid_td_chat_id(chat_id) && valid_td_message_id(message_id);
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
        return !request.invite_link.has_value() && valid_td_chat_id(request.chat_id.value_or(0));
    }
    if (!request.invite_link.has_value()) {
        return false;
    }
    const auto& invite_link = request.invite_link.value();
    return !invite_link.empty() && common::valid_utf8(invite_link);
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

} // namespace tgcli::core
