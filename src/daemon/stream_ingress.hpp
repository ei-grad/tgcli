#pragma once

#include "daemon/stream_limits.hpp"
#include "daemon/stream_storage.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace tgcli::daemon {

enum class StreamOperation : std::uint8_t { Listen, WaitFor };
enum class StreamMode : std::uint8_t { Items, Match };
enum class StreamIngressState : std::uint8_t {
    Free,
    Reserved,
    Armed,
    Installing,
    Published,
    Removing,
    Removed,
    Reclaimable
};

enum class StreamIngressAdmissionResource : std::uint8_t { SubscriberSlots, LockFreeIngress };
enum class StreamIngressAtomic : std::uint8_t {
    SlotPointer,
    PublisherCount,
    DescriptorIndex,
    ByteIndex,
    TerminalCause
};

struct StreamIngressAdmissionFailure {
    StreamIngressAdmissionResource resource = StreamIngressAdmissionResource::SubscriberSlots;
    StreamIngressAtomic atomic = StreamIngressAtomic::SlotPointer;
};

struct StreamIngressRequest {
    std::int32_t client_id = 0;
    std::uint64_t generation = 0;
    StreamOperation operation = StreamOperation::Listen;
    StreamMode mode = StreamMode::Items;
    std::uint8_t type_mask = 0;
    std::array<std::int64_t, kStreamChatFilters> chat_ids{};
    std::uint8_t chat_count = 0;
};

struct StreamActivationProjection {
    std::int32_t client_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t activation_receive_sequence = 0;
    std::array<std::int64_t, kStreamChatFilters> chat_ids{};
    std::uint8_t chat_count = 0;
    std::uint8_t type_mask = 0;
    StreamMode mode = StreamMode::Items;
    StreamOperation operation = StreamOperation::Listen;
};

enum class StreamTerminalCause : std::uint32_t {
    Open,
    ClaimingCounterExhausted,
    ClaimingItemBytes,
    ClaimingQueueItems,
    ClaimingQueueBytes,
    ClaimingAuthorizationLost,
    ClaimingGenerationReplaced,
    ClaimingShutdown,
    ClaimingMetadataFailure,
    CounterExhausted,
    ItemBytes,
    QueueItems,
    QueueBytes,
    AuthorizationLost,
    GenerationReplaced,
    Shutdown,
    MetadataFailure
};

struct StreamTerminalPayload {
    StreamTerminalCause cause = StreamTerminalCause::Open;
    StreamOperation operation = StreamOperation::Listen;
    std::uint64_t limit_items = kStreamQueueItems;
    std::uint64_t limit_bytes = kStreamQueueBytes;
    std::uint64_t queued_items = 0;
    std::uint64_t queued_bytes = 0;
    std::uint64_t incoming_bytes = 0;
    std::int32_t auth_state = 0;
    StreamFailure metadata_failure;
};

struct StreamIngressDescriptor {
    std::uint64_t byte_ticket = 0;
    std::uint64_t receive_sequence = 0;
    std::int64_t chat_id = 0;
    std::int64_t sender_id = 0;
    std::uint32_t json_offset = 0;
    std::uint32_t json_size = 0;
    std::uint32_t message_offset = 0;
    std::uint32_t message_size = 0;
    std::uint32_t text_offset = 0;
    std::uint32_t text_size = 0;
    StreamEventClass event_class = StreamEventClass::Chat;
    StreamSenderKind sender_kind = StreamSenderKind::None;
    std::array<std::byte, 6> reserved{};
};

static_assert(sizeof(StreamIngressDescriptor) == 64);

struct StreamIngressSlot;

class StreamIngressItemView {
  public:
    ~StreamIngressItemView() = default;
    StreamIngressItemView(StreamIngressItemView&&) noexcept = default;
    StreamIngressItemView& operator=(StreamIngressItemView&&) noexcept = default;
    StreamIngressItemView(const StreamIngressItemView&) = delete;
    StreamIngressItemView& operator=(const StreamIngressItemView&) = delete;

    [[nodiscard]] const StreamIngressDescriptor& descriptor() const noexcept;
    [[nodiscard]] std::array<std::span<const char>, 2> spans() const noexcept;

  private:
    StreamIngressItemView(StreamIngressDescriptor descriptor, std::span<const char> first,
                          std::span<const char> second) noexcept;

    StreamIngressDescriptor descriptor_;
    std::span<const char> first_;
    std::span<const char> second_;

    friend class StreamIngressHub;
};

class StreamIngressHub;

