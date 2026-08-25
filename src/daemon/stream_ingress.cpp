#include "daemon/stream_ingress.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace tgcli::daemon {

// Fixed rings use validated monotonic tickets and checked modulo indices on the callback path.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)

namespace {

constexpr unsigned char kReclaimedPoison = 0xA7;

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
    case StreamTerminalCause::Open:
    case StreamTerminalCause::ClaimingCounterExhausted:
    case StreamTerminalCause::ClaimingItemBytes:
    case StreamTerminalCause::ClaimingQueueItems:
    case StreamTerminalCause::ClaimingQueueBytes:
    case StreamTerminalCause::ClaimingAuthorizationLost:
    case StreamTerminalCause::ClaimingGenerationReplaced:
    case StreamTerminalCause::ClaimingShutdown:
    case StreamTerminalCause::ClaimingMetadataFailure:
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

} // namespace

struct StreamIngressSlot {
    using DescriptorStorage = std::array<StreamIngressDescriptor, kStreamQueueItems>;
    using ByteStorage = std::array<char, kStreamQueueBytes>;

    std::atomic<StreamIngressState> state{StreamIngressState::Free};
    std::uint64_t epoch = 0;
    StreamIngressRequest staged;
    StreamActivationProjection projection;
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

static_assert(std::atomic<StreamIngressSlot*>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<StreamTerminalCause>::is_always_lock_free);

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
        const std::atomic<std::uint32_t> count;
        const std::atomic<StreamTerminalCause> terminal;
        if (!pointer.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::SlotPointer;
        } else if (!count.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::PublisherCount;
        } else if (!slots.front().descriptor_producer.is_lock_free() ||
                   !slots.front().descriptor_consumer.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::DescriptorIndex;
        } else if (!slots.front().byte_producer.is_lock_free() ||
                   !slots.front().byte_consumer.is_lock_free()) {
            lock_free_failure = StreamIngressAtomic::ByteIndex;
        } else if (!terminal.is_lock_free()) {
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
            owner_.publisher_count.fetch_add(1, std::memory_order_seq_cst);
            owner_.notify(detail::StreamIngressProbePoint::PublisherIncrement);
        }

        ~PublisherGuard() {
            owner_.notify(detail::StreamIngressProbePoint::PublisherDecrement);
            owner_.publisher_count.fetch_sub(1, std::memory_order_seq_cst);
        }

        PublisherGuard(const PublisherGuard&) = delete;
        PublisherGuard& operator=(const PublisherGuard&) = delete;
        PublisherGuard(PublisherGuard&&) = delete;
        PublisherGuard& operator=(PublisherGuard&&) = delete;

