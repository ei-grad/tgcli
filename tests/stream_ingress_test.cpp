#include "daemon/stream_ingress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
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

StreamIngressReservation reserve_slot(StreamIngressHub& hub,
                                      StreamIngressRequest value = request()) {
    auto result = hub.reserve(value);
    REQUIRE(std::holds_alternative<StreamIngressReservation>(result));
    return std::move(std::get<StreamIngressReservation>(result));
}

struct FrontObservation {
    std::optional<StreamIngressDescriptor> descriptor;
    std::array<std::size_t, 2> span_sizes{};
    std::string bytes;
    const StreamIngressFrontCursor* retained_cursor = nullptr;
    const StreamIngressBorrowedSpan* retained_first = nullptr;
    bool valid = true;
};

void copy_spans(void* context, const StreamIngressBorrowedSpan& first,
                const StreamIngressBorrowedSpan& second) noexcept {
    auto& observation = *static_cast<FrontObservation*>(context);
    const std::array spans{&first, &second};
    for (std::size_t index = 0; index < spans.size(); ++index) {
        const auto size = spans.at(index)->size();
        if (!size) {
            observation.valid = false;
            return;
        }
        observation.span_sizes.at(index) = *size;
        const auto offset = observation.bytes.size();
        observation.bytes.resize(offset + *size);
        observation.valid =
            observation.valid &&
            spans.at(index)->copy_to(std::span<char>{observation.bytes}.subspan(offset, *size));
    }
    observation.retained_first = &first;
}

StreamIngressFrontAction observe_front(void* context, const StreamIngressFrontCursor& cursor) {
    auto& observation = *static_cast<FrontObservation*>(context);
    observation.descriptor = cursor.descriptor();
    REQUIRE(observation.descriptor);
    REQUIRE(cursor.visit_spans(context, &copy_spans));
    REQUIRE(observation.valid);
    observation.retained_cursor = &cursor;
    return StreamIngressFrontAction::Keep;
}

StreamIngressFrontAction consume_front(void* context, const StreamIngressFrontCursor& cursor) {
    static_cast<void>(observe_front(context, cursor));
    return StreamIngressFrontAction::Consume;
}

std::optional<FrontObservation>
read_front(StreamIngressHub& hub, StreamIngressReservation& reservation, bool consume = false) {
    FrontObservation observation;
    const auto result =
        hub.visit_front(reservation, &observation, consume ? &consume_front : &observe_front);
    if (result == StreamIngressFrontResult::Empty) {
        return std::nullopt;
    }
    REQUIRE(result ==
            (consume ? StreamIngressFrontResult::Consumed : StreamIngressFrontResult::Visited));
    return observation;
}

