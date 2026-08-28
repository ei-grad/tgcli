#include "daemon/m6_topic_scan.hpp"

#include <cstddef>
#include <limits>

#include <catch2/catch_test_macros.hpp>

namespace {

namespace core = tgcli::core;
namespace daemon = tgcli::daemon;

core::TdM6ForumTopic topic(std::int32_t id, std::int64_t order) {
    return {.info = {.chat_id = -1001,
                     .id = id,
                     .name = "Topic",
                     .icon = {.color = core::TdM6TopicColor::Blue, .custom_emoji_id = "0"},
                     .creation_date = 1,
                     .creator = {.kind = core::TdM6SenderKind::User,
                                 .id = 42,
                                 .unsupported_tdlib_type_id = std::nullopt}},
            .order = order,
            .is_pinned = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0};
}

core::TdM6ForumTopics page(std::vector<core::TdM6ForumTopic> topics, daemon::M6TopicCursor next) {
    return {.total_count = static_cast<std::int32_t>(topics.size()),
            .topics = std::move(topics),
            .next_offset_date = next.date,
            .next_offset_message_id = next.message_id,
            .next_offset_forum_topic_id = next.topic_id};
}

TEST_CASE("M6 topic cursor structural validation precedes progress", "[m6][topic-scan]") {
    constexpr std::int64_t unit = 1LL << 20;
    constexpr std::int64_t maximum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) << 20;
    CHECK(daemon::valid_m6_topic_cursor({}));
    CHECK(daemon::valid_m6_topic_cursor({.date = 1, .message_id = unit, .topic_id = 1}));
    CHECK_FALSE(daemon::valid_m6_topic_cursor({.date = -1}));
    CHECK_FALSE(daemon::valid_m6_topic_cursor({.message_id = unit + 1}));
    CHECK_FALSE(daemon::valid_m6_topic_cursor({.message_id = maximum + unit}));
    CHECK_FALSE(daemon::valid_m6_topic_cursor({.topic_id = -1}));

    daemon::M6TopicAccumulator accumulator(-1001);
    const auto malformed = page({topic(1, 2)}, {.date = 1, .message_id = unit + 1, .topic_id = 1});
    CHECK(accumulator.append({}, malformed).status == daemon::M6TopicScanStatus::StructuralError);
    CHECK(accumulator.items().empty());
}

TEST_CASE("M6 topic scanner accepts pages then closes only on the zero cursor",
          "[m6][topic-scan]") {
    constexpr std::int64_t unit = 1LL << 20;
    daemon::M6TopicAccumulator accumulator(-1001);
    const daemon::M6TopicCursor first_next{.date = 10, .message_id = 10 * unit, .topic_id = 10};
    const auto first = accumulator.append({}, page({topic(1, 20)}, first_next));
    REQUIRE(first.status == daemon::M6TopicScanStatus::Accepted);
    REQUIRE(first.next == first_next);
    CHECK(accumulator.items().size() == 1);

    const auto last = accumulator.append(first_next, page({topic(2, 10)}, {}));
    CHECK(last.status == daemon::M6TopicScanStatus::Complete);
    CHECK(accumulator.items().size() == 2);
}

TEST_CASE("M6 topic scanner separates structural duplicate from cursor non-advancement",
          "[m6][topic-scan]") {
    constexpr std::int64_t unit = 1LL << 20;
    const daemon::M6TopicCursor cursor{.date = 10, .message_id = 10 * unit, .topic_id = 10};

    daemon::M6TopicAccumulator duplicate(-1001);
    REQUIRE(duplicate.append({}, page({topic(1, 20)}, cursor)).status ==
            daemon::M6TopicScanStatus::Accepted);
    CHECK(duplicate.append(cursor, page({topic(1, 10)}, {})).status ==
          daemon::M6TopicScanStatus::NonAdvancing);

    daemon::M6TopicAccumulator stalled(-1001);
    REQUIRE(stalled.append({}, page({topic(1, 20)}, cursor)).status ==
            daemon::M6TopicScanStatus::Accepted);
    CHECK(stalled.append(cursor, page({}, cursor)).status ==
          daemon::M6TopicScanStatus::NonAdvancing);
}