      private:
        Impl& owner_;
    };

    bool valid(const StreamIngressReservation& reservation) const noexcept {
        return reservation.hub_ != nullptr && reservation.index_ < kStreamSubscriberSlots &&
               slots[reservation.index_].epoch == reservation.epoch_;
    }

    static bool claim(StreamIngressSlot& slot, StreamTerminalPayload payload) noexcept {
        const auto claiming = claiming_cause(payload.cause);
        if (claiming == StreamTerminalCause::Open) {
            return false;
        }
        auto expected = StreamTerminalCause::Open;
        if (!slot.terminal_cause.compare_exchange_strong(
                expected, claiming, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }
        slot.terminal_payload = payload;
        slot.terminal_cause.store(payload.cause, std::memory_order_release);
        return true;
    }

    static void overflow(StreamIngressSlot& slot, StreamTerminalCause cause,
                         std::uint64_t queued_items, std::uint64_t queued_bytes,
                         std::uint64_t incoming_bytes) noexcept {
        static_cast<void>(claim(slot, {.cause = cause,
                                       .operation = slot.projection.operation,
                                       .queued_items = queued_items,
                                       .queued_bytes = queued_bytes,
                                       .incoming_bytes = incoming_bytes}));
    }

    static bool matches(const StreamIngressSlot& slot, const StreamItemView& item) noexcept {
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

    std::array<StreamIngressSlot, kStreamSubscriberSlots> slots;
    std::array<std::atomic<StreamIngressSlot*>, kStreamSubscriberSlots> dormant;
    std::array<std::atomic<StreamIngressSlot*>, kStreamSubscriberSlots> published;
    std::atomic<std::uint32_t> publisher_count{0};
    mutable std::mutex control;
    std::optional<StreamIngressAtomic> lock_free_failure;
    std::optional<StreamIngressAdmissionFailure> last_failure;
    detail::StreamIngressProbe probe;
};

StreamIngressItemView::StreamIngressItemView(StreamIngressDescriptor descriptor,
                                             std::span<const char> first,
                                             std::span<const char> second) noexcept
    : descriptor_(descriptor), first_(first), second_(second) {}

const StreamIngressDescriptor& StreamIngressItemView::descriptor() const noexcept {
    return descriptor_;
}

std::array<std::span<const char>, 2> StreamIngressItemView::spans() const noexcept {
    return {first_, second_};
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

void StreamIngressReservation::reset() noexcept {
    if (hub_ != nullptr) {
        hub_->abandon(*this);
    }
}

StreamIngressHub::StreamIngressHub(detail::StreamIngressProbe probe)
    : impl_(std::make_unique<Impl>(probe)) {}

StreamIngressHub::~StreamIngressHub() = default;

std::optional<StreamIngressAtomic> StreamIngressHub::lock_free_failure() const noexcept {
    return impl_->lock_free_failure;
}

std::optional<StreamIngressReservation>
StreamIngressHub::reserve(const StreamIngressRequest& request) {
    const std::lock_guard lock(impl_->control);
    impl_->last_failure.reset();
    if (impl_->lock_free_failure) {
        impl_->last_failure = StreamIngressAdmissionFailure{
            .resource = StreamIngressAdmissionResource::LockFreeIngress,
            .atomic = impl_->lock_free_failure.value_or(StreamIngressAtomic::SlotPointer)};
        return std::nullopt;
    }
    if (!valid_request(request)) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto& slot = impl_->slots[index];
        if (slot.state.load(std::memory_order_acquire) != StreamIngressState::Free) {
            continue;
        }
        if (slot.descriptors == nullptr) {
            slot.descriptors =
                std::make_unique_for_overwrite<StreamIngressSlot::DescriptorStorage>();
            slot.bytes = std::make_unique_for_overwrite<StreamIngressSlot::ByteStorage>();
        }
        ++slot.epoch;
        if (slot.epoch == 0) {
            continue;
        }
        slot.staged = request;
        slot.projection = {};
        slot.descriptor_producer.store(0, std::memory_order_relaxed);
        slot.descriptor_consumer.store(0, std::memory_order_relaxed);
        slot.byte_producer.store(0, std::memory_order_relaxed);
        slot.byte_consumer.store(0, std::memory_order_relaxed);
        slot.terminal_payload = {};
        slot.terminal_delivered = false;
        slot.terminal_cause.store(StreamTerminalCause::Open, std::memory_order_relaxed);
        slot.state.store(StreamIngressState::Reserved, std::memory_order_release);
        return StreamIngressReservation(this, static_cast<std::uint32_t>(index), slot.epoch);
    }
    impl_->last_failure = {.resource = StreamIngressAdmissionResource::SubscriberSlots,
                           .atomic = StreamIngressAtomic::SlotPointer};
    return std::nullopt;
}

std::optional<StreamIngressAdmissionFailure>
StreamIngressHub::last_reservation_failure() const noexcept {
    const std::lock_guard lock(impl_->control);
    return impl_->last_failure;
}

bool StreamIngressHub::commit_activation(StreamIngressReservation& reservation) noexcept {
    const std::lock_guard lock(impl_->control);
    if (!impl_->valid(reservation)) {
        return false;
    }
    auto& slot = impl_->slots[reservation.index_];
    auto expected = StreamIngressState::Reserved;
    if (!slot.state.compare_exchange_strong(expected, StreamIngressState::Armed,
                                            std::memory_order_acq_rel)) {
        return false;
    }
    impl_->dormant[reservation.index_].store(&slot, std::memory_order_seq_cst);
    return true;
}

StreamIngressState
StreamIngressHub::activation_state(const StreamIngressReservation& reservation) const noexcept {
    if (!impl_->valid(reservation)) {
        return StreamIngressState::Free;
    }
    return impl_->slots[reservation.index_].state.load(std::memory_order_acquire);
}

std::optional<StreamActivationProjection> StreamIngressHub::activation_projection(
    const StreamIngressReservation& reservation) const noexcept {
    if (!impl_->valid(reservation) ||
        impl_->slots[reservation.index_].state.load(std::memory_order_acquire) !=
            StreamIngressState::Published) {
        return std::nullopt;
    }
    return impl_->slots[reservation.index_].projection;
}

std::size_t StreamIngressHub::activate_armed(std::int32_t client_id, std::uint64_t generation,
                                             std::uint64_t receive_sequence) noexcept {
    std::size_t activated = 0;
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto* slot = impl_->dormant[index].load(std::memory_order_seq_cst);
        if (slot == nullptr || slot->staged.client_id != client_id ||
            slot->staged.generation != generation) {
            continue;
        }
        auto expected = StreamIngressState::Armed;
        if (!slot->state.compare_exchange_strong(expected, StreamIngressState::Installing,
                                                 std::memory_order_acq_rel)) {
            continue;
        }
        slot->projection = {.client_id = client_id,
                            .generation = generation,
                            .activation_receive_sequence = receive_sequence,
                            .chat_ids = slot->staged.chat_ids,
                            .chat_count = slot->staged.chat_count,
                            .type_mask = slot->staged.type_mask,
                            .mode = slot->staged.mode,
                            .operation = slot->staged.operation};
        if (slot->terminal_cause.load(std::memory_order_acquire) != StreamTerminalCause::Open) {
            slot->state.store(StreamIngressState::Removing, std::memory_order_release);
            impl_->dormant[index].store(nullptr, std::memory_order_seq_cst);
            slot->state.store(StreamIngressState::Removed, std::memory_order_release);
            continue;
        }
        impl_->dormant[index].store(nullptr, std::memory_order_seq_cst);
        impl_->published[index].store(slot, std::memory_order_seq_cst);
        impl_->notify(detail::StreamIngressProbePoint::Publication, index);
        slot->state.store(StreamIngressState::Published, std::memory_order_release);
        ++activated;
    }
    return activated;
}

