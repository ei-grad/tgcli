#pragma once

#include "core/m6_td.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

struct M6TopicCursor {
    std::int32_t date = 0;
    std::int64_t message_id = 0;
    std::int32_t topic_id = 0;

    bool operator==(const M6TopicCursor&) const = default;
};

enum class M6TopicScanStatus { Accepted, Complete, StructuralError, NonAdvancing, Capacity };
enum class M6TopicCapacityResource { Topics, Bytes, ItemBytes };

struct M6TopicScanResult {
    M6TopicScanStatus status = M6TopicScanStatus::StructuralError;
    std::optional<M6TopicCursor> next;
    std::optional<M6TopicCapacityResource> capacity_resource;
    std::size_t capacity_limit = 0;
};

bool valid_m6_topic_cursor(const M6TopicCursor& cursor) noexcept;

namespace testing {
using M6TopicSerializedSize = std::function<std::size_t(const nlohmann::json&)>;
}

class M6TopicAccumulator final {
  public:
    explicit M6TopicAccumulator(std::int64_t chat_id,
                                testing::M6TopicSerializedSize serialized_size = {})
        : chat_id_(chat_id), serialized_size_(std::move(serialized_size)) {}

    M6TopicScanResult append(const M6TopicCursor& request, const core::TdM6ForumTopics& page);
    [[nodiscard]] const std::vector<nlohmann::json>& items() const noexcept;

  private:
    [[nodiscard]] std::size_t serialized_size(const nlohmann::json& value) const;

    std::int64_t chat_id_ = 0;
    std::vector<nlohmann::json> items_;
    std::unordered_set<std::int32_t> topic_ids_;
    std::vector<M6TopicCursor> cursors_;
    std::optional<std::int64_t> previous_order_;
    std::size_t charged_bytes_ = 0;
    bool complete_ = false;
    testing::M6TopicSerializedSize serialized_size_;
};

} // namespace tgcli::daemon