class StreamIngressReservation {
  public:
    StreamIngressReservation();
    ~StreamIngressReservation();
    StreamIngressReservation(const StreamIngressReservation&) = delete;
    StreamIngressReservation& operator=(const StreamIngressReservation&) = delete;
    StreamIngressReservation(StreamIngressReservation&& other) noexcept;
    StreamIngressReservation& operator=(StreamIngressReservation&& other) noexcept;

    explicit operator bool() const noexcept;

  private:
    StreamIngressReservation(StreamIngressHub* hub, std::uint32_t index,
                             std::uint64_t epoch) noexcept;
    void reset() noexcept;

    StreamIngressHub* hub_ = nullptr;
    std::uint32_t index_ = 0;
    std::uint64_t epoch_ = 0;

    friend class StreamIngressHub;
    friend class StreamIngressTestAccess;
};

namespace detail {

enum class StreamIngressProbePoint {
    Publication,
    PublisherIncrement,
    SlotLoad,
    PublisherDecrement,
    SlotExchange,
    ReclaimZero
};
using StreamIngressProbeHook = void (*)(void*, StreamIngressProbePoint, std::size_t) noexcept;

struct StreamIngressProbe {
    void* context = nullptr;
    StreamIngressProbeHook hook = nullptr;
    std::optional<StreamIngressAtomic> forced_lock_free_failure;
};

} // namespace detail

class StreamIngressHub {
  public:
    class Impl;

    explicit StreamIngressHub(detail::StreamIngressProbe probe = {});
    ~StreamIngressHub();
    StreamIngressHub(const StreamIngressHub&) = delete;
    StreamIngressHub& operator=(const StreamIngressHub&) = delete;
    StreamIngressHub(StreamIngressHub&&) = delete;
    StreamIngressHub& operator=(StreamIngressHub&&) = delete;

    [[nodiscard]] std::optional<StreamIngressAtomic> lock_free_failure() const noexcept;
    [[nodiscard]] std::optional<StreamIngressReservation>
    reserve(const StreamIngressRequest& request);
    [[nodiscard]] std::optional<StreamIngressAdmissionFailure>
    last_reservation_failure() const noexcept;
    void begin_generation(std::int32_t client_id, std::uint64_t generation) noexcept;
    void claim_generation(std::int32_t client_id, std::uint64_t generation,
                          StreamTerminalPayload payload) noexcept;
    bool commit_activation(StreamIngressReservation& reservation) noexcept;
    [[nodiscard]] StreamIngressState
    activation_state(const StreamIngressReservation& reservation) const noexcept;
    [[nodiscard]] std::optional<StreamActivationProjection>
    activation_projection(const StreamIngressReservation& reservation) const noexcept;
    std::size_t activate_armed(std::int32_t client_id, std::uint64_t generation,
                               std::uint64_t receive_sequence, bool ready = true) noexcept;

    void publish(const StreamItemView& item) noexcept;
    [[nodiscard]] std::optional<StreamIngressItemView>
    poll_front(const StreamIngressReservation& reservation) noexcept;
    bool consume(const StreamIngressReservation& reservation,
                 const StreamIngressItemView& item) noexcept;
    void discard(const StreamIngressReservation& reservation) noexcept;

    bool claim(StreamIngressReservation& reservation, StreamTerminalPayload payload) noexcept;
    [[nodiscard]] std::optional<StreamTerminalPayload>
    claim_terminal(StreamIngressReservation& reservation) noexcept;
    [[nodiscard]] std::optional<StreamTerminalPayload>
    terminal_snapshot(const StreamIngressReservation& reservation) const noexcept;
    bool detach(StreamIngressReservation& reservation) noexcept;
    bool poll_reclaim(StreamIngressReservation& reservation) noexcept;

  private:
    void abandon(StreamIngressReservation& reservation) noexcept;
    std::unique_ptr<Impl> impl_;

    friend class StreamIngressReservation;
    friend class StreamIngressTestAccess;
};

class StreamPollSchedule {
  public:
    using Clock = std::chrono::steady_clock;

    explicit StreamPollSchedule(Clock::time_point now) noexcept;
    [[nodiscard]] Clock::time_point next(std::optional<Clock::time_point> deadline) const noexcept;
    void advance(Clock::time_point now) noexcept;

  private:
    Clock::time_point next_poll_;
};

class StreamIngressTestAccess {
  public:
    static StreamItemView item(std::string_view first, std::string_view second,
                               std::uint64_t sequence, StreamRoutingSidecar routing) noexcept;
    static void set_tickets(StreamIngressHub& hub, const StreamIngressReservation& reservation,
                            std::uint64_t descriptor_producer, std::uint64_t descriptor_consumer,
                            std::uint64_t byte_producer, std::uint64_t byte_consumer) noexcept;
    static void hold_publisher(StreamIngressHub& hub, bool hold) noexcept;
};

} // namespace tgcli::daemon