struct BlockingProbe {
    std::size_t target = 0;
    std::atomic<bool> enabled{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    static void notify(void* context, detail::StreamIngressProbePoint point,
                       std::size_t index) noexcept {
        auto& probe = *static_cast<BlockingProbe*>(context);
        if (!probe.enabled.load(std::memory_order_acquire) ||
            point != detail::StreamIngressProbePoint::SlotLoad || index != probe.target) {
            return;
        }
        probe.entered.store(true, std::memory_order_release);
        while (!probe.release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

struct PointProbe {
    detail::StreamIngressProbePoint blocked = detail::StreamIngressProbePoint::Publication;
    std::size_t target = 0;
    std::atomic<bool> enabled{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<std::uint64_t> sequence{1};
    std::array<std::atomic<std::uint64_t>, 9> first_order{};

    static void notify(void* context, detail::StreamIngressProbePoint point,
                       std::size_t index) noexcept {
        auto& probe = *static_cast<PointProbe*>(context);
        const bool global = point == detail::StreamIngressProbePoint::PublisherIncrement ||
                            point == detail::StreamIngressProbePoint::PublisherDecrement;
        if (!global && index != probe.target) {
            return;
        }
        auto& order = probe.first_order.at(static_cast<std::size_t>(point));
        auto empty = std::uint64_t{0};
        static_cast<void>(order.compare_exchange_strong(
            empty, probe.sequence.fetch_add(1, std::memory_order_relaxed),
            std::memory_order_release, std::memory_order_relaxed));
        if (!probe.enabled.load(std::memory_order_acquire) || point != probe.blocked) {
            return;
        }
        probe.entered.store(true, std::memory_order_release);
        while (!probe.release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void reset(detail::StreamIngressProbePoint point, std::size_t index) noexcept {
        blocked = point;
        target = index;
        entered.store(false, std::memory_order_relaxed);
        release.store(false, std::memory_order_relaxed);
        enabled.store(false, std::memory_order_relaxed);
        sequence.store(1, std::memory_order_relaxed);
        for (auto& order : first_order) {
            order.store(0, std::memory_order_relaxed);
        }
    }
};

struct BorrowProbe {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    const StreamIngressFrontCursor* retained = nullptr;
};

struct ThrowProbe {
    const StreamIngressFrontCursor* retained = nullptr;
};

StreamIngressFrontAction block_front(void* context, const StreamIngressFrontCursor& cursor) {
    auto& probe = *static_cast<BorrowProbe*>(context);
    probe.retained = &cursor;
    probe.entered.store(true, std::memory_order_release);
    while (!probe.release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return StreamIngressFrontAction::Keep;
}

StreamIngressFrontAction throw_front(void* context, const StreamIngressFrontCursor& cursor) {
    static_cast<ThrowProbe*>(context)->retained = &cursor;
    throw std::runtime_error("front visitor failure");
}

StreamIngressFrontAction keep_front([[maybe_unused]] void* context,
                                    [[maybe_unused]] const StreamIngressFrontCursor& cursor) {
    return StreamIngressFrontAction::Keep;
}

void wait_entered(const BlockingProbe& probe) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!probe.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(probe.entered.load(std::memory_order_acquire));
}

void wait_entered(const PointProbe& probe) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!probe.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(probe.entered.load(std::memory_order_acquire));
}

} // namespace

static_assert(sizeof(tgcli::daemon::StreamIngressDescriptor) == 64);
static_assert(sizeof(tgcli::daemon::PublishedIngressFilter) == 552);
static_assert(sizeof(tgcli::daemon::StreamActivationProjection) == 552);
static_assert(!std::is_copy_constructible_v<tgcli::daemon::StreamIngressReservation>);
static_assert(std::is_move_constructible_v<tgcli::daemon::StreamIngressReservation>);
static_assert(!std::is_copy_constructible_v<tgcli::daemon::StreamIngressFrontCursor>);
static_assert(!std::is_move_constructible_v<tgcli::daemon::StreamIngressFrontCursor>);
static_assert(!std::is_copy_constructible_v<tgcli::daemon::StreamIngressBorrowedSpan>);
static_assert(!std::is_move_constructible_v<tgcli::daemon::StreamIngressBorrowedSpan>);
static_assert(std::variant_size_v<tgcli::daemon::StreamIngressAdmissionResult> == 3);
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
    auto reserved = reserve_slot(hub);
    CHECK(hub.activation_state(reserved) == StreamIngressState::Reserved);
    REQUIRE(hub.commit_activation(reserved));
    CHECK(hub.activation_state(reserved) == StreamIngressState::Armed);
    CHECK(hub.activate_armed(1001, 7, 41) == 1);
    CHECK(hub.activation_state(reserved) == StreamIngressState::Published);
    const auto projection = hub.activation_projection(reserved);
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
        const auto result = hub.reserve(request());
        const auto* failure = std::get_if<StreamIngressAdmissionFailure>(&result);
        REQUIRE(failure != nullptr);
        CHECK(failure->resource == StreamIngressAdmissionResource::LockFreeIngress);
        CHECK(failure->atomic == atomic);
    }
}

TEST_CASE("stream ingress admission returns one closed per-call result",
          "[stream][ingress][admission]") {
    StreamIngressHub hub;
    auto invalid_request = request();
    invalid_request.client_id = 0;
    CHECK(std::holds_alternative<StreamIngressInvalidRequest>(hub.reserve(invalid_request)));

    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots - 1; ++index) {
        reservations.push_back(reserve_slot(hub));
    }
    std::array<StreamIngressAdmissionResult, 2> results{StreamIngressInvalidRequest{},
                                                        StreamIngressInvalidRequest{}};
    std::atomic<bool> start{false};
    std::thread first([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        results[0] = hub.reserve(request());
    });
    std::thread second([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        results[1] = hub.reserve(request());
    });
    start.store(true, std::memory_order_release);
    first.join();
    second.join();
    const auto successes = static_cast<std::size_t>(
        std::count_if(results.begin(), results.end(), [](const auto& result) {
            return std::holds_alternative<StreamIngressReservation>(result);
        }));
    const auto failures = static_cast<std::size_t>(
        std::count_if(results.begin(), results.end(), [](const auto& result) {
            const auto* failure = std::get_if<StreamIngressAdmissionFailure>(&result);
            return failure != nullptr &&
                   failure->resource == StreamIngressAdmissionResource::SubscriberSlots;
        }));
    CHECK(successes == 1);
    CHECK(failures == 1);
}

TEST_CASE("stream ingress rejects stale reserved and invalid raw state fail closed",
          "[stream][ingress][activation][generation][capacity]") {
    StreamIngressHub hub;
    hub.begin_generation(1001, 7);
    auto stale = reserve_slot(hub);
    hub.begin_generation(1002, 8);
    CHECK_FALSE(hub.commit_activation(stale));
    const auto terminal = hub.claim_terminal(stale);
    REQUIRE(terminal);
    CHECK(terminal->cause == StreamTerminalCause::GenerationReplaced);
    CHECK(hub.activation_state(stale) == StreamIngressState::Removed);
    REQUIRE(hub.poll_reclaim(stale));

    auto malformed = reserve_slot(hub, request(1002, 8));
    StreamIngressTestAccess::set_state_raw(hub, malformed,
                                           std::numeric_limits<std::uint32_t>::max());
    CHECK(hub.activation_state(malformed) == StreamIngressState::Removed);
    REQUIRE(hub.detach(malformed));
    REQUIRE(hub.poll_reclaim(malformed));
}

TEST_CASE("installing marker cancellation forbids publication and reuse before X Z",
          "[stream][ingress][activation][reclaim][concurrency]") {
    PointProbe probe;
    StreamIngressHub hub(
        {.context = &probe, .hook = &PointProbe::notify, .forced_lock_free_failure = std::nullopt});
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto value = request(1001 + static_cast<std::int32_t>(index), index + 1);
        auto reserved = reserve_slot(hub, value);
        REQUIRE(hub.commit_activation(reserved));
        reservations.push_back(std::move(reserved));
    }
    constexpr std::size_t target = 17;
    probe.reset(detail::StreamIngressProbePoint::MarkerLoad, target);
    probe.enabled.store(true, std::memory_order_release);
    std::size_t activated = 1;
    std::thread activation([&] {
        activated = hub.activate_armed(1001 + static_cast<std::int32_t>(target), target + 1, 41);
    });
    wait_entered(probe);
    REQUIRE(StreamIngressTestAccess::publisher_count(hub) == 1);
    REQUIRE(hub.detach(reservations[target]));
    CHECK_FALSE(hub.poll_reclaim(reservations[target]));
    CHECK(std::holds_alternative<StreamIngressAdmissionFailure>(hub.reserve(request())));
    probe.release.store(true, std::memory_order_release);
    activation.join();
    CHECK(activated == 0);
    CHECK(probe.first_order[static_cast<std::size_t>(detail::StreamIngressProbePoint::MarkerLoad)]
              .load(std::memory_order_acquire) <
          probe.first_order[static_cast<std::size_t>(detail::StreamIngressProbePoint::SlotExchange)]
              .load(std::memory_order_acquire));
    CHECK(probe.first_order[static_cast<std::size_t>(detail::StreamIngressProbePoint::Publication)]
              .load(std::memory_order_acquire) == 0);
    REQUIRE(hub.poll_reclaim(reservations[target]));
    CHECK(std::holds_alternative<StreamIngressReservation>(hub.reserve(request())));
}

TEST_CASE("reservation destruction transfers unreclaimed slots to fixed retirement",
          "[stream][ingress][reclaim][raii]") {
    StreamIngressHub hub;
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        reservations.push_back(std::move(reserved));
    }
    REQUIRE(hub.activate_armed(1001, 7, 1) == kStreamSubscriberSlots);
    const auto reclaimed_index = StreamIngressTestAccess::slot_index(reservations.front());
    StreamIngressTestAccess::hold_publisher(hub, true);
    reservations.clear();
    CHECK(StreamIngressTestAccess::retired_count(hub) == kStreamSubscriberSlots);
    CHECK(std::holds_alternative<StreamIngressAdmissionFailure>(hub.reserve(request())));
    StreamIngressTestAccess::hold_publisher(hub, false);
    hub.poll_control();
    CHECK(StreamIngressTestAccess::retired_count(hub) == 0);
    CHECK(StreamIngressTestAccess::reclaimed_state_is_poisoned(hub, reclaimed_index));
    CHECK(std::holds_alternative<StreamIngressReservation>(hub.reserve(request())));
}