void StreamIngressHub::publish(const StreamItemView& item) noexcept {
    const Impl::PublisherGuard publisher(*impl_);
    for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
        auto* slot = impl_->published[index].load(std::memory_order_seq_cst);
        impl_->notify(detail::StreamIngressProbePoint::SlotLoad, index);
        if (slot != nullptr) {
            impl_->enqueue(*slot, item);
        }
    }
}

std::optional<StreamIngressItemView>
StreamIngressHub::poll_front(const StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        return std::nullopt;
    }
    auto& slot = impl_->slots[reservation.index_];
    if (slot.terminal_cause.load(std::memory_order_acquire) != StreamTerminalCause::Open) {
        return std::nullopt;
    }
    const auto consumer = slot.descriptor_consumer.load(std::memory_order_relaxed);
    const auto producer = slot.descriptor_producer.load(std::memory_order_acquire);
    if (consumer == producer) {
        return std::nullopt;
    }
    const auto descriptor = (*slot.descriptors)[consumer % kStreamQueueItems];
    const auto physical = static_cast<std::size_t>(descriptor.byte_ticket % kStreamQueueBytes);
    const auto first_size =
        std::min<std::size_t>(descriptor.json_size, kStreamQueueBytes - physical);
    return StreamIngressItemView(descriptor, {slot.bytes->data() + physical, first_size},
                                 {slot.bytes->data(), descriptor.json_size - first_size});
}

bool StreamIngressHub::consume(const StreamIngressReservation& reservation,
                               const StreamIngressItemView& item) noexcept {
    if (!impl_->valid(reservation)) {
        return false;
    }
    auto& slot = impl_->slots[reservation.index_];
    const auto consumer = slot.descriptor_consumer.load(std::memory_order_relaxed);
    const auto producer = slot.descriptor_producer.load(std::memory_order_acquire);
    if (consumer == producer || (*slot.descriptors)[consumer % kStreamQueueItems].byte_ticket !=
                                    item.descriptor().byte_ticket) {
        return false;
    }
    const auto physical =
        static_cast<std::size_t>(item.descriptor().byte_ticket % kStreamQueueBytes);
    const auto first_size =
        std::min<std::size_t>(item.descriptor().json_size, kStreamQueueBytes - physical);
    if (first_size != 0) {
        std::memset(slot.bytes->data() + physical, kReclaimedPoison, first_size);
    }
    if (item.descriptor().json_size > first_size) {
        std::memset(slot.bytes->data(), kReclaimedPoison, item.descriptor().json_size - first_size);
    }
    slot.byte_consumer.store(item.descriptor().byte_ticket + item.descriptor().json_size,
                             std::memory_order_release);
    slot.descriptor_consumer.store(consumer + 1, std::memory_order_release);
    return true;
}

