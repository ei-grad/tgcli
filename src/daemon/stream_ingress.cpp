#include "daemon/stream_ingress.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace tgcli::daemon {

// Fixed rings use validated monotonic tickets and checked modulo indices on the callback path.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)

namespace {

constexpr unsigned char kReclaimedPoison = 0xA7;
constexpr auto kStateMaximum = static_cast<std::uint32_t>(StreamIngressState::Reclaimable);

enum class ActivationCommit : std::uint32_t {
    Open,
    Prepared,
    TerminalClaiming,
    Cancelled,
    PromotionCommitted
};

StreamTerminalCause claiming_cause(StreamTerminalCause cause) noexcept {
    switch (cause) {
    case StreamTerminalCause::CounterExhausted:
        return StreamTerminalCause::ClaimingCounterExhausted;
    case StreamTerminalCause::ItemBytes:
        return StreamTerminalCause::ClaimingItemBytes;
    case StreamTerminalCause::QueueItems:
        return StreamTerminalCause::ClaimingQueueItems;
    case StreamTerminalCause::QueueBytes:
        return StreamTerminalCause::ClaimingQueueBytes;
    case StreamTerminalCause::AuthorizationLost:
        return StreamTerminalCause::ClaimingAuthorizationLost;
    case StreamTerminalCause::GenerationReplaced:
        return StreamTerminalCause::ClaimingGenerationReplaced;
    case StreamTerminalCause::Shutdown:
        return StreamTerminalCause::ClaimingShutdown;
    case StreamTerminalCause::MetadataFailure:
        return StreamTerminalCause::ClaimingMetadataFailure;
    case StreamTerminalCause::PlannedSuccess:
        return StreamTerminalCause::ClaimingPlannedSuccess;
    case StreamTerminalCause::Deadline:
        return StreamTerminalCause::ClaimingDeadline;
    case StreamTerminalCause::Disconnected:
        return StreamTerminalCause::ClaimingDisconnected;
    case StreamTerminalCause::Open:
    case StreamTerminalCause::ClaimingCounterExhausted:
    case StreamTerminalCause::ClaimingItemBytes:
    case StreamTerminalCause::ClaimingQueueItems:
    case StreamTerminalCause::ClaimingQueueBytes:
    case StreamTerminalCause::ClaimingAuthorizationLost:
    case StreamTerminalCause::ClaimingGenerationReplaced:
    case StreamTerminalCause::ClaimingShutdown:
    case StreamTerminalCause::ClaimingMetadataFailure:
    case StreamTerminalCause::ClaimingPlannedSuccess:
    case StreamTerminalCause::ClaimingDeadline:
    case StreamTerminalCause::ClaimingDisconnected:
        return StreamTerminalCause::Open;
    }
    return StreamTerminalCause::Open;
}

bool ready_cause(StreamTerminalCause cause) noexcept {
    return cause >= StreamTerminalCause::CounterExhausted;
}

bool valid_request(const StreamIngressRequest& request) noexcept {
    if (request.client_id <= 0 || request.generation == 0 || request.type_mask == 0 ||
        request.chat_count > kStreamChatFilters) {
        return false;
    }
    return std::adjacent_find(
               request.chat_ids.begin(), request.chat_ids.begin() + request.chat_count,
               std::greater_equal<>()) == request.chat_ids.begin() + request.chat_count;
}

std::optional<StreamIngressState> decode_state(std::uint32_t raw) noexcept {
    if (raw > kStateMaximum) {
        return std::nullopt;
    }
    return static_cast<StreamIngressState>(raw);
}

template <typename Value> void poison(Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    auto bytes = std::as_writable_bytes(std::span{&value, std::size_t{1}});
    std::ranges::fill(bytes, static_cast<std::byte>(kReclaimedPoison));
}

} // namespace

struct StreamIngressSlot {
    using DescriptorStorage = std::array<StreamIngressDescriptor, kStreamQueueItems>;
    using ByteStorage = std::array<char, kStreamQueueBytes>;

    std::atomic<std::uint32_t> state{static_cast<std::uint32_t>(StreamIngressState::Free)};
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> borrow_epoch{0};
    std::atomic<std::uint32_t> active_borrows{0};
    std::atomic<std::uint32_t> publication_ready{0};
    std::atomic<std::uint32_t> activation_commit{
        static_cast<std::uint32_t>(ActivationCommit::Open)};
    std::atomic<std::int32_t> owner_client_id{0};
    std::atomic<std::uint64_t> owner_generation{0};
    std::atomic<std::uint32_t> owner_operation{static_cast<std::uint32_t>(StreamOperation::Listen)};
    StreamIngressRequest staged;
    PublishedIngressFilter projection;
    std::unique_ptr<DescriptorStorage> descriptors;
    std::unique_ptr<ByteStorage> bytes;
    std::atomic<std::uint64_t> descriptor_producer{0};
    std::atomic<std::uint64_t> descriptor_consumer{0};
    std::atomic<std::uint64_t> byte_producer{0};
    std::atomic<std::uint64_t> byte_consumer{0};
    std::atomic<StreamTerminalCause> terminal_cause{StreamTerminalCause::Open};
    StreamTerminalPayload terminal_payload;
    bool terminal_delivered = false;
};