TEST_CASE("stream ingress queue preserves routing and two-span bytes", "[stream][ingress][queue]") {
    StreamIngressHub hub;
    auto reserved = reserve_slot(hub);
    REQUIRE(hub.commit_activation(reserved));
    REQUIRE(hub.activate_armed(1001, 7, 10) == 1);

    constexpr std::string_view line = "{\"event\":\"message\"}\n";
    hub.publish(item(line, 11));
    const auto front = read_front(hub, reserved, true);
    REQUIRE(front);
    CHECK(front->descriptor->receive_sequence == 11);
    CHECK(front->descriptor->chat_id == -1001);
    CHECK(front->descriptor->sender_id == 42);
    CHECK(front->descriptor->event_class == StreamEventClass::Message);
    CHECK(front->descriptor->message_offset == 11);
    CHECK(front->descriptor->text_offset == 16);
    CHECK(front->bytes == line);
    REQUIRE(front->retained_cursor != nullptr);
    CHECK_FALSE(front->retained_cursor->valid());
    REQUIRE(front->retained_first != nullptr);
    CHECK_FALSE(front->retained_first->size());
    CHECK_FALSE(read_front(hub, reserved));
}

TEST_CASE("front borrow invalidates on exception detach reclaim and reuse",
          "[stream][ingress][queue][borrow][concurrency]") {
    StreamIngressHub hub;
    auto reserved = reserve_slot(hub);
    REQUIRE(hub.commit_activation(reserved));
    REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
    hub.publish(item("front\n", 2));

    ThrowProbe thrown;
    CHECK_THROWS_AS(hub.visit_front(reserved, &thrown, &throw_front), std::runtime_error);
    REQUIRE(thrown.retained != nullptr);
    CHECK_FALSE(thrown.retained->valid());
    REQUIRE(read_front(hub, reserved));

    BorrowProbe probe;
    StreamIngressFrontResult result = StreamIngressFrontResult::Visited;
    std::thread worker([&] { result = hub.visit_front(reserved, &probe, &block_front); });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!probe.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(probe.entered.load(std::memory_order_acquire));
    CHECK(hub.visit_front(reserved, nullptr, &keep_front) == StreamIngressFrontResult::Invalidated);
    REQUIRE(hub.detach(reserved));
    CHECK_FALSE(hub.poll_reclaim(reserved));
    probe.release.store(true, std::memory_order_release);
    worker.join();
    CHECK(result == StreamIngressFrontResult::Invalidated);
    REQUIRE(probe.retained != nullptr);
    CHECK_FALSE(probe.retained->valid());
    const auto* const old_cursor = probe.retained;
    REQUIRE(hub.poll_reclaim(reserved));

    auto replacement = reserve_slot(hub);
    REQUIRE(hub.commit_activation(replacement));
    REQUIRE(hub.activate_armed(1001, 7, 3) == 1);
    hub.publish(item("replacement\n", 4));
    REQUIRE(read_front(hub, replacement));
    CHECK_FALSE(old_cursor->valid());
}

