#include "daemon/m6_topic_scan.hpp"

#include "daemon/m6_model.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <tuple>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kServerMessageUnit = 1LL << 20;
constexpr std::int64_t kMaximumServerMessageId =
    static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) << 20;
constexpr std::size_t kMaximumTopics = 4'096;
constexpr std::size_t kMaximumBytes = 16'777'216;
constexpr std::size_t kMaximumItemBytes = 262'144;

bool terminal(const M6TopicCursor& cursor) {
    return cursor.date == 0 && cursor.message_id == 0 && cursor.topic_id == 0;
}

bool strictly_after(const M6TopicCursor& next, const M6TopicCursor& request) {
    return std::tie(next.date, next.message_id, next.topic_id) <
           std::tie(request.date, request.message_id, request.topic_id);
}

} // namespace

bool valid_m6_topic_cursor(const M6TopicCursor& cursor) noexcept {
    if (cursor.date < 0 || cursor.topic_id < 0 || cursor.message_id < 0) {
        return false;
    }
    return cursor.message_id == 0 || (cursor.message_id >= kServerMessageUnit &&
                                      cursor.message_id <= kMaximumServerMessageId &&
                                      (cursor.message_id & (kServerMessageUnit - 1)) == 0);
}

M6TopicScanResult M6TopicAccumulator::append(const M6TopicCursor& request,
                                             const core::TdM6ForumTopics& page) {
    if (complete_ || !valid_m6_topic_cursor(request)) {
        return {.status = M6TopicScanStatus::StructuralError, .next = std::nullopt};
    }
    const M6TopicCursor next{.date = page.next_offset_date,
                             .message_id = page.next_offset_message_id,
                             .topic_id = page.next_offset_forum_topic_id};
    if (!valid_m6_topic_cursor(next) || page.total_count < 0 || page.topics.size() > 100 ||
        static_cast<std::size_t>(page.total_count) < page.topics.size()) {
        return {.status = M6TopicScanStatus::StructuralError, .next = std::nullopt};
    }

    std::vector<nlohmann::json> accepted;
    accepted.reserve(page.topics.size());
    std::vector<std::int32_t> accepted_ids;
    accepted_ids.reserve(page.topics.size());
    auto previous_order = previous_order_;
    std::size_t accepted_bytes = 0;
    for (const auto& topic : page.topics) {
        const auto projected = m6_topic_row_json(topic);
        if (!projected || topic.info.chat_id != chat_id_ || topic.info.id <= 0 ||
            topic_ids_.contains(topic.info.id) ||
            std::ranges::find(accepted_ids, topic.info.id) != accepted_ids.end() ||
            (previous_order && topic.order > *previous_order)) {
            return {.status = M6TopicScanStatus::StructuralError, .next = std::nullopt};
        }
        const auto bytes = projected->dump().size();
        if (bytes > kMaximumItemBytes || accepted_bytes > kMaximumBytes - bytes ||
            charged_bytes_ > kMaximumBytes - accepted_bytes - bytes ||
            items_.size() + accepted.size() >= kMaximumTopics) {
            return {.status = M6TopicScanStatus::Capacity, .next = std::nullopt};
        }
        accepted_bytes += bytes;
        accepted_ids.push_back(topic.info.id);
        accepted.push_back(std::move(*projected));
        previous_order = topic.order;
    }

    if (!terminal(next)) {
        if (page.topics.empty() || (!terminal(request) && !strictly_after(next, request)) ||
            std::ranges::find(cursors_, next) != cursors_.end()) {
            return {.status = M6TopicScanStatus::NonAdvancing, .next = std::nullopt};
        }
    }

    for (const auto id : accepted_ids) {
        topic_ids_.insert(id);
    }
    items_.insert(items_.end(), std::make_move_iterator(accepted.begin()),
                  std::make_move_iterator(accepted.end()));
    charged_bytes_ += accepted_bytes;
    previous_order_ = previous_order;
    if (terminal(next)) {
        complete_ = true;
        return {.status = M6TopicScanStatus::Complete, .next = std::nullopt};
    }
    cursors_.push_back(next);
    return {.status = M6TopicScanStatus::Accepted, .next = next};
}

const std::vector<nlohmann::json>& M6TopicAccumulator::items() const noexcept {
    return items_;
}

} // namespace tgcli::daemon