struct RetainedGenerationTerminal {
    std::atomic<StreamTerminalCause> cause{StreamTerminalCause::Open};
    std::atomic<std::int32_t> client_id{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::int32_t> auth_state{0};
    std::atomic<std::uint32_t> failure_kind{0};
    std::atomic<std::uint32_t> update_kind{0};
    std::atomic<std::uint32_t> malformed_reason{0};
    std::atomic<std::int32_t> tdlib_type_id{0};
    std::atomic<std::int32_t> tdlib_error_code{0};
    std::atomic<std::int32_t> retry_after{0};
    std::atomic<std::uint32_t> current_state_index{0};
    std::atomic<std::uint32_t> capacity_resource{0};
    std::atomic<std::uint32_t> capacity_phase{0};
    std::atomic<std::uint64_t> capacity_limit{0};
    std::atomic<std::uint64_t> capacity_used{0};
    std::atomic<std::uint64_t> capacity_incoming{0};
    std::atomic<std::uint64_t> capacity_would_use{0};

    void reset(std::int32_t next_client_id, std::uint64_t next_generation) noexcept {
        static_cast<void>(cause.exchange(StreamTerminalCause::ClaimingGenerationReplaced,
                                         std::memory_order_acq_rel));
        client_id.store(next_client_id, std::memory_order_relaxed);
        generation.store(next_generation, std::memory_order_relaxed);
        auth_state.store(0, std::memory_order_relaxed);
        failure_kind.store(static_cast<std::uint32_t>(StreamFailureKind::None),
                           std::memory_order_relaxed);
        update_kind.store(
            static_cast<std::uint32_t>(core::TdSupportedUpdateKind::CurrentStateEntry),
            std::memory_order_relaxed);
        malformed_reason.store(
            static_cast<std::uint32_t>(core::TdMalformedUpdateReason::MissingObject),
            std::memory_order_relaxed);
        tdlib_type_id.store(0, std::memory_order_relaxed);
        tdlib_error_code.store(0, std::memory_order_relaxed);
        retry_after.store(0, std::memory_order_relaxed);
        current_state_index.store(0, std::memory_order_relaxed);
        capacity_resource.store(static_cast<std::uint32_t>(StreamMetadataResource::BootstrapItems),
                                std::memory_order_relaxed);
        capacity_phase.store(static_cast<std::uint32_t>(StreamMetadataPhase::Bootstrap),
                             std::memory_order_relaxed);
        capacity_limit.store(0, std::memory_order_relaxed);
        capacity_used.store(0, std::memory_order_relaxed);
        capacity_incoming.store(0, std::memory_order_relaxed);
        capacity_would_use.store(0, std::memory_order_relaxed);
        cause.store(StreamTerminalCause::Open, std::memory_order_release);
    }

    bool publish(std::int32_t expected_client_id, std::uint64_t expected_generation,
                 const StreamTerminalPayload& payload) noexcept {
        if (client_id.load(std::memory_order_acquire) != expected_client_id ||
            generation.load(std::memory_order_acquire) != expected_generation) {
            return false;
        }
        const auto claiming = claiming_cause(payload.cause);
        if (claiming == StreamTerminalCause::Open) {
            return false;
        }
        auto expected = StreamTerminalCause::Open;
        if (!cause.compare_exchange_strong(expected, claiming, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return false;
        }
        auth_state.store(payload.auth_state, std::memory_order_relaxed);
        failure_kind.store(static_cast<std::uint32_t>(payload.metadata_failure.kind),
                           std::memory_order_relaxed);
        update_kind.store(static_cast<std::uint32_t>(payload.metadata_failure.update_kind),
                          std::memory_order_relaxed);
        malformed_reason.store(
            static_cast<std::uint32_t>(payload.metadata_failure.malformed_reason),
            std::memory_order_relaxed);
        tdlib_type_id.store(payload.metadata_failure.tdlib_type_id, std::memory_order_relaxed);
        tdlib_error_code.store(payload.metadata_failure.tdlib_error_code,
                               std::memory_order_relaxed);
        retry_after.store(payload.metadata_failure.retry_after, std::memory_order_relaxed);
        current_state_index.store(payload.metadata_failure.current_state_index,
                                  std::memory_order_relaxed);
        capacity_resource.store(
            static_cast<std::uint32_t>(payload.metadata_failure.capacity.resource),
            std::memory_order_relaxed);
        capacity_phase.store(static_cast<std::uint32_t>(payload.metadata_failure.capacity.phase),
                             std::memory_order_relaxed);
        capacity_limit.store(payload.metadata_failure.capacity.limit, std::memory_order_relaxed);
        capacity_used.store(payload.metadata_failure.capacity.used, std::memory_order_relaxed);
        capacity_incoming.store(payload.metadata_failure.capacity.incoming,
                                std::memory_order_relaxed);
        capacity_would_use.store(payload.metadata_failure.capacity.would_use,
                                 std::memory_order_relaxed);
        auto expected_claiming = claiming;
        return cause.compare_exchange_strong(expected_claiming, payload.cause,
                                             std::memory_order_release, std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<StreamTerminalPayload>
    snapshot(std::int32_t expected_client_id, std::uint64_t expected_generation,
             StreamOperation operation) const noexcept {
        for (;;) {
            const auto before = cause.load(std::memory_order_acquire);
            if (before == StreamTerminalCause::Open) {
                return std::nullopt;
            }
            if (!ready_cause(before)) {
                continue;
            }
            const auto observed_client_id = client_id.load(std::memory_order_relaxed);
            const auto observed_generation = generation.load(std::memory_order_relaxed);
            StreamTerminalPayload payload{
                .cause = before,
                .operation = operation,
                .auth_state = auth_state.load(std::memory_order_relaxed),
                .metadata_failure = {
                    .kind = static_cast<StreamFailureKind>(
                        failure_kind.load(std::memory_order_relaxed)),
                    .update_kind = static_cast<core::TdSupportedUpdateKind>(
                        update_kind.load(std::memory_order_relaxed)),
                    .malformed_reason = static_cast<core::TdMalformedUpdateReason>(
                        malformed_reason.load(std::memory_order_relaxed)),
                    .tdlib_type_id = tdlib_type_id.load(std::memory_order_relaxed),
                    .tdlib_error_code = tdlib_error_code.load(std::memory_order_relaxed),
                    .retry_after = retry_after.load(std::memory_order_relaxed),
                    .current_state_index = current_state_index.load(std::memory_order_relaxed),
                    .capacity = {.resource = static_cast<StreamMetadataResource>(
                                     capacity_resource.load(std::memory_order_relaxed)),
                                 .phase = static_cast<StreamMetadataPhase>(
                                     capacity_phase.load(std::memory_order_relaxed)),
                                 .limit = capacity_limit.load(std::memory_order_relaxed),
                                 .used = capacity_used.load(std::memory_order_relaxed),
                                 .incoming = capacity_incoming.load(std::memory_order_relaxed),
                                 .would_use = capacity_would_use.load(std::memory_order_relaxed)}}};
            if (cause.load(std::memory_order_acquire) != before) {
                continue;
            }
            if (observed_client_id != expected_client_id ||
                observed_generation != expected_generation) {
                return std::nullopt;
            }
            return payload;
        }
    }
};

static_assert(std::atomic<StreamIngressSlot*>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<StreamTerminalCause>::is_always_lock_free);

namespace {

StreamIngressState load_state(const StreamIngressSlot& slot,
                              std::memory_order order = std::memory_order_acquire) noexcept {
    return decode_state(slot.state.load(order)).value_or(StreamIngressState::Removed);
}

void store_state(StreamIngressSlot& slot, StreamIngressState state,
                 std::memory_order order = std::memory_order_release) noexcept {
    slot.state.store(static_cast<std::uint32_t>(state), order);
}

bool compare_state(StreamIngressSlot& slot, StreamIngressState& expected,
                   StreamIngressState desired) noexcept {
    auto raw = static_cast<std::uint32_t>(expected);
    const bool changed =
        slot.state.compare_exchange_weak(raw, static_cast<std::uint32_t>(desired),
                                         std::memory_order_acq_rel, std::memory_order_acquire);
    expected = decode_state(raw).value_or(StreamIngressState::Removed);
    return changed;
}

bool compare_state_strong(StreamIngressSlot& slot, StreamIngressState expected,
                          StreamIngressState desired) noexcept {
    auto raw = static_cast<std::uint32_t>(expected);
    return slot.state.compare_exchange_strong(raw, static_cast<std::uint32_t>(desired),
                                              std::memory_order_acq_rel, std::memory_order_acquire);
}

bool advance_borrow_epoch(StreamIngressSlot& slot, std::uint64_t expected,
                          std::uint64_t desired) noexcept {
    return slot.borrow_epoch.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
}

void invalidate_borrow(StreamIngressSlot& slot) noexcept {
    auto current = slot.borrow_epoch.load(std::memory_order_acquire);
    while (current != std::numeric_limits<std::uint64_t>::max()) {
        const auto increment = (current & 1U) != 0U ? 1U : 2U;
        if (current > std::numeric_limits<std::uint64_t>::max() - increment) {
            return;
        }
        if (slot.borrow_epoch.compare_exchange_weak(current, current + increment,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return;
        }
    }
}

} // namespace

class StreamIngressHub::Impl {
  public:
    explicit Impl(detail::StreamIngressProbe probe_value) : probe(probe_value) {
        for (auto& pointer : dormant) {
            pointer.store(nullptr, std::memory_order_relaxed);
        }
        for (auto& pointer : published) {
            pointer.store(nullptr, std::memory_order_relaxed);
        }
        const std::atomic<StreamIngressSlot*> pointer;
        if (!pointer.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::SlotPointer;
        } else if (!publisher_count.is_lock_free() || !current_client_id.is_lock_free() ||
                   !slots.front().owner_client_id.is_lock_free() ||
                   !retained_terminal.client_id.is_lock_free() ||
                   !retained_terminal.auth_state.is_lock_free() ||
                   !retained_terminal.tdlib_type_id.is_lock_free() ||
                   !retained_terminal.tdlib_error_code.is_lock_free() ||
                   !retained_terminal.retry_after.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::PublisherCount;
        } else if (!slots.front().state.is_lock_free() ||
                   !slots.front().descriptor_producer.is_lock_free() ||
                   !slots.front().descriptor_consumer.is_lock_free() ||
                   !slots.front().borrow_epoch.is_lock_free() ||
                   !slots.front().active_borrows.is_lock_free() ||
                   !slots.front().publication_ready.is_lock_free() ||
                   !slots.front().activation_commit.is_lock_free() ||
                   !slots.front().owner_generation.is_lock_free() ||
                   !current_generation.is_lock_free() ||
                   !retained_terminal.generation.is_lock_free() ||
                   !retained_terminal.failure_kind.is_lock_free() ||
                   !retained_terminal.update_kind.is_lock_free() ||
                   !retained_terminal.malformed_reason.is_lock_free() ||
                   !retained_terminal.current_state_index.is_lock_free() ||
                   !retained_terminal.capacity_resource.is_lock_free() ||
                   !retained_terminal.capacity_phase.is_lock_free() ||
                   !retained_terminal.capacity_limit.is_lock_free() ||
                   !retained_terminal.capacity_used.is_lock_free() ||
                   !retained_terminal.capacity_incoming.is_lock_free() ||
                   !retained_terminal.capacity_would_use.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::DescriptorIndex;
        } else if (!slots.front().byte_producer.is_lock_free() ||
                   !slots.front().byte_consumer.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::ByteIndex;
        } else if (!slots.front().terminal_cause.is_lock_free() ||
                   !retained_terminal.cause.is_lock_free() ||
                   !slots.front().owner_operation.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::TerminalCause;
        }
        if (probe.forced_lock_free_failure) {
            lock_free_failure = probe.forced_lock_free_failure;
        }
    }

    void notify(detail::StreamIngressProbePoint point, std::size_t index = 0) const noexcept {
        if (probe.hook != nullptr) {
            probe.hook(probe.context, point, index);
        }
    }

    class PublisherGuard {
      public:
        explicit PublisherGuard(Impl& owner) noexcept : owner_(owner) {
            const auto previous = owner_.publisher_count.fetch_add(1, std::memory_order_seq_cst);
            if (previous != 0) {
                owner_.publisher_count.fetch_sub(1, std::memory_order_seq_cst);
                active_ = false;
                return;
            }
            owner_.notify(detail::StreamIngressProbePoint::PublisherIncrement);
        }

        ~PublisherGuard() {
            if (active_) {
                owner_.notify(detail::StreamIngressProbePoint::PublisherDecrement);
                owner_.publisher_count.fetch_sub(1, std::memory_order_seq_cst);
            }
        }

        PublisherGuard(const PublisherGuard&) = delete;
        PublisherGuard& operator=(const PublisherGuard&) = delete;
        PublisherGuard(PublisherGuard&&) = delete;
        PublisherGuard& operator=(PublisherGuard&&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept {
            return active_;
        }

      private:
        Impl& owner_;
        bool active_ = true;
    };

    [[nodiscard]] bool valid(const StreamIngressReservation& reservation) const noexcept {
        return reservation.hub_ != nullptr && reservation.index_ < kStreamSubscriberSlots &&
               slots[reservation.index_].epoch.load(std::memory_order_acquire) ==
                   reservation.epoch_;
    }

    [[nodiscard]] bool marker(std::size_t index, const StreamIngressSlot* pointer) const noexcept {
        return pointer == &installing_markers[index];
    }

    [[nodiscard]] bool actual(std::size_t index, const StreamIngressSlot* pointer) const noexcept {
        return pointer != nullptr && !marker(index, pointer);
    }

    static bool claim(StreamIngressSlot& slot, StreamTerminalPayload payload) noexcept {
        const auto claiming = claiming_cause(payload.cause);
        if (claiming == StreamTerminalCause::Open) {
            return false;
        }
        auto activation = slot.activation_commit.load(std::memory_order_acquire);
        bool activation_claimed = false;
        while (activation == static_cast<std::uint32_t>(ActivationCommit::Open) ||
               activation == static_cast<std::uint32_t>(ActivationCommit::Prepared)) {
            if (slot.activation_commit.compare_exchange_weak(
                    activation, static_cast<std::uint32_t>(ActivationCommit::TerminalClaiming),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                activation_claimed = true;
                break;
            }
        }
        auto expected = StreamTerminalCause::Open;
        if (!slot.terminal_cause.compare_exchange_strong(
                expected, claiming, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (activation_claimed) {
                slot.activation_commit.store(
                    static_cast<std::uint32_t>(ActivationCommit::Cancelled),
                    std::memory_order_release);
            }
            return false;
        }
        slot.terminal_payload = payload;
        slot.terminal_cause.store(payload.cause, std::memory_order_release);
        if (activation_claimed) {
            slot.activation_commit.store(static_cast<std::uint32_t>(ActivationCommit::Cancelled),
                                         std::memory_order_release);
        }
        return true;
    }

    static void overflow(StreamIngressSlot& slot, StreamTerminalCause cause,
                         std::uint64_t queued_items, std::uint64_t queued_bytes,
                         std::uint64_t incoming_bytes) noexcept {
        static_cast<void>(claim(slot, {.cause = cause,
                                       .operation = slot.projection.operation,
                                       .queued_items = queued_items,
                                       .queued_bytes = queued_bytes,
                                       .incoming_bytes = incoming_bytes,
                                       .metadata_failure = {}}));
    }

    static bool matches(const StreamIngressSlot& slot, const StreamItemView& item) noexcept {
        if (load_state(slot) != StreamIngressState::Published) {
            return false;
        }
        const auto& projection = slot.projection;
        if (projection.client_id <= 0 || projection.generation == 0 ||
            item.receive_sequence() <= projection.activation_receive_sequence ||
            (projection.type_mask & stream_event_mask(item.routing().event_class)) == 0) {
            return false;
        }
        if (projection.chat_count == 0) {
            return true;
        }
        return std::binary_search(projection.chat_ids.begin(),
                                  projection.chat_ids.begin() + projection.chat_count,
                                  item.routing().chat_id);
    }

    static void enqueue(StreamIngressSlot& slot, const StreamItemView& item) noexcept {
        if (slot.terminal_cause.load(std::memory_order_acquire) != StreamTerminalCause::Open ||
            !matches(slot, item)) {
            return;
        }
        const auto producer = slot.descriptor_producer.load(std::memory_order_relaxed);
        const auto consumer = slot.descriptor_consumer.load(std::memory_order_acquire);
        const auto byte_producer_value = slot.byte_producer.load(std::memory_order_relaxed);
        const auto byte_consumer_value = slot.byte_consumer.load(std::memory_order_acquire);
        const auto incoming = static_cast<std::uint64_t>(item.size());
        if (producer == std::numeric_limits<std::uint64_t>::max() ||
            incoming > std::numeric_limits<std::uint64_t>::max() - byte_producer_value) {
            overflow(slot, StreamTerminalCause::CounterExhausted, producer - consumer,
                     byte_producer_value - byte_consumer_value, incoming);
            return;
        }
        if (incoming > kStreamQueueItemBytes) {
            overflow(slot, StreamTerminalCause::ItemBytes, producer - consumer,
                     byte_producer_value - byte_consumer_value, incoming);
            return;
        }
        const auto queued_items = producer - consumer;
        if (queued_items >= kStreamQueueItems) {
            overflow(slot, StreamTerminalCause::QueueItems, queued_items,
                     byte_producer_value - byte_consumer_value, incoming);
            return;
        }
        const auto queued_bytes = byte_producer_value - byte_consumer_value;
        if (incoming > kStreamQueueBytes - queued_bytes) {
            overflow(slot, StreamTerminalCause::QueueBytes, queued_items, queued_bytes, incoming);
            return;
        }

        const auto physical = static_cast<std::size_t>(byte_producer_value % kStreamQueueBytes);
        const auto first_capacity = kStreamQueueBytes - physical;
        const auto first_size = std::min<std::size_t>(item.size(), first_capacity);
        auto destination = physical;
        auto remaining_first = first_size;
        std::size_t source_offset = 0;
        for (const auto source : item.spans()) {
            const auto into_first = std::min(remaining_first, source.size());
            if (into_first != 0) {
                std::memcpy(slot.bytes->data() + destination, source.data(), into_first);
                destination += into_first;
                remaining_first -= into_first;
            }
            if (source.size() > into_first) {
                std::memcpy(slot.bytes->data() + source_offset, source.data() + into_first,
                            source.size() - into_first);
                source_offset += source.size() - into_first;
            }
        }
        const auto& routing = item.routing();
        (*slot.descriptors)[producer % kStreamQueueItems] = {
            .byte_ticket = byte_producer_value,
            .receive_sequence = item.receive_sequence(),
            .chat_id = routing.chat_id,
            .sender_id = routing.sender_id,
            .json_offset = 0,
            .json_size = static_cast<std::uint32_t>(incoming),
            .message_offset = routing.message_offset,
            .message_size = routing.message_size,
            .text_offset = routing.text_offset,
            .text_size = routing.text_size,
            .event_class = routing.event_class,
            .sender_kind = routing.sender_kind};
        slot.byte_producer.store(byte_producer_value + incoming, std::memory_order_release);
        slot.descriptor_producer.store(producer + 1, std::memory_order_release);
    }

    static void poison_queued(StreamIngressSlot& slot) noexcept {
        const auto descriptor_begin = slot.descriptor_consumer.load(std::memory_order_relaxed);
        const auto descriptor_end = slot.descriptor_producer.load(std::memory_order_relaxed);
        if (descriptor_end >= descriptor_begin &&
            descriptor_end - descriptor_begin <= kStreamQueueItems && slot.descriptors != nullptr) {
            for (auto ticket = descriptor_begin; ticket < descriptor_end; ++ticket) {
                poison((*slot.descriptors)[ticket % kStreamQueueItems]);
            }
        }
        const auto byte_begin = slot.byte_consumer.load(std::memory_order_relaxed);
        const auto byte_end = slot.byte_producer.load(std::memory_order_relaxed);
        if (byte_end >= byte_begin && byte_end - byte_begin <= kStreamQueueBytes &&
            slot.bytes != nullptr) {
            const auto amount = static_cast<std::size_t>(byte_end - byte_begin);
            const auto physical = static_cast<std::size_t>(byte_begin % kStreamQueueBytes);
            const auto first = std::min(amount, kStreamQueueBytes - physical);
            std::memset(slot.bytes->data() + physical, kReclaimedPoison, first);
            if (amount > first) {
                std::memset(slot.bytes->data(), kReclaimedPoison, amount - first);
            }
        }
    }

    static void reset_slot_locked(StreamIngressSlot& slot) noexcept {
        poison_queued(slot);
        invalidate_borrow(slot);
        poison(slot.staged);
        poison(slot.projection);
        poison(slot.terminal_payload);
        slot.descriptor_producer.store(0, std::memory_order_relaxed);
        slot.descriptor_consumer.store(0, std::memory_order_relaxed);
        slot.byte_producer.store(0, std::memory_order_relaxed);
        slot.byte_consumer.store(0, std::memory_order_relaxed);
        slot.owner_client_id.store(0, std::memory_order_release);
        slot.owner_generation.store(0, std::memory_order_release);
        slot.owner_operation.store(static_cast<std::uint32_t>(StreamOperation::Listen),
                                   std::memory_order_release);
        slot.terminal_cause.store(StreamTerminalCause::Open, std::memory_order_relaxed);
        slot.publication_ready.store(0, std::memory_order_relaxed);
        slot.activation_commit.store(static_cast<std::uint32_t>(ActivationCommit::Open),
                                     std::memory_order_relaxed);
        slot.terminal_delivered = false;
        store_state(slot, StreamIngressState::Free);
    }

    bool reclaim_locked(std::size_t index, std::optional<std::uint64_t> epoch) noexcept {
        auto& slot = slots[index];
        if (epoch && slot.epoch.load(std::memory_order_acquire) != *epoch) {
            return false;
        }
        notify(detail::StreamIngressProbePoint::ReclaimLoad, index);
        if (load_state(slot) != StreamIngressState::Removed ||
            slot.active_borrows.load(std::memory_order_acquire) != 0 ||
            (slot.borrow_epoch.load(std::memory_order_acquire) & 1U) != 0U ||
            slot.activation_commit.load(std::memory_order_acquire) ==
                static_cast<std::uint32_t>(ActivationCommit::TerminalClaiming) ||
            publisher_count.load(std::memory_order_seq_cst) != 0) {
            return false;
        }
        notify(detail::StreamIngressProbePoint::ReclaimZero, index);
        if (!compare_state_strong(slot, StreamIngressState::Removed,
                                  StreamIngressState::Reclaimable)) {
            return false;
        }
        reset_slot_locked(slot);
        retired[index] = false;
        retired_epoch[index] = 0;
        return true;
    }

    void sweep_retired_locked() noexcept {
        if (publisher_count.load(std::memory_order_seq_cst) != 0) {
            return;
        }
        for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
            if (retired[index]) {
                static_cast<void>(reclaim_locked(index, retired_epoch[index]));
            }
        }
    }

    void transfer_retired_locked(std::size_t index, std::uint64_t epoch) noexcept {
        retired[index] = true;
        retired_epoch[index] = epoch;
    }

    void receive_claim_slots(std::int32_t client_id, std::uint64_t generation,
                             StreamTerminalPayload payload, bool matching) noexcept {
        const PublisherGuard guard(*this);
        if (!guard) {
            return;
        }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            auto& slot = slots[index];
            const auto owner_client = slot.owner_client_id.load(std::memory_order_acquire);
            const auto owner_generation_value =
                slot.owner_generation.load(std::memory_order_acquire);
            const auto state = load_state(slot);
            notify(detail::StreamIngressProbePoint::OwnerLoad, index);
            const bool same = owner_client == client_id && owner_generation_value == generation;
            if (owner_client == 0 || owner_generation_value == 0 || same != matching ||
                state == StreamIngressState::Free) {
                continue;
            }
            payload.operation =
                static_cast<StreamOperation>(slot.owner_operation.load(std::memory_order_acquire));
            static_cast<void>(claim(slot, payload));
        }
        for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
            auto* first = dormant[index].load(std::memory_order_seq_cst);
            notify(detail::StreamIngressProbePoint::SlotLoad, index);
            if (actual(index, first)) {
                const bool same =
                    first->staged.client_id == client_id && first->staged.generation == generation;
                if (same == matching) {
                    payload.operation = first->staged.operation;
                    static_cast<void>(claim(*first, payload));
                }
            }
            auto* second = published[index].load(std::memory_order_seq_cst);
            notify(detail::StreamIngressProbePoint::SlotLoad, index);
            if (actual(index, second) && second != first) {
                const bool same = second->staged.client_id == client_id &&
                                  second->staged.generation == generation;
                if (same == matching) {
                    payload.operation = second->staged.operation;
                    static_cast<void>(claim(*second, payload));
                }
            }
        }
    }

    std::array<StreamIngressSlot, kStreamSubscriberSlots> slots;
    std::array<StreamIngressSlot, kStreamSubscriberSlots> installing_markers;
    std::array<std::atomic<StreamIngressSlot*>, kStreamSubscriberSlots> dormant;
    std::array<std::atomic<StreamIngressSlot*>, kStreamSubscriberSlots> published;
    std::atomic<std::uint32_t> publisher_count{0};
    std::atomic<std::int32_t> current_client_id{0};
    std::atomic<std::uint64_t> current_generation{0};
    RetainedGenerationTerminal retained_terminal;
    mutable std::mutex control;
    std::array<bool, kStreamSubscriberSlots> retired{};
    std::array<std::uint64_t, kStreamSubscriberSlots> retired_epoch{};
    std::optional<StreamIngressAtomic> lock_free_failure;
    detail::StreamIngressProbe probe;
};

StreamIngressBorrowedSpan::StreamIngressBorrowedSpan(StreamIngressFrontCursor& owner,
                                                     std::size_t index) noexcept
    : owner_(&owner), index_(index) {}

std::optional<std::size_t> StreamIngressBorrowedSpan::size() const noexcept {
    return owner_ == nullptr ? std::nullopt : owner_->span_size(index_);
}

bool StreamIngressBorrowedSpan::copy_to(std::span<char> destination) const noexcept {
    return owner_ != nullptr && owner_->copy_span(index_, destination);
}

StreamIngressFrontCursor::StreamIngressFrontCursor() noexcept
    : first_(*this, 0), second_(*this, 1) {}

void StreamIngressFrontCursor::activate(StreamIngressSlot& slot, std::uint64_t reservation_epoch,
                                        std::uint64_t borrow_epoch,
                                        StreamIngressDescriptor descriptor,
                                        std::span<const char> first,
                                        std::span<const char> second) noexcept {
    slot_ = &slot;
    reservation_epoch_ = reservation_epoch;
    borrow_epoch_ = borrow_epoch;
    descriptor_ = descriptor;
    spans_ = {first, second};
}

bool StreamIngressFrontCursor::valid() const noexcept {
    return slot_ != nullptr && slot_->epoch.load(std::memory_order_acquire) == reservation_epoch_ &&
           slot_->borrow_epoch.load(std::memory_order_acquire) == borrow_epoch_ &&
           (borrow_epoch_ & 1U) != 0U && load_state(*slot_) == StreamIngressState::Published;
}

std::optional<StreamIngressDescriptor> StreamIngressFrontCursor::descriptor() const noexcept {
    return valid() ? std::optional<StreamIngressDescriptor>{descriptor_} : std::nullopt;
}

bool StreamIngressFrontCursor::visit_spans(void* context,
                                           StreamIngressSpanVisitor visitor) const noexcept {
    if (!valid() || visitor == nullptr) {
        return false;
    }
    visitor(context, first_, second_);
    return valid();
}

bool StreamIngressFrontCursor::copy_span(std::size_t index,
                                         std::span<char> destination) const noexcept {
    if (!valid() || index >= spans_.size() || destination.size() < spans_[index].size()) {
        return false;
    }
    if (!spans_[index].empty()) {
        std::memcpy(destination.data(), spans_[index].data(), spans_[index].size());
    }
    return valid();
}

std::optional<std::size_t> StreamIngressFrontCursor::span_size(std::size_t index) const noexcept {
    if (!valid() || index >= spans_.size()) {
        return std::nullopt;
    }
    return spans_[index].size();
}

StreamIngressReservation::StreamIngressReservation() = default;

StreamIngressReservation::StreamIngressReservation(StreamIngressHub* hub, std::uint32_t index,
                                                   std::uint64_t epoch) noexcept
    : hub_(hub), index_(index), epoch_(epoch) {}

StreamIngressReservation::~StreamIngressReservation() {
    reset();
}

StreamIngressReservation::StreamIngressReservation(StreamIngressReservation&& other) noexcept
    : hub_(std::exchange(other.hub_, nullptr)), index_(other.index_), epoch_(other.epoch_) {}

StreamIngressReservation&
StreamIngressReservation::operator=(StreamIngressReservation&& other) noexcept {
    if (this != &other) {
        reset();
        hub_ = std::exchange(other.hub_, nullptr);
        index_ = other.index_;
        epoch_ = other.epoch_;
    }
    return *this;
}

StreamIngressReservation::operator bool() const noexcept {
    return hub_ != nullptr;
}

StreamIngressPreparedActivation::StreamIngressPreparedActivation(StreamIngressHub* hub,
                                                                 std::uint32_t index,
                                                                 std::uint64_t epoch) noexcept
    : hub_(hub), index_(index), epoch_(epoch) {}

StreamIngressPreparedActivation::StreamIngressPreparedActivation(
    StreamIngressPreparedActivation&& other) noexcept
    : hub_(std::exchange(other.hub_, nullptr)), index_(other.index_), epoch_(other.epoch_) {}

StreamIngressPreparedActivation&
StreamIngressPreparedActivation::operator=(StreamIngressPreparedActivation&& other) noexcept {
    if (this != &other) {
        hub_ = std::exchange(other.hub_, nullptr);
        index_ = other.index_;
        epoch_ = other.epoch_;
    }
    return *this;
}

StreamIngressPreparedActivation::operator bool() const noexcept {
    return hub_ != nullptr;
}

void StreamIngressReservation::reset() noexcept {
    if (hub_ != nullptr) {
        hub_->abandon(*this);
    }
}

StreamIngressHub::StreamIngressHub(detail::StreamIngressProbe probe)
    : impl_(std::make_unique<Impl>(probe)) {}

StreamIngressHub::~StreamIngressHub() {
    const std::lock_guard lock(impl_->control);
    impl_->sweep_retired_locked();
}

std::optional<StreamIngressAtomic> StreamIngressHub::lock_free_failure() const noexcept {
    return impl_->lock_free_failure;
}

StreamIngressAdmissionResult StreamIngressHub::reserve(const StreamIngressRequest& request) {
    const std::lock_guard lock(impl_->control);
    impl_->sweep_retired_locked();
    if (impl_->lock_free_failure) {
        return StreamIngressAdmissionFailure{
            .resource = StreamIngressAdmissionResource::LockFreeIngress,
            .atomic = impl_->lock_free_failure.value_or(StreamIngressAtomic::SlotPointer)};
    }
    if (!valid_request(request)) {
        return StreamIngressInvalidRequest{};
    }
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto& slot = impl_->slots[index];
        if (load_state(slot) != StreamIngressState::Free) {
            continue;
        }
        if (slot.descriptors == nullptr) {
            slot.descriptors =
                std::make_unique_for_overwrite<StreamIngressSlot::DescriptorStorage>();
            slot.bytes = std::make_unique_for_overwrite<StreamIngressSlot::ByteStorage>();
        }
        const auto old_epoch = slot.epoch.load(std::memory_order_relaxed);
        if (old_epoch == std::numeric_limits<std::uint64_t>::max()) {
            continue;
        }
        const auto epoch = old_epoch + 1;
        slot.epoch.store(epoch, std::memory_order_relaxed);
        slot.staged = request;
        slot.projection = {};
        slot.descriptor_producer.store(0, std::memory_order_relaxed);
        slot.descriptor_consumer.store(0, std::memory_order_relaxed);
        slot.byte_producer.store(0, std::memory_order_relaxed);
        slot.byte_consumer.store(0, std::memory_order_relaxed);
        slot.terminal_payload = {};
        slot.terminal_delivered = false;
        slot.terminal_cause.store(StreamTerminalCause::Open, std::memory_order_relaxed);
        slot.publication_ready.store(0, std::memory_order_relaxed);
        slot.activation_commit.store(static_cast<std::uint32_t>(ActivationCommit::Open),
                                     std::memory_order_relaxed);
        slot.owner_client_id.store(request.client_id, std::memory_order_relaxed);
        slot.owner_generation.store(request.generation, std::memory_order_relaxed);
        slot.owner_operation.store(static_cast<std::uint32_t>(request.operation),
                                   std::memory_order_relaxed);
        impl_->notify(detail::StreamIngressProbePoint::ReservationOwnerPublished, index);
        store_state(slot, StreamIngressState::Reserved);
        return StreamIngressReservation(this, static_cast<std::uint32_t>(index), epoch);
    }
    return StreamIngressAdmissionFailure{.resource =
                                             StreamIngressAdmissionResource::SubscriberSlots,
                                         .atomic = StreamIngressAtomic::SlotPointer};
}

void StreamIngressHub::begin_generation(std::int32_t client_id, std::uint64_t generation) noexcept {
    impl_->retained_terminal.reset(client_id, generation);
    impl_->current_client_id.store(client_id, std::memory_order_release);
    impl_->current_generation.store(generation, std::memory_order_release);
    const StreamTerminalPayload replacement{.cause = StreamTerminalCause::GenerationReplaced,
                                            .metadata_failure = {}};
    impl_->receive_claim_slots(client_id, generation, replacement, false);
}

void StreamIngressHub::claim_generation(std::int32_t client_id, std::uint64_t generation,
                                        StreamTerminalPayload payload) noexcept {
    static_cast<void>(impl_->retained_terminal.publish(client_id, generation, payload));
    const auto retained =
        impl_->retained_terminal.snapshot(client_id, generation, StreamOperation::Listen);
    if (!retained) {
        return;
    }
    impl_->receive_claim_slots(client_id, generation, *retained, true);
}

void StreamIngressHub::claim_control_generation(std::int32_t client_id, std::uint64_t generation,
                                                StreamTerminalPayload payload) noexcept {
    const std::lock_guard lock(impl_->control);
    for (auto& slot : impl_->slots) {
        if (slot.owner_client_id.load(std::memory_order_acquire) != client_id ||
            slot.owner_generation.load(std::memory_order_acquire) != generation ||
            load_state(slot) == StreamIngressState::Free) {
            continue;
        }
        payload.operation =
            static_cast<StreamOperation>(slot.owner_operation.load(std::memory_order_acquire));
        static_cast<void>(Impl::claim(slot, payload));
    }
}

std::optional<StreamIngressPreparedActivation>
StreamIngressHub::prepare_activation(StreamIngressReservation& reservation) noexcept {
    const std::lock_guard lock(impl_->control);
    if (!impl_->valid(reservation)) {
        return std::nullopt;
    }
    auto& slot = impl_->slots[reservation.index_];
    if (load_state(slot) != StreamIngressState::Reserved) {
        return std::nullopt;
    }
    const auto current_generation = impl_->current_generation.load(std::memory_order_acquire);
    const bool stale =
        current_generation != 0 &&
        (impl_->current_client_id.load(std::memory_order_acquire) != slot.staged.client_id ||
         current_generation != slot.staged.generation);
    if (stale) {
        static_cast<void>(Impl::claim(slot, {.cause = StreamTerminalCause::GenerationReplaced,
                                             .operation = slot.staged.operation,
                                             .metadata_failure = {}}));
    }
    const auto retained = impl_->retained_terminal.snapshot(
        slot.staged.client_id, slot.staged.generation, slot.staged.operation);
    if (retained) {
        static_cast<void>(Impl::claim(slot, *retained));
    }
    if (stale || retained ||
        slot.terminal_cause.load(std::memory_order_acquire) != StreamTerminalCause::Open) {
        store_state(slot, StreamIngressState::Removed);
        return std::nullopt;
    }
    impl_->notify(detail::StreamIngressProbePoint::ActivationPreparing, reservation.index_);
    auto activation = static_cast<std::uint32_t>(ActivationCommit::Open);
    if (!slot.activation_commit.compare_exchange_strong(
            activation, static_cast<std::uint32_t>(ActivationCommit::Prepared),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        store_state(slot, StreamIngressState::Removed);
        return std::nullopt;
    }
    return StreamIngressPreparedActivation(this, reservation.index_, reservation.epoch_);
}

bool StreamIngressHub::commit_activation_promotion(
    StreamIngressPreparedActivation& prepared) noexcept {
    if (prepared.hub_ != this || prepared.index_ >= kStreamSubscriberSlots) {
        return false;
    }
    auto& slot = impl_->slots[prepared.index_];
    if (slot.epoch.load(std::memory_order_acquire) != prepared.epoch_) {
        return false;
    }
    auto expected = static_cast<std::uint32_t>(ActivationCommit::Prepared);
    return slot.activation_commit.compare_exchange_strong(
        expected, static_cast<std::uint32_t>(ActivationCommit::PromotionCommitted),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

void StreamIngressHub::publish_prepared(StreamIngressReservation& reservation,
                                        StreamIngressPreparedActivation& prepared) noexcept {
    const std::lock_guard lock(impl_->control);
    if (!impl_->valid(reservation) || prepared.hub_ != this ||
        prepared.index_ != reservation.index_ || prepared.epoch_ != reservation.epoch_ ||
        impl_->slots[reservation.index_].activation_commit.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(ActivationCommit::PromotionCommitted)) {
        std::terminate();
    }
    auto& slot = impl_->slots[reservation.index_];
    if (!compare_state_strong(slot, StreamIngressState::Reserved, StreamIngressState::Armed)) {
        std::terminate();
    }
    impl_->dormant[reservation.index_].store(&slot, std::memory_order_seq_cst);
    prepared.hub_ = nullptr;
}

bool StreamIngressHub::commit_activation(StreamIngressReservation& reservation) noexcept {
    auto prepared = prepare_activation(reservation);
    if (!prepared || !commit_activation_promotion(*prepared)) {
        if (impl_->valid(reservation)) {
            store_state(impl_->slots[reservation.index_], StreamIngressState::Removed);
        }
        return false;
    }
    publish_prepared(reservation, *prepared);
    return true;
}

StreamIngressState
StreamIngressHub::activation_state(const StreamIngressReservation& reservation) const noexcept {
    if (!impl_->valid(reservation)) {
        return StreamIngressState::Free;
    }
    const auto& slot = impl_->slots[reservation.index_];
    const auto state = load_state(slot);
    return state == StreamIngressState::Published &&
                   slot.publication_ready.load(std::memory_order_acquire) == 0
               ? StreamIngressState::Installing
               : state;
}

std::optional<StreamActivationProjection> StreamIngressHub::activation_projection(
    const StreamIngressReservation& reservation) const noexcept {
    if (!impl_->valid(reservation)) {
        return std::nullopt;
    }
    const auto& slot = impl_->slots[reservation.index_];
    if (load_state(slot) != StreamIngressState::Published ||
        slot.publication_ready.load(std::memory_order_acquire) == 0) {
        return std::nullopt;
    }
    return slot.projection;
}

std::size_t StreamIngressHub::activate_armed(std::int32_t client_id, std::uint64_t generation,
                                             std::uint64_t receive_sequence, bool ready) noexcept {
    const auto current_generation = impl_->current_generation.load(std::memory_order_acquire);
    if (!ready || (current_generation != 0 &&
                   (impl_->current_client_id.load(std::memory_order_acquire) != client_id ||
                    current_generation != generation))) {
        return 0;
    }
    const Impl::PublisherGuard guard(*impl_);
    if (!guard) {
        return 0;
    }
    std::size_t activated = 0;
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto* slot = impl_->dormant[index].load(std::memory_order_seq_cst);
        impl_->notify(detail::StreamIngressProbePoint::SlotLoad, index);
        if (!impl_->actual(index, slot) || slot->staged.client_id != client_id ||
            slot->staged.generation != generation) {
            continue;
        }
        auto* expected_pointer = static_cast<StreamIngressSlot*>(nullptr);
        if (!impl_->published[index].compare_exchange_strong(
                expected_pointer, &impl_->installing_markers[index], std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            continue;
        }
        if (!compare_state_strong(*slot, StreamIngressState::Armed,
                                  StreamIngressState::Installing)) {
            impl_->published[index].exchange(nullptr, std::memory_order_seq_cst);
            continue;
        }
        impl_->notify(detail::StreamIngressProbePoint::ActivationInstalling, index);
        slot->projection = {.client_id = client_id,
                            .generation = generation,
                            .activation_receive_sequence = receive_sequence,
                            .chat_ids = slot->staged.chat_ids,
                            .chat_count = slot->staged.chat_count,
                            .type_mask = slot->staged.type_mask,
                            .mode = slot->staged.mode,
                            .operation = slot->staged.operation};
        const auto* observed_marker = impl_->published[index].load(std::memory_order_seq_cst);
        impl_->notify(detail::StreamIngressProbePoint::SlotLoad, index);
        if (impl_->marker(index, observed_marker)) {
            impl_->notify(detail::StreamIngressProbePoint::MarkerLoad, index);
        }
        const bool still_current =
            (impl_->current_generation.load(std::memory_order_acquire) == 0 ||
             (impl_->current_client_id.load(std::memory_order_acquire) == client_id &&
              impl_->current_generation.load(std::memory_order_acquire) == generation));
        if (!impl_->marker(index, observed_marker) || !still_current ||
            slot->terminal_cause.load(std::memory_order_acquire) != StreamTerminalCause::Open ||
            !compare_state_strong(*slot, StreamIngressState::Installing,
                                  StreamIngressState::Published)) {
            impl_->published[index].exchange(nullptr, std::memory_order_seq_cst);
            impl_->dormant[index].exchange(nullptr, std::memory_order_seq_cst);
            store_state(*slot, StreamIngressState::Removed);
            continue;
        }
        auto* marker = &impl_->installing_markers[index];
        if (!impl_->published[index].compare_exchange_strong(
                marker, slot, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
            slot->publication_ready.store(0, std::memory_order_release);
            store_state(*slot, StreamIngressState::Removed);
            continue;
        }
        impl_->notify(detail::StreamIngressProbePoint::Publication, index);
        slot->publication_ready.store(1U, std::memory_order_release);
        impl_->dormant[index].exchange(nullptr, std::memory_order_seq_cst);
        ++activated;
    }
    return activated;
}

void StreamIngressHub::publish(const StreamItemView& item) noexcept {
    const Impl::PublisherGuard guard(*impl_);
    if (!guard) {
        return;
    }
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto* slot = impl_->published[index].load(std::memory_order_seq_cst);
        impl_->notify(detail::StreamIngressProbePoint::SlotLoad, index);
        if (impl_->marker(index, slot)) {
            impl_->notify(detail::StreamIngressProbePoint::MarkerLoad, index);
        }
        if (impl_->actual(index, slot)) {
            Impl::enqueue(*slot, item);
        }
    }
}

StreamIngressFrontResult StreamIngressHub::visit_front(StreamIngressReservation& reservation,
                                                       void* context,
                                                       StreamIngressFrontVisitor visitor) {
    if (!impl_->valid(reservation) || visitor == nullptr) {
        return StreamIngressFrontResult::InvalidReservation;
    }
    auto& slot = impl_->slots[reservation.index_];
    if (load_state(slot) != StreamIngressState::Published ||
        slot.terminal_cause.load(std::memory_order_acquire) != StreamTerminalCause::Open) {
        return StreamIngressFrontResult::Empty;
    }
    const auto consumer = slot.descriptor_consumer.load(std::memory_order_relaxed);
    const auto producer = slot.descriptor_producer.load(std::memory_order_acquire);
    if (consumer == producer) {
        return StreamIngressFrontResult::Empty;
    }
    auto borrow = slot.borrow_epoch.load(std::memory_order_acquire);
    auto no_active_borrow = 0U;
    if (!slot.active_borrows.compare_exchange_strong(
            no_active_borrow, 1U, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return StreamIngressFrontResult::Invalidated;
    }
    if ((borrow & 1U) != 0U || borrow == std::numeric_limits<std::uint64_t>::max() ||
        !advance_borrow_epoch(slot, borrow, borrow + 1)) {
        slot.active_borrows.store(0, std::memory_order_release);
        return StreamIngressFrontResult::Invalidated;
    }
    const auto active_borrow = borrow + 1;
    struct BorrowRelease {
        StreamIngressSlot* slot = nullptr;
        std::uint64_t token = 0;
        bool active = true;

        BorrowRelease(StreamIngressSlot& slot_value, std::uint64_t token_value) noexcept
            : slot(&slot_value), token(token_value) {}
        ~BorrowRelease() {
            if (token != std::numeric_limits<std::uint64_t>::max()) {
                static_cast<void>(advance_borrow_epoch(*slot, token, token + 1));
            }
            if (active) {
                slot->active_borrows.store(0, std::memory_order_release);
            }
        }
        BorrowRelease(const BorrowRelease&) = delete;
        BorrowRelease& operator=(const BorrowRelease&) = delete;
        BorrowRelease(BorrowRelease&&) = delete;
        BorrowRelease& operator=(BorrowRelease&&) = delete;
    } release(slot, active_borrow);

    const auto descriptor = (*slot.descriptors)[consumer % kStreamQueueItems];
    const auto physical = static_cast<std::size_t>(descriptor.byte_ticket % kStreamQueueBytes);
    const auto first_size =
        std::min<std::size_t>(descriptor.json_size, kStreamQueueBytes - physical);
    reservation.cursor_.activate(slot, reservation.epoch_, active_borrow, descriptor,
                                 {slot.bytes->data() + physical, first_size},
                                 {slot.bytes->data(), descriptor.json_size - first_size});
    const auto action = visitor(context, reservation.cursor_);
    const bool retained = reservation.cursor_.valid();
    static_cast<void>(advance_borrow_epoch(slot, active_borrow, active_borrow + 1));
    slot.active_borrows.store(0, std::memory_order_release);
    release.token = std::numeric_limits<std::uint64_t>::max();
    release.active = false;
    if (!retained) {
        return StreamIngressFrontResult::Invalidated;
    }
    if (action == StreamIngressFrontAction::Keep) {
        return StreamIngressFrontResult::Visited;
    }

    const auto current_consumer = slot.descriptor_consumer.load(std::memory_order_relaxed);
    const auto current_producer = slot.descriptor_producer.load(std::memory_order_acquire);
    if (current_consumer != consumer || current_consumer == current_producer ||
        (*slot.descriptors)[current_consumer % kStreamQueueItems].byte_ticket !=
            descriptor.byte_ticket) {
        return StreamIngressFrontResult::Invalidated;
    }
    if (first_size != 0) {
        std::memset(slot.bytes->data() + physical, kReclaimedPoison, first_size);
    }
    if (descriptor.json_size > first_size) {
        std::memset(slot.bytes->data(), kReclaimedPoison, descriptor.json_size - first_size);
    }
    poison((*slot.descriptors)[current_consumer % kStreamQueueItems]);
    slot.byte_consumer.store(descriptor.byte_ticket + descriptor.json_size,
                             std::memory_order_release);
    slot.descriptor_consumer.store(current_consumer + 1, std::memory_order_release);
    return StreamIngressFrontResult::Consumed;
}

void StreamIngressHub::discard(const StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        return;
    }
    auto& slot = impl_->slots[reservation.index_];
    invalidate_borrow(slot);
    impl_->poison_queued(slot);
    slot.descriptor_consumer.store(slot.descriptor_producer.load(std::memory_order_acquire),
                                   std::memory_order_release);
    slot.byte_consumer.store(slot.byte_producer.load(std::memory_order_acquire),
                             std::memory_order_release);
}

bool StreamIngressHub::claim(StreamIngressReservation& reservation,
                             StreamTerminalPayload payload) noexcept {
    return impl_->valid(reservation) && Impl::claim(impl_->slots[reservation.index_], payload);
}

bool StreamIngressHub::begin_item_delivery(StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        return false;
    }
    auto& slot = impl_->slots[reservation.index_];
    if (load_state(slot) != StreamIngressState::Published) {
        return false;
    }
    return slot.terminal_cause.load(std::memory_order_acquire) == StreamTerminalCause::Open;
}

std::optional<StreamTerminalPayload>
StreamIngressHub::claim_terminal(StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        return std::nullopt;
    }
    auto& slot = impl_->slots[reservation.index_];
    const auto cause = slot.terminal_cause.load(std::memory_order_acquire);
    if (!ready_cause(cause) || slot.terminal_delivered) {
        return std::nullopt;
    }
    slot.terminal_delivered = true;
    return slot.terminal_payload;
}

std::optional<StreamTerminalPayload>
StreamIngressHub::terminal_snapshot(const StreamIngressReservation& reservation) const noexcept {
    if (!impl_->valid(reservation)) {
        return std::nullopt;
    }
    const auto& slot = impl_->slots[reservation.index_];
    if (!ready_cause(slot.terminal_cause.load(std::memory_order_acquire))) {
        return std::nullopt;
    }
    return slot.terminal_payload;
}

bool StreamIngressHub::detach(StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        return false;
    }
    auto& slot = impl_->slots[reservation.index_];
    const auto raw_state = slot.state.load(std::memory_order_acquire);
    auto decoded_state = decode_state(raw_state);
    if (!decoded_state) {
        impl_->published[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
        impl_->dormant[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
        impl_->notify(detail::StreamIngressProbePoint::SlotExchange, reservation.index_);
        invalidate_borrow(slot);
        slot.publication_ready.store(0, std::memory_order_release);
        store_state(slot, StreamIngressState::Removed);
        return true;
    }
    auto state = *decoded_state;
    while (state == StreamIngressState::Armed || state == StreamIngressState::Installing ||
           state == StreamIngressState::Published) {
        if (compare_state(slot, state, StreamIngressState::Removing)) {
            invalidate_borrow(slot);
            slot.publication_ready.store(0, std::memory_order_release);
            impl_->published[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
            impl_->dormant[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
            impl_->notify(detail::StreamIngressProbePoint::SlotExchange, reservation.index_);
            store_state(slot, StreamIngressState::Removed);
            return true;
        }
    }
    if (state == StreamIngressState::Reserved) {
        return false;
    }
    if (state == StreamIngressState::Removed || state == StreamIngressState::Reclaimable) {
        return true;
    }
    if (state == StreamIngressState::Removing) {
        return false;
    }
    impl_->published[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
    impl_->dormant[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
    impl_->notify(detail::StreamIngressProbePoint::SlotExchange, reservation.index_);
    slot.publication_ready.store(0, std::memory_order_release);
    store_state(slot, StreamIngressState::Removed);
    return true;
}

bool StreamIngressHub::poll_reclaim(StreamIngressReservation& reservation) noexcept {
    const std::lock_guard lock(impl_->control);
    if (!impl_->valid(reservation) ||
        !impl_->reclaim_locked(reservation.index_, reservation.epoch_)) {
        return false;
    }
    reservation.hub_ = nullptr;
    return true;
}

void StreamIngressHub::poll_control() noexcept {
    const std::lock_guard lock(impl_->control);
    impl_->sweep_retired_locked();
}

void StreamIngressHub::abandon(StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        reservation.hub_ = nullptr;
        return;
    }
    auto& slot = impl_->slots[reservation.index_];
    if (load_state(slot) == StreamIngressState::Reserved) {
        const std::lock_guard lock(impl_->control);
        if (impl_->valid(reservation) && load_state(slot) == StreamIngressState::Reserved) {
            store_state(slot, StreamIngressState::Removed);
            impl_->transfer_retired_locked(reservation.index_, reservation.epoch_);
            impl_->sweep_retired_locked();
        }
        reservation.hub_ = nullptr;
        return;
    }
    static_cast<void>(detach(reservation));
    {
        const std::lock_guard lock(impl_->control);
        if (impl_->valid(reservation)) {
            impl_->transfer_retired_locked(reservation.index_, reservation.epoch_);
            impl_->sweep_retired_locked();
        }
    }
    reservation.hub_ = nullptr;
}

namespace {

using PollRep = StreamPollSchedule::Clock::duration::rep;
using PollUnsignedRep = std::make_unsigned_t<PollRep>;
static_assert(std::is_integral_v<PollRep>);
static_assert(std::is_signed_v<PollRep>);

PollUnsignedRep magnitude(PollRep value) noexcept {
    if (value >= 0) {
        return static_cast<PollUnsignedRep>(value);
    }
    return static_cast<PollUnsignedRep>(-(value + 1)) + 1U;
}

PollUnsignedRep distance(PollRep lower, PollRep upper) noexcept {
    if (lower >= 0) {
        return static_cast<PollUnsignedRep>(upper) - static_cast<PollUnsignedRep>(lower);
    }
    if (upper < 0) {
        return magnitude(lower) - magnitude(upper);
    }
    return magnitude(lower) + static_cast<PollUnsignedRep>(upper);
}

PollRep add_nonnegative(PollRep base, PollUnsignedRep delta) noexcept {
    if (base >= 0) {
        return static_cast<PollRep>(static_cast<PollUnsignedRep>(base) + delta);
    }
    const auto base_magnitude = magnitude(base);
    if (delta < base_magnitude) {
        const auto result_magnitude = base_magnitude - delta;
        if (result_magnitude == magnitude(std::numeric_limits<PollRep>::min())) {
            return std::numeric_limits<PollRep>::min();
        }
        return -static_cast<PollRep>(result_magnitude);
    }
    return static_cast<PollRep>(delta - base_magnitude);
}

StreamPollSchedule::Clock::time_point
saturating_add(StreamPollSchedule::Clock::time_point base,
               StreamPollSchedule::Clock::duration positive) noexcept {
    const auto base_count = base.time_since_epoch().count();
    const auto maximum = std::numeric_limits<PollRep>::max();
    const auto increment = static_cast<PollUnsignedRep>(positive.count());
    const auto room = distance(base_count, maximum);
    if (increment > room) {
        return StreamPollSchedule::Clock::time_point::max();
    }
    return StreamPollSchedule::Clock::time_point{
        StreamPollSchedule::Clock::duration{add_nonnegative(base_count, increment)}};
}

} // namespace

StreamPollSchedule::StreamPollSchedule(Clock::time_point now) noexcept
    : next_poll_(saturating_add(now, kStreamWorkerPollInterval)) {}

StreamPollSchedule::Clock::time_point
StreamPollSchedule::next(std::optional<Clock::time_point> deadline) const noexcept {
    return deadline && *deadline < next_poll_ ? *deadline : next_poll_;
}

void StreamPollSchedule::advance(Clock::time_point now) noexcept {
    if (next_poll_ > now || next_poll_ == Clock::time_point::max()) {
        return;
    }
    const auto interval = static_cast<PollUnsignedRep>(
        std::chrono::duration_cast<Clock::duration>(kStreamWorkerPollInterval).count());
    const auto elapsed =
        distance(next_poll_.time_since_epoch().count(), now.time_since_epoch().count());
    const auto skipped = elapsed / interval + 1U;
    const auto maximum = std::numeric_limits<PollRep>::max();
    const auto room = distance(next_poll_.time_since_epoch().count(), maximum);
    if (skipped > room / interval) {
        next_poll_ = Clock::time_point::max();
        return;
    }
    next_poll_ = Clock::time_point{Clock::duration{
        add_nonnegative(next_poll_.time_since_epoch().count(), skipped * interval)}};
}

StreamItemView StreamIngressTestAccess::item(std::string_view first, std::string_view second,
                                             std::uint64_t sequence,
                                             StreamRoutingSidecar routing) noexcept {
    return {{first.data(), first.size()}, {second.data(), second.size()}, sequence, routing};
}

void StreamIngressTestAccess::set_tickets(StreamIngressHub& hub,
                                          const StreamIngressReservation& reservation,
                                          std::uint64_t descriptor_producer,
                                          std::uint64_t descriptor_consumer,
                                          std::uint64_t byte_producer,
                                          std::uint64_t byte_consumer) noexcept {
    if (!hub.impl_->valid(reservation)) {
        return;
    }
    auto& slot = hub.impl_->slots[reservation.index_];
    slot.descriptor_producer.store(descriptor_producer, std::memory_order_relaxed);
    slot.descriptor_consumer.store(descriptor_consumer, std::memory_order_relaxed);
    slot.byte_producer.store(byte_producer, std::memory_order_relaxed);
    slot.byte_consumer.store(byte_consumer, std::memory_order_relaxed);
}

void StreamIngressTestAccess::hold_publisher(StreamIngressHub& hub, bool hold) noexcept {
    hub.impl_->publisher_count.store(hold ? 1U : 0U, std::memory_order_seq_cst);
}

std::uint32_t StreamIngressTestAccess::publisher_count(const StreamIngressHub& hub) noexcept {
    return hub.impl_->publisher_count.load(std::memory_order_seq_cst);
}

std::size_t
StreamIngressTestAccess::slot_index(const StreamIngressReservation& reservation) noexcept {
    return reservation.index_;
}

void StreamIngressTestAccess::set_state_raw(StreamIngressHub& hub,
                                            const StreamIngressReservation& reservation,
                                            std::uint32_t state) noexcept {
    if (hub.impl_->valid(reservation)) {
        hub.impl_->slots[reservation.index_].state.store(state, std::memory_order_release);
    }
}

std::size_t StreamIngressTestAccess::retired_count(const StreamIngressHub& hub) noexcept {
    const std::lock_guard lock(hub.impl_->control);
    return static_cast<std::size_t>(
        std::count(hub.impl_->retired.begin(), hub.impl_->retired.end(), true));
}

bool StreamIngressTestAccess::reclaimed_state_is_poisoned(const StreamIngressHub& hub,
                                                          std::size_t index) noexcept {
    if (index >= kStreamSubscriberSlots ||
        load_state(hub.impl_->slots[index]) != StreamIngressState::Free) {
        return false;
    }
    const auto all_poison = [](const auto& value) {
        const auto bytes = std::as_bytes(std::span{&value, std::size_t{1}});
        return std::ranges::all_of(bytes, [](std::byte byte) {
            return std::to_integer<unsigned char>(byte) == kReclaimedPoison;
        });
    };
    const auto& slot = hub.impl_->slots[index];
    return all_poison(slot.staged) && all_poison(slot.projection) &&
           all_poison(slot.terminal_payload);
}

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

} // namespace tgcli::daemon