void StreamIngressHub::discard(const StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        return;
    }
    auto& slot = impl_->slots[reservation.index_];
    slot.descriptor_consumer.store(slot.descriptor_producer.load(std::memory_order_acquire),
                                   std::memory_order_release);
    slot.byte_consumer.store(slot.byte_producer.load(std::memory_order_acquire),
                             std::memory_order_release);
}

bool StreamIngressHub::claim(StreamIngressReservation& reservation,
                             StreamTerminalPayload payload) noexcept {
    return impl_->valid(reservation) && impl_->claim(impl_->slots[reservation.index_], payload);
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
    auto state = slot.state.load(std::memory_order_acquire);
    while (state == StreamIngressState::Armed || state == StreamIngressState::Published) {
        if (slot.state.compare_exchange_weak(state, StreamIngressState::Removing,
                                             std::memory_order_acq_rel)) {
            impl_->published[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
            impl_->dormant[reservation.index_].exchange(nullptr, std::memory_order_seq_cst);
            impl_->notify(detail::StreamIngressProbePoint::SlotExchange, reservation.index_);
            slot.state.store(StreamIngressState::Removed, std::memory_order_release);
            return true;
        }
    }
    return state == StreamIngressState::Removed || state == StreamIngressState::Reclaimable;
}

bool StreamIngressHub::poll_reclaim(StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        return false;
    }
    auto& slot = impl_->slots[reservation.index_];
    auto expected = StreamIngressState::Removed;
    if (slot.state.load(std::memory_order_acquire) != StreamIngressState::Removed ||
        impl_->publisher_count.load(std::memory_order_seq_cst) != 0) {
        return false;
    }
    impl_->notify(detail::StreamIngressProbePoint::ReclaimZero, reservation.index_);
    if (!slot.state.compare_exchange_strong(expected, StreamIngressState::Reclaimable,
                                            std::memory_order_acq_rel)) {
        return false;
    }
    const std::lock_guard lock(impl_->control);
    if (!impl_->valid(reservation)) {
        return false;
    }
    slot.descriptor_producer.store(0, std::memory_order_relaxed);
    slot.descriptor_consumer.store(0, std::memory_order_relaxed);
    slot.byte_producer.store(0, std::memory_order_relaxed);
    slot.byte_consumer.store(0, std::memory_order_relaxed);
    slot.terminal_cause.store(StreamTerminalCause::Open, std::memory_order_relaxed);
    slot.state.store(StreamIngressState::Free, std::memory_order_release);
    reservation.hub_ = nullptr;
    return true;
}

void StreamIngressHub::abandon(StreamIngressReservation& reservation) noexcept {
    if (!impl_->valid(reservation)) {
        reservation.hub_ = nullptr;
        return;
    }
    auto& slot = impl_->slots[reservation.index_];
    const auto state = slot.state.load(std::memory_order_acquire);
    if (state == StreamIngressState::Reserved) {
        const std::lock_guard lock(impl_->control);
        if (impl_->valid(reservation) &&
            slot.state.load(std::memory_order_acquire) == StreamIngressState::Reserved) {
            slot.state.store(StreamIngressState::Free, std::memory_order_release);
        }
        reservation.hub_ = nullptr;
        return;
    }
    static_cast<void>(detach(reservation));
    static_cast<void>(poll_reclaim(reservation));
    reservation.hub_ = nullptr;
}

StreamPollSchedule::StreamPollSchedule(Clock::time_point now) noexcept
    : next_poll_(now + kStreamWorkerPollInterval) {}

StreamPollSchedule::Clock::time_point
StreamPollSchedule::next(std::optional<Clock::time_point> deadline) const noexcept {
    return deadline && *deadline < next_poll_ ? *deadline : next_poll_;
}

void StreamPollSchedule::advance(Clock::time_point now) noexcept {
    if (next_poll_ > now) {
        return;
    }
    const auto elapsed = now - next_poll_;
    const auto skipped = elapsed / kStreamWorkerPollInterval + 1;
    next_poll_ += kStreamWorkerPollInterval * skipped;
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

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

} // namespace tgcli::daemon