TEST_CASE("stream ingress byte ring wraps without changing logical order",
          "[stream][ingress][queue][wrap]") {
    StreamIngressHub hub;
    auto reserved = reserve_slot(hub);
    REQUIRE(hub.commit_activation(reserved));
    REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
    StreamIngressTestAccess::set_tickets(hub, reserved, 0, 0, kStreamQueueBytes - 2,
                                         kStreamQueueBytes - 2);
    constexpr std::string_view line = "abc\n";
    hub.publish(item(line, 2));
    const auto front = read_front(hub, reserved);
    REQUIRE(front);
    CHECK(front->span_sizes[0] == 2);
    CHECK(front->span_sizes[1] == 2);
    CHECK(front->bytes == line);
}

TEST_CASE("wrapped FIFO preserves chat sender and escaped UTF-8 message text offsets",
          "[stream][ingress][queue][wrap][routing]") {
    StreamIngressHub hub;
    auto reserved = reserve_slot(hub);
    REQUIRE(hub.commit_activation(reserved));
    REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
    StreamIngressTestAccess::set_tickets(hub, reserved, kStreamQueueItems - 1,
                                         kStreamQueueItems - 1, kStreamQueueBytes - 3,
                                         kStreamQueueBytes - 3);
    const std::string line = "{\"event\":\"message\",\"message\":{\"text\":\"a\\\\nΩ\"}}\n";
    auto sidecar = routing();
    sidecar.sender_kind = StreamSenderKind::Chat;
    sidecar.sender_id = -1009;
    sidecar.message_offset = static_cast<std::uint32_t>(line.find("{\"text\""));
    sidecar.message_size = static_cast<std::uint32_t>(line.find('}', sidecar.message_offset) -
                                                      sidecar.message_offset + 1);
    sidecar.text_offset = static_cast<std::uint32_t>(line.find("a\\\\nΩ"));
    sidecar.text_size = static_cast<std::uint32_t>(std::string_view{"a\\\\nΩ"}.size());
    hub.publish(item(line, 2, sidecar));

    const auto front = read_front(hub, reserved, true);
    REQUIRE(front);
    CHECK(front->span_sizes[0] == 3);
    CHECK(front->span_sizes[1] == line.size() - 3);
    CHECK(front->bytes == line);
    CHECK(front->descriptor->sender_kind == StreamSenderKind::Chat);
    CHECK(front->descriptor->sender_id == -1009);
    CHECK(front->bytes.substr(front->descriptor->message_offset, front->descriptor->message_size) ==
          line.substr(sidecar.message_offset, sidecar.message_size));
    CHECK(front->bytes.substr(front->descriptor->text_offset, front->descriptor->text_size) ==
          "a\\\\nΩ");
    hub.publish(item("next\n", 3));
    const auto reused = read_front(hub, reserved, true);
    REQUIRE(reused);
    CHECK(reused->descriptor->receive_sequence == 3);
    CHECK(reused->bytes == "next\n");
}

