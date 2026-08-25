#include "daemon/stream_ingress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

namespace {

using namespace tgcli::daemon;

StreamIngressRequest request(std::int32_t client_id = 1001, std::uint64_t generation = 7) {
    StreamIngressRequest result{.client_id = client_id,
                                .generation = generation,
                                .operation = StreamOperation::Listen,
                                .mode = StreamMode::Items,
                                .type_mask = stream_event_mask(StreamEventClass::Message) |
                                             stream_event_mask(StreamEventClass::Chat)};
    result.chat_ids[0] = -1002;
    result.chat_ids[1] = -1001;
    result.chat_count = 2;
    return result;
}

StreamRoutingSidecar routing(StreamEventClass event = StreamEventClass::Message,
                             std::int64_t chat_id = -1001) {
    return {.event_class = event,
            .chat_id = chat_id,
            .sender_kind = StreamSenderKind::User,
            .sender_id = 42,
            .message_offset = 11,
            .message_size = 8,
            .text_offset = 16,
            .text_size = 2};
}

StreamItemView item(std::string_view bytes, std::uint64_t sequence,
                    StreamRoutingSidecar sidecar = routing()) {
    const auto middle = bytes.size() / 2;
    sidecar.json_size = static_cast<std::uint32_t>(bytes.size());
    return StreamIngressTestAccess::item(bytes.substr(0, middle), bytes.substr(middle), sequence,
                                         sidecar);
}

std::string copy(const StreamIngressItemView& value) {
    std::string result;
    for (const auto bytes : value.spans()) {
        result.append(bytes.data(), bytes.size());
    }
    return result;
}

struct BlockingProbe {
    std::size_t target = 0;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    static void notify(void* context, detail::StreamIngressProbePoint point,
                       std::size_t index) noexcept {
        auto& probe = *static_cast<BlockingProbe*>(context);
        if (point != detail::StreamIngressProbePoint::SlotLoad || index != probe.target) {
            return;
        }
        probe.entered.store(true, std::memory_order_release);
        while (!probe.release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

void wait_entered(const BlockingProbe& probe) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!probe.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(probe.entered.load(std::memory_order_acquire));
}

} // namespace

static_assert(sizeof(tgcli::daemon::StreamIngressDescriptor) == 64);
static_assert(sizeof(tgcli::daemon::StreamActivationProjection) <= 552);
static_assert(!std::is_copy_constructible_v<tgcli::daemon::StreamIngressReservation>);
static_assert(std::is_move_constructible_v<tgcli::daemon::StreamIngressReservation>);
static_assert(!std::is_copy_constructible_v<tgcli::daemon::StreamIngressItemView>);
static_assert(std::atomic<tgcli::daemon::StreamIngressSlot*>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<tgcli::daemon::StreamTerminalCause>::is_always_lock_free);

TEST_CASE("stream ingress limits are exact", "[stream][ingress][limits]") {
    CHECK(tgcli::daemon::kStreamSubscriberSlots == 32);
    CHECK(tgcli::daemon::kStreamQueueItems == 1'024);
    CHECK(tgcli::daemon::kStreamQueueBytes == 8'388'608);
    CHECK(tgcli::daemon::kStreamQueueItemBytes == 262'144);
    CHECK(tgcli::daemon::kStreamChatFilters == 64);
    CHECK(tgcli::daemon::kStreamWorkerPollInterval == 2ms);
}

TEST_CASE("stream ingress activation freezes the narrow projection",
          "[stream][ingress][activation]") {
    StreamIngressHub hub;
    REQUIRE_FALSE(hub.lock_free_failure());
    auto reserved = hub.reserve(request());
    REQUIRE(reserved);
    CHECK(hub.activation_state(*reserved) == StreamIngressState::Reserved);
    REQUIRE(hub.commit_activation(*reserved));
    CHECK(hub.activation_state(*reserved) == StreamIngressState::Armed);
    CHECK(hub.activate_armed(1001, 7, 41) == 1);
    CHECK(hub.activation_state(*reserved) == StreamIngressState::Published);
    const auto projection = hub.activation_projection(*reserved);
    REQUIRE(projection);
    CHECK(projection->client_id == 1001);
    CHECK(projection->generation == 7);
    CHECK(projection->activation_receive_sequence == 41);
    CHECK(projection->chat_count == 2);
    CHECK(projection->chat_ids[0] == -1002);
    CHECK(projection->chat_ids[1] == -1001);
    CHECK(projection->type_mask == request().type_mask);
    CHECK(projection->mode == StreamMode::Items);
    CHECK(projection->operation == StreamOperation::Listen);
}

TEST_CASE("stream ingress reports the first failing lock-free atomic exactly",
          "[stream][ingress][capacity]") {
    constexpr std::array atomics{
        StreamIngressAtomic::SlotPointer, StreamIngressAtomic::PublisherCount,
        StreamIngressAtomic::DescriptorIndex, StreamIngressAtomic::ByteIndex,
        StreamIngressAtomic::TerminalCause};
    for (const auto atomic : atomics) {
        StreamIngressHub hub({.forced_lock_free_failure = atomic});
        CHECK(hub.lock_free_failure() == atomic);
        CHECK_FALSE(hub.reserve(request()));
        const auto failure = hub.last_reservation_failure();
        REQUIRE(failure);
        CHECK(failure->resource == StreamIngressAdmissionResource::LockFreeIngress);
        CHECK(failure->atomic == atomic);
    }
}

TEST_CASE("stream ingress queue preserves routing and two-span bytes", "[stream][ingress][queue]") {
    StreamIngressHub hub;
    auto reserved = hub.reserve(request());
    REQUIRE(reserved);
    REQUIRE(hub.commit_activation(*reserved));
    REQUIRE(hub.activate_armed(1001, 7, 10) == 1);

    constexpr std::string_view line = "{\"event\":\"message\"}\n";
    hub.publish(item(line, 11));
    auto front = hub.poll_front(*reserved);
    REQUIRE(front);
    CHECK(front->descriptor().receive_sequence == 11);
    CHECK(front->descriptor().chat_id == -1001);
    CHECK(front->descriptor().sender_id == 42);
    CHECK(front->descriptor().event_class == StreamEventClass::Message);
    CHECK(front->descriptor().message_offset == 11);
    CHECK(front->descriptor().text_offset == 16);
    CHECK(copy(*front) == line);
    REQUIRE(hub.consume(*reserved, *front));
    for (const auto bytes : front->spans()) {
        CHECK(std::ranges::all_of(
            bytes, [](char value) { return static_cast<unsigned char>(value) == 0xA7; }));
    }
    CHECK_FALSE(hub.poll_front(*reserved));
}

TEST_CASE("stream ingress byte ring wraps without changing logical order",
          "[stream][ingress][queue][wrap]") {
    StreamIngressHub hub;
    auto reserved = hub.reserve(request());
    REQUIRE(reserved);
    REQUIRE(hub.commit_activation(*reserved));
    REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
    StreamIngressTestAccess::set_tickets(hub, *reserved, 0, 0, kStreamQueueBytes - 2,
                                         kStreamQueueBytes - 2);
    constexpr std::string_view line = "abc\n";
    hub.publish(item(line, 2));
    auto front = hub.poll_front(*reserved);
    REQUIRE(front);
    CHECK(front->spans()[0].size() == 2);
    CHECK(front->spans()[1].size() == 2);
    CHECK(copy(*front) == line);
}

TEST_CASE("stream ingress applies immutable type chat generation and anchor filters",
          "[stream][ingress][filter]") {
    StreamIngressHub hub;
    auto reserved = hub.reserve(request());
    REQUIRE(reserved);
    REQUIRE(hub.commit_activation(*reserved));
    REQUIRE(hub.activate_armed(1001, 7, 10) == 1);
    constexpr std::string_view line = "{}\n";
    hub.publish(item(line, 10));
    hub.publish(item(line, 11, routing(StreamEventClass::Delete)));
    hub.publish(item(line, 12, routing(StreamEventClass::Message, -2000)));
    CHECK_FALSE(hub.poll_front(*reserved));
    hub.publish(item(line, 13));
    REQUIRE(hub.poll_front(*reserved));
    CHECK(hub.poll_front(*reserved)->descriptor().receive_sequence == 13);
}

TEST_CASE("stream ingress admission is exact and waits for exchange plus quiescence",
          "[stream][ingress][capacity][reclaim]") {
    StreamIngressHub hub;
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        reservations.push_back(std::move(*reserved));
    }
    CHECK_FALSE(hub.reserve(request()));
    const auto failure = hub.last_reservation_failure();
    REQUIRE(failure);
    CHECK(failure->resource == StreamIngressAdmissionResource::SubscriberSlots);

    REQUIRE(hub.activate_armed(1001, 7, 4) == kStreamSubscriberSlots);
    StreamIngressTestAccess::hold_publisher(hub, true);
    REQUIRE(hub.detach(reservations.front()));
    CHECK_FALSE(hub.poll_reclaim(reservations.front()));
    CHECK_FALSE(hub.reserve(request()));
    StreamIngressTestAccess::hold_publisher(hub, false);
    REQUIRE(hub.poll_reclaim(reservations.front()));
    CHECK(hub.reserve(request()));
}

TEST_CASE("stream ingress removal waits for a publisher that loaded the slot",
          "[stream][ingress][reclaim][concurrency]") {
    BlockingProbe probe;
    StreamIngressHub hub({.context = &probe,
                          .hook = &BlockingProbe::notify,
                          .forced_lock_free_failure = std::nullopt});
    auto reserved = hub.reserve(request());
    REQUIRE(reserved);
    REQUIRE(hub.commit_activation(*reserved));
    REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
    constexpr std::string_view line = "{}\n";
    auto value = item(line, 2);
    std::thread publisher([&] { hub.publish(value); });
    wait_entered(probe);
    REQUIRE(hub.detach(*reserved));
    CHECK_FALSE(hub.poll_reclaim(*reserved));
    probe.release.store(true, std::memory_order_release);
    publisher.join();
    REQUIRE(hub.poll_reclaim(*reserved));
    CHECK(hub.reserve(request()));
}

TEST_CASE("stream ingress proves P I L D X Z for every registry slot",
          "[stream][ingress][reclaim][concurrency]") {
    BlockingProbe probe;
    StreamIngressHub hub({.context = &probe,
                          .hook = &BlockingProbe::notify,
                          .forced_lock_free_failure = std::nullopt});
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        reservations.push_back(std::move(*reserved));
    }
    REQUIRE(hub.activate_armed(1001, 7, 1) == kStreamSubscriberSlots);
    constexpr std::string_view line = "{}\n";
    auto value = item(line, 2);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        probe.target = index;
        probe.entered.store(false, std::memory_order_relaxed);
        probe.release.store(false, std::memory_order_relaxed);
        std::thread publisher([&] { hub.publish(value); });
        wait_entered(probe);
        REQUIRE(hub.detach(reservations[index]));
        CHECK_FALSE(hub.poll_reclaim(reservations[index]));
        probe.release.store(true, std::memory_order_release);
        publisher.join();
        REQUIRE(hub.poll_reclaim(reservations[index]));
    }
}

TEST_CASE("stream ingress queue failures use exact precedence and first cause",
          "[stream][ingress][overflow]") {
    constexpr std::string_view line = "{}\n";
    SECTION("item bytes precedes item and byte occupancy") {
        StreamIngressHub hub;
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, *reserved, kStreamQueueItems, 0,
                                             kStreamQueueBytes, 0);
        const std::string oversized(kStreamQueueItemBytes + 1, 'x');
        hub.publish(item(oversized, 2));
        const auto terminal = hub.claim_terminal(*reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::ItemBytes);
        CHECK(terminal->incoming_bytes == kStreamQueueItemBytes + 1);
    }
    SECTION("descriptor count precedes byte capacity") {
        StreamIngressHub hub;
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, *reserved, kStreamQueueItems, 0,
                                             kStreamQueueBytes, 0);
        hub.publish(item(line, 2));
        const auto terminal = hub.claim_terminal(*reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::QueueItems);
    }
    SECTION("byte capacity is exact") {
        StreamIngressHub hub;
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        const std::string maximum(kStreamQueueItemBytes, 'x');
        for (std::size_t index = 0; index < kStreamQueueBytes / kStreamQueueItemBytes; ++index) {
            hub.publish(item(maximum, index + 2));
        }
        CHECK_FALSE(hub.terminal_snapshot(*reserved));
        hub.publish(item(line, 40));
        const auto terminal = hub.claim_terminal(*reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::QueueBytes);
        CHECK(terminal->queued_bytes == kStreamQueueBytes);
    }
    SECTION("descriptor counter exhaustion wins before size") {
        StreamIngressHub hub;
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, *reserved,
                                             std::numeric_limits<std::uint64_t>::max(),
                                             std::numeric_limits<std::uint64_t>::max(), 0, 0);
        const std::string oversized(kStreamQueueItemBytes + 1, 'x');
        hub.publish(item(oversized, 2));
        const auto terminal = hub.claim_terminal(*reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::CounterExhausted);
    }
    SECTION("byte counter exhaustion wins before capacity") {
        StreamIngressHub hub;
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, *reserved, 0, 0,
                                             std::numeric_limits<std::uint64_t>::max() - 1,
                                             std::numeric_limits<std::uint64_t>::max() - 1);
        hub.publish(item(line, 2));
        const auto terminal = hub.claim_terminal(*reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::CounterExhausted);
    }
    SECTION("first terminal payload is retained") {
        StreamIngressHub hub;
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.claim(*reserved, {.cause = StreamTerminalCause::Shutdown,
                                      .operation = StreamOperation::Listen}));
        CHECK_FALSE(hub.claim(*reserved, {.cause = StreamTerminalCause::AuthorizationLost,
                                          .operation = StreamOperation::WaitFor,
                                          .auth_state = 9}));
        const auto terminal = hub.claim_terminal(*reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::Shutdown);
        CHECK(terminal->operation == StreamOperation::Listen);
    }
}