TEST_CASE("M6 topic scanner rejects cross-page order regression and item capacity",
          "[m6][topic-scan]") {
    constexpr std::int64_t unit = 1LL << 20;
    const daemon::M6TopicCursor cursor{.date = 10, .message_id = 10 * unit, .topic_id = 10};
    daemon::M6TopicAccumulator ordering(-1001);
    REQUIRE(ordering.append({}, page({topic(1, 10)}, cursor)).status ==
            daemon::M6TopicScanStatus::Accepted);
    CHECK(ordering.append(cursor, page({topic(2, 11)}, {})).status ==
          daemon::M6TopicScanStatus::StructuralError);

    daemon::M6TopicAccumulator capacity(-1001);
    daemon::M6TopicCursor request;
    for (std::int32_t id = 1; id <= 4'096; ++id) {
        const daemon::M6TopicCursor next{.date = 5'000 - id,
                                         .message_id = static_cast<std::int64_t>(5'000 - id) * unit,
                                         .topic_id = 5'000 - id};
        const auto result = capacity.append(request, page({topic(id, 5'000 - id)}, next));
        REQUIRE(result.status == daemon::M6TopicScanStatus::Accepted);
        request = next;
    }
    const auto overflow = capacity.append(request, page({topic(4'097, 0)}, {}));
    CHECK(overflow.status == daemon::M6TopicScanStatus::Capacity);
    CHECK(overflow.capacity_resource == daemon::M6TopicCapacityResource::Topics);
    CHECK(overflow.capacity_limit == 4'096);
    CHECK(capacity.items().size() == 4'096);
}

TEST_CASE("M6 topic scanner enforces exact item and aggregate byte boundaries atomically",
          "[m6][topic-scan][capacity]") {
    constexpr std::size_t item_limit = 262'144;
    constexpr std::size_t bytes_limit = 16'777'216;
    const auto fixed_charge = [](std::size_t bytes) {
        return [bytes](const nlohmann::json&) { return bytes; };
    };

    daemon::M6TopicAccumulator item_exact(-1001, fixed_charge(item_limit));
    const auto exact_item = item_exact.append({}, page({topic(1, 1)}, {}));
    CHECK(exact_item.status == daemon::M6TopicScanStatus::Complete);
    CHECK(item_exact.items().size() == 1);

    daemon::M6TopicAccumulator item_overflow(-1001, fixed_charge(item_limit + 1));
    const auto oversized_item = item_overflow.append({}, page({topic(1, 1)}, {}));
    CHECK(oversized_item.status == daemon::M6TopicScanStatus::Capacity);
    CHECK(oversized_item.capacity_resource == daemon::M6TopicCapacityResource::ItemBytes);
    CHECK(oversized_item.capacity_limit == item_limit);
    CHECK(item_overflow.items().empty());

    std::vector<core::TdM6ForumTopic> exact_topics;
    exact_topics.reserve(64);
    for (std::int32_t id = 1; id <= 64; ++id) {
        exact_topics.push_back(topic(id, 100 - id));
    }
    daemon::M6TopicAccumulator bytes_exact(-1001, fixed_charge(item_limit));
    const auto exact_bytes = bytes_exact.append({}, page(std::move(exact_topics), {}));
    CHECK(exact_bytes.status == daemon::M6TopicScanStatus::Complete);
    CHECK(bytes_exact.items().size() == 64);

    std::vector<core::TdM6ForumTopic> overflow_topics;
    overflow_topics.reserve(65);
    for (std::int32_t id = 1; id <= 65; ++id) {
        overflow_topics.push_back(topic(id, 100 - id));
    }
    daemon::M6TopicAccumulator bytes_overflow(-1001, [](const nlohmann::json& value) {
        return value["id"].get<std::int32_t>() <= 64 ? item_limit : std::size_t{1};
    });
    const auto oversized_bytes = bytes_overflow.append({}, page(std::move(overflow_topics), {}));
    CHECK(oversized_bytes.status == daemon::M6TopicScanStatus::Capacity);
    CHECK(oversized_bytes.capacity_resource == daemon::M6TopicCapacityResource::Bytes);
    CHECK(oversized_bytes.capacity_limit == bytes_limit);
    CHECK(bytes_overflow.items().empty());
}

} // namespace