TEST_CASE("stream ingress applies immutable type chat generation and anchor filters",
          "[stream][ingress][filter]") {
    StreamIngressHub hub;
    auto reserved = reserve_slot(hub);
    REQUIRE(hub.commit_activation(reserved));
    REQUIRE(hub.activate_armed(1001, 7, 10) == 1);
    constexpr std::string_view line = "{}\n";
    hub.publish(item(line, 10));
    hub.publish(item(line, 11, routing(StreamEventClass::Delete)));
    hub.publish(item(line, 12, routing(StreamEventClass::Message, -2000)));
    CHECK_FALSE(read_front(hub, reserved));
    hub.publish(item(line, 13));
    const auto front = read_front(hub, reserved);
    REQUIRE(front);
    CHECK(front->descriptor->receive_sequence == 13);
}

TEST_CASE("stream ingress admission is exact and waits for exchange plus quiescence",
          "[stream][ingress][capacity][reclaim]") {
    StreamIngressHub hub;
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        reservations.push_back(std::move(reserved));
    }
    const auto full = hub.reserve(request());
    const auto* failure = std::get_if<StreamIngressAdmissionFailure>(&full);
    REQUIRE(failure != nullptr);
    CHECK(failure->resource == StreamIngressAdmissionResource::SubscriberSlots);

    REQUIRE(hub.activate_armed(1001, 7, 4) == kStreamSubscriberSlots);
    StreamIngressTestAccess::hold_publisher(hub, true);
    REQUIRE(hub.detach(reservations.front()));
    CHECK_FALSE(hub.poll_reclaim(reservations.front()));
    CHECK(std::holds_alternative<StreamIngressAdmissionFailure>(hub.reserve(request())));
    StreamIngressTestAccess::hold_publisher(hub, false);
    REQUIRE(hub.poll_reclaim(reservations.front()));
    CHECK(std::holds_alternative<StreamIngressReservation>(hub.reserve(request())));
}

TEST_CASE("stream ingress removal waits for a publisher that loaded the slot",
          "[stream][ingress][reclaim][concurrency]") {
    BlockingProbe probe;
    StreamIngressHub hub({.context = &probe,
                          .hook = &BlockingProbe::notify,
                          .forced_lock_free_failure = std::nullopt});
    auto reserved = reserve_slot(hub);
    REQUIRE(hub.commit_activation(reserved));
    REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
    constexpr std::string_view line = "{}\n";
    auto value = item(line, 2);
    probe.enabled.store(true, std::memory_order_release);
    std::thread publisher([&] { hub.publish(value); });
    wait_entered(probe);
    REQUIRE(hub.detach(reserved));
    CHECK_FALSE(hub.poll_reclaim(reserved));
    probe.release.store(true, std::memory_order_release);
    publisher.join();
    REQUIRE(hub.poll_reclaim(reserved));
    CHECK(std::holds_alternative<StreamIngressReservation>(hub.reserve(request())));
}