TEST_CASE("one full ingress queue does not alter another subscriber",
          "[stream][ingress][isolation]") {
    StreamIngressHub hub;
    auto first = hub.reserve(request());
    auto second = hub.reserve(request());
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(hub.commit_activation(*first));
    REQUIRE(hub.commit_activation(*second));
    REQUIRE(hub.activate_armed(1001, 7, 1) == 2);
    constexpr std::string_view line = "{}\n";
    StreamIngressTestAccess::set_tickets(hub, *first, kStreamQueueItems, 0, 0, 0);
    hub.publish(item(line, 2));
    REQUIRE(hub.claim_terminal(*first));
    CHECK(hub.terminal_snapshot(*first)->cause == StreamTerminalCause::QueueItems);
    auto front = hub.poll_front(*second);
    REQUIRE(front);
    CHECK(copy(*front) == line);
}

TEST_CASE("stream poll schedule substitutes deadlines and skips elapsed ticks",
          "[stream][ingress][schedule]") {
    using Clock = StreamPollSchedule::Clock;
    const auto start = Clock::time_point{};
    StreamPollSchedule schedule(start);
    CHECK(schedule.next(std::nullopt) == start + 2ms);
    CHECK(schedule.next(start + 1ms) == start + 1ms);
    schedule.advance(start + 7ms);
    CHECK(schedule.next(std::nullopt) == start + 8ms);
}

TEST_CASE("stream ingress preallocated slot survives one million ABA-safe cycles",
          "[stream][ingress][stress]") {
    StreamIngressHub hub;
    constexpr std::string_view line = "{}\n";
    auto value = item(line, 2);
    for (std::size_t cycle = 0; cycle < 1'000'000; ++cycle) {
        auto reserved = hub.reserve(request());
        REQUIRE(reserved);
        REQUIRE(hub.commit_activation(*reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        hub.publish(value);
        auto front = hub.poll_front(*reserved);
        REQUIRE(front);
        REQUIRE(hub.consume(*reserved, *front));
        REQUIRE(hub.detach(*reserved));
        REQUIRE(hub.poll_reclaim(*reserved));
    }
}