TEST_CASE("stream ingress proves P I L D X Z for every registry slot",
          "[stream][ingress][reclaim][concurrency]") {
    PointProbe probe;
    StreamIngressHub hub(
        {.context = &probe, .hook = &PointProbe::notify, .forced_lock_free_failure = std::nullopt});
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        reservations.push_back(std::move(reserved));
    }
    REQUIRE(hub.activate_armed(1001, 7, 1) == kStreamSubscriberSlots);
    constexpr std::string_view line = "{}\n";
    auto value = item(line, 2);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        probe.reset(detail::StreamIngressProbePoint::SlotLoad, index);
        probe.enabled.store(true, std::memory_order_release);
        std::thread publisher([&] { hub.publish(value); });
        wait_entered(probe);
        REQUIRE(hub.detach(reservations[index]));
        CHECK_FALSE(hub.poll_reclaim(reservations[index]));
        probe.release.store(true, std::memory_order_release);
        publisher.join();
        const auto load =
            probe.first_order[static_cast<std::size_t>(detail::StreamIngressProbePoint::SlotLoad)]
                .load(std::memory_order_acquire);
        const auto exchange = probe
                                  .first_order[static_cast<std::size_t>(
                                      detail::StreamIngressProbePoint::SlotExchange)]
                                  .load(std::memory_order_acquire);
        const auto reclaim =
            probe
                .first_order[static_cast<std::size_t>(detail::StreamIngressProbePoint::ReclaimLoad)]
                .load(std::memory_order_acquire);
        const auto decrement = probe
                                   .first_order[static_cast<std::size_t>(
                                       detail::StreamIngressProbePoint::PublisherDecrement)]
                                   .load(std::memory_order_acquire);
        CHECK(load < exchange);
        CHECK(exchange < reclaim);
        CHECK(reclaim < decrement);
        REQUIRE(hub.poll_reclaim(reservations[index]));
    }
}

TEST_CASE("stream ingress forces every null-load and publication order for all slots",
          "[stream][ingress][reclaim][concurrency]") {
    PointProbe probe;
    StreamIngressHub hub(
        {.context = &probe, .hook = &PointProbe::notify, .forced_lock_free_failure = std::nullopt});
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        reservations.push_back(std::move(reserved));
    }
    REQUIRE(hub.activate_armed(1001, 7, 1) == kStreamSubscriberSlots);
    constexpr std::string_view line = "{}\n";
    auto value = item(line, 2);
    const auto order = [&](detail::StreamIngressProbePoint point) {
        return probe.first_order.at(static_cast<std::size_t>(point))
            .load(std::memory_order_acquire);
    };
    const auto restore = [&](std::size_t index) {
        REQUIRE(hub.poll_reclaim(reservations[index]));
        auto replacement = reserve_slot(hub);
        REQUIRE(StreamIngressTestAccess::slot_index(replacement) == index);
        REQUIRE(hub.commit_activation(replacement));
        REQUIRE(hub.activate_armed(1001, 7, 2) == 1);
        reservations[index] = std::move(replacement);
    };

    SECTION("D before X") {
        for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
            probe.reset(detail::StreamIngressProbePoint::Publication, index);
            hub.publish(value);
            REQUIRE(hub.detach(reservations[index]));
            CHECK(order(detail::StreamIngressProbePoint::PublisherDecrement) <
                  order(detail::StreamIngressProbePoint::SlotExchange));
            restore(index);
        }
    }

    SECTION("X before I and null L") {
        for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
            probe.reset(detail::StreamIngressProbePoint::Publication, index);
            REQUIRE(hub.detach(reservations[index]));
            hub.publish(value);
            CHECK(order(detail::StreamIngressProbePoint::SlotExchange) <
                  order(detail::StreamIngressProbePoint::PublisherIncrement));
            CHECK(order(detail::StreamIngressProbePoint::PublisherIncrement) <
                  order(detail::StreamIngressProbePoint::SlotLoad));
            restore(index);
        }
    }

    SECTION("X before null L while I is held") {
        for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
            probe.reset(detail::StreamIngressProbePoint::PublisherIncrement, index);
            probe.enabled.store(true, std::memory_order_release);
            std::thread publisher([&] { hub.publish(value); });
            wait_entered(probe);
            REQUIRE(hub.detach(reservations[index]));
            probe.release.store(true, std::memory_order_release);
            publisher.join();
            CHECK(order(detail::StreamIngressProbePoint::PublisherIncrement) <
                  order(detail::StreamIngressProbePoint::SlotExchange));
            CHECK(order(detail::StreamIngressProbePoint::SlotExchange) <
                  order(detail::StreamIngressProbePoint::SlotLoad));
            restore(index);
        }
    }

    SECTION("null L before D and replacement P only after X Z") {
        for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
            probe.reset(detail::StreamIngressProbePoint::Publication, index);
            REQUIRE(hub.detach(reservations[index]));
            const auto before_reclaim = hub.reserve(request());
            REQUIRE(std::holds_alternative<StreamIngressAdmissionFailure>(before_reclaim));
            hub.publish(value);
            REQUIRE(hub.poll_reclaim(reservations[index]));
            auto replacement = reserve_slot(hub);
            REQUIRE(StreamIngressTestAccess::slot_index(replacement) == index);
            REQUIRE(hub.commit_activation(replacement));
            REQUIRE(hub.activate_armed(1001, 7, 2) == 1);
            CHECK(order(detail::StreamIngressProbePoint::SlotLoad) <
                  order(detail::StreamIngressProbePoint::PublisherDecrement));
            CHECK(order(detail::StreamIngressProbePoint::PublisherDecrement) <
                  order(detail::StreamIngressProbePoint::Publication));
            CHECK(order(detail::StreamIngressProbePoint::SlotExchange) <
                  order(detail::StreamIngressProbePoint::ReclaimZero));
            CHECK(order(detail::StreamIngressProbePoint::ReclaimZero) <
                  order(detail::StreamIngressProbePoint::Publication));
            reservations[index] = std::move(replacement);
        }
    }
}

TEST_CASE("stream ingress queue failures use exact precedence and first cause",
          "[stream][ingress][overflow]") {
    constexpr std::string_view line = "{}\n";
    SECTION("item bytes precedes item and byte occupancy") {
        StreamIngressHub hub;
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, reserved, kStreamQueueItems, 0, kStreamQueueBytes,
                                             0);
        const std::string oversized(kStreamQueueItemBytes + 1, 'x');
        hub.publish(item(oversized, 2));
        const auto terminal = hub.claim_terminal(reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::ItemBytes);
        CHECK(terminal->incoming_bytes == kStreamQueueItemBytes + 1);
    }
    SECTION("descriptor count precedes byte capacity") {
        StreamIngressHub hub;
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, reserved, kStreamQueueItems, 0, kStreamQueueBytes,
                                             0);
        hub.publish(item(line, 2));
        const auto terminal = hub.claim_terminal(reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::QueueItems);
    }
    SECTION("byte capacity is exact") {
        StreamIngressHub hub;
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        const std::string maximum(kStreamQueueItemBytes, 'x');
        for (std::size_t index = 0; index < kStreamQueueBytes / kStreamQueueItemBytes; ++index) {
            hub.publish(item(maximum, index + 2));
        }
        CHECK_FALSE(hub.terminal_snapshot(reserved));
        hub.publish(item(line, 40));
        const auto terminal = hub.claim_terminal(reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::QueueBytes);
        CHECK(terminal->queued_bytes == kStreamQueueBytes);
    }
    SECTION("descriptor counter exhaustion wins before size") {
        StreamIngressHub hub;
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, reserved,
                                             std::numeric_limits<std::uint64_t>::max(),
                                             std::numeric_limits<std::uint64_t>::max(), 0, 0);
        const std::string oversized(kStreamQueueItemBytes + 1, 'x');
        hub.publish(item(oversized, 2));
        const auto terminal = hub.claim_terminal(reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::CounterExhausted);
    }
    SECTION("byte counter exhaustion wins before capacity") {
        StreamIngressHub hub;
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        StreamIngressTestAccess::set_tickets(hub, reserved, 0, 0,
                                             std::numeric_limits<std::uint64_t>::max() - 1,
                                             std::numeric_limits<std::uint64_t>::max() - 1);
        hub.publish(item(line, 2));
        const auto terminal = hub.claim_terminal(reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::CounterExhausted);
    }
    SECTION("first terminal payload is retained") {
        StreamIngressHub hub;
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.claim(reserved, {.cause = StreamTerminalCause::Shutdown,
                                     .operation = StreamOperation::Listen,
                                     .metadata_failure = {}}));
        CHECK_FALSE(hub.claim(reserved, {.cause = StreamTerminalCause::AuthorizationLost,
                                         .operation = StreamOperation::WaitFor,
                                         .auth_state = 9,
                                         .metadata_failure = {}}));
        const auto terminal = hub.claim_terminal(reserved);
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::Shutdown);
        CHECK(terminal->operation == StreamOperation::Listen);
    }
    SECTION("concurrent first terminal retains one complete payload") {
        StreamIngressHub hub;
        auto reserved = reserve_slot(hub);
        std::array<bool, 2> won{};
        std::atomic<bool> start{false};
        std::thread first([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            won[0] = hub.claim(reserved, {.cause = StreamTerminalCause::AuthorizationLost,
                                          .operation = StreamOperation::Listen,
                                          .auth_state = 11,
                                          .metadata_failure = {}});
        });
        std::thread second([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            won[1] = hub.claim(reserved, {.cause = StreamTerminalCause::Shutdown,
                                          .operation = StreamOperation::Listen,
                                          .auth_state = 22,
                                          .metadata_failure = {}});
        });
        start.store(true, std::memory_order_release);
        first.join();
        second.join();
        CHECK(won[0] != won[1]);
        const auto terminal = hub.claim_terminal(reserved);
        REQUIRE(terminal);
        if (won[0]) {
            CHECK(terminal->cause == StreamTerminalCause::AuthorizationLost);
            CHECK(terminal->auth_state == 11);
        } else {
            CHECK(terminal->cause == StreamTerminalCause::Shutdown);
            CHECK(terminal->auth_state == 22);
        }
    }
}

TEST_CASE("one full ingress queue does not alter another subscriber",
          "[stream][ingress][isolation]") {
    StreamIngressHub hub;
    std::vector<StreamIngressReservation> reservations;
    reservations.reserve(kStreamSubscriberSlots);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        reservations.push_back(std::move(reserved));
    }
    REQUIRE(hub.activate_armed(1001, 7, 1) == kStreamSubscriberSlots);
    constexpr std::string_view line = "{}\n";
    StreamIngressTestAccess::set_tickets(hub, reservations.front(), kStreamQueueItems, 0, 0, 0);
    hub.publish(item(line, 2));
    REQUIRE(hub.claim_terminal(reservations.front()));
    CHECK(hub.terminal_snapshot(reservations.front())->cause == StreamTerminalCause::QueueItems);
    for (std::size_t index = 1; index < reservations.size(); ++index) {
        const auto front = read_front(hub, reservations[index]);
        REQUIRE(front);
        CHECK(front->bytes == line);
    }
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

    StreamPollSchedule at_maximum(Clock::time_point::max());
    CHECK(at_maximum.next(std::nullopt) == Clock::time_point::max());
    at_maximum.advance(Clock::time_point::max());
    CHECK(at_maximum.next(std::nullopt) == Clock::time_point::max());

    StreamPollSchedule adjacent(Clock::time_point::max() - 4ms);
    CHECK(adjacent.next(std::nullopt) == Clock::time_point::max() - 2ms);
    adjacent.advance(Clock::time_point::max() - 2ms);
    CHECK(adjacent.next(std::nullopt) == Clock::time_point::max());
    adjacent.advance(Clock::time_point::max());
    CHECK(adjacent.next(std::nullopt) == Clock::time_point::max());

    const StreamPollSchedule saturated(Clock::time_point::max() - 1ms);
    CHECK(saturated.next(std::nullopt) == Clock::time_point::max());
    CHECK(saturated.next(Clock::time_point::min()) == Clock::time_point::min());

    StreamPollSchedule from_minimum(Clock::time_point::min());
    CHECK(from_minimum.next(std::nullopt) == Clock::time_point::min() + 2ms);
    from_minimum.advance(Clock::time_point::max());
    CHECK(from_minimum.next(std::nullopt) == Clock::time_point::max());
}

TEST_CASE("stream ingress preallocated slot survives one million concurrent ABA-safe cycles",
          "[stream][ingress][stress]") {
    StreamIngressHub hub;
    constexpr std::string_view line = "{}\n";
    auto value = item(line, 2);
    std::atomic<std::size_t> requested{0};
    std::atomic<std::size_t> completed{0};
    std::thread publisher([&] {
        for (std::size_t cycle = 1; cycle <= 1'000'000; ++cycle) {
            while (requested.load(std::memory_order_acquire) < cycle) {
                std::this_thread::yield();
            }
            hub.publish(value);
            completed.store(cycle, std::memory_order_release);
        }
    });
    for (std::size_t cycle = 1; cycle <= 1'000'000; ++cycle) {
        auto reserved = reserve_slot(hub);
        REQUIRE(hub.commit_activation(reserved));
        REQUIRE(hub.activate_armed(1001, 7, 1) == 1);
        requested.store(cycle, std::memory_order_release);
        while (completed.load(std::memory_order_acquire) < cycle) {
            std::this_thread::yield();
        }
        REQUIRE(read_front(hub, reserved, true));
        REQUIRE(hub.detach(reserved));
        REQUIRE(hub.poll_reclaim(reserved));
    }
    publisher.join();
}
