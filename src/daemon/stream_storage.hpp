#pragma once

#include "core/td_runtime.hpp"
#include "daemon/stream_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace tgcli::daemon {

struct StreamEscapeResult {
    bool valid = false;
    std::size_t written_bytes = 0;
    std::size_t required_bytes = 0;
};

bool stream_timestamp_utc(std::int32_t seconds, std::span<char, 20> output) noexcept;
StreamEscapeResult stream_json_escape(std::string_view value, std::span<char> output) noexcept;

enum class StreamNormalizationPhase { Empty, Bootstrap, Ready, Failed };
enum class StreamFailureKind {
    None,
    MalformedSupported,
    WrongCurrentState,
    DirectConversion,
    TdlibError,
    RateLimited,
    DispatchFailure,
    Capacity
};

struct StreamFailure {
    StreamFailureKind kind = StreamFailureKind::None;
    core::TdSupportedUpdateKind update_kind = core::TdSupportedUpdateKind::CurrentStateEntry;
    core::TdMalformedUpdateReason malformed_reason = core::TdMalformedUpdateReason::MissingObject;
    std::int32_t tdlib_type_id = 0;
    std::int32_t tdlib_error_code = 0;
    std::int32_t retry_after = 0;
    std::uint32_t current_state_index = 0;
    StreamMetadataCapacityFailure capacity;

    bool operator==(const StreamFailure&) const = default;
};

struct StreamNormalizationStatus {
    std::int32_t client_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t receive_sequence = 0;
    StreamNormalizationPhase phase = StreamNormalizationPhase::Empty;
    bool ordering_barrier_open = false;
    StreamFailure failure;

    [[nodiscard]] bool ready_for_admission() const noexcept {
        return phase == StreamNormalizationPhase::Ready && !ordering_barrier_open;
    }
};

enum class StreamEventClass : std::uint8_t { Message, Edit, Delete, Reaction, Chat };
enum class StreamSenderKind : std::uint8_t { None, User, Chat };

constexpr std::uint8_t stream_event_mask(StreamEventClass event) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(event));
}

struct StreamRoutingSidecar {
    StreamEventClass event_class = StreamEventClass::Chat;
    std::int64_t chat_id = 0;
    StreamSenderKind sender_kind = StreamSenderKind::None;
    std::int64_t sender_id = 0;
    std::uint32_t json_offset = 0;
    std::uint32_t json_size = 0;
    std::uint32_t message_offset = 0;
    std::uint32_t message_size = 0;
    std::uint32_t text_offset = 0;
    std::uint32_t text_size = 0;

    bool operator==(const StreamRoutingSidecar&) const = default;
};

class StreamItemView {
  public:
    ~StreamItemView() = default;
    StreamItemView(const StreamItemView&) = delete;
    StreamItemView& operator=(const StreamItemView&) = delete;
    StreamItemView(StreamItemView&&) = delete;
    StreamItemView& operator=(StreamItemView&&) = delete;

    [[nodiscard]] std::array<std::span<const char>, 2> spans() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t receive_sequence() const noexcept;
    [[nodiscard]] const StreamRoutingSidecar& routing() const noexcept;

  private:
    StreamItemView(std::span<const char> first, std::span<const char> second,
                   std::uint64_t sequence, StreamRoutingSidecar routing) noexcept;

    std::span<const char> first_;
    std::span<const char> second_;
    std::uint64_t sequence_ = 0;
    StreamRoutingSidecar routing_;

    friend class FixedStreamNormalizer;
    friend class StreamIngressTestAccess;
};

enum class StreamMetadataChatKind { Private, BasicGroup, Supergroup, Channel };

struct StreamMetadataItemView {
    std::int64_t chat_id = 0;
    std::string_view title;
    StreamMetadataChatKind kind = StreamMetadataChatKind::Private;
    bool is_bot = false;
    std::size_t username_count = 0;
};

class StreamMetadataCursor {
  public:
    ~StreamMetadataCursor() = default;
    StreamMetadataCursor(const StreamMetadataCursor&) = delete;
    StreamMetadataCursor& operator=(const StreamMetadataCursor&) = delete;
    StreamMetadataCursor(StreamMetadataCursor&&) = delete;
    StreamMetadataCursor& operator=(StreamMetadataCursor&&) = delete;

    [[nodiscard]] bool next(StreamMetadataItemView& item) noexcept;
    [[nodiscard]] bool username(std::size_t index, std::string_view& value) const noexcept;

  private:
    StreamMetadataCursor(const void* owner, std::size_t position, std::uint64_t token) noexcept;

    const void* owner_ = nullptr;
    std::size_t position_ = 0;
    std::size_t current_ = 0;
    std::uint64_t token_ = 0;

    friend class StreamMetadataView;
};

class StreamMetadataView {
  public:
    ~StreamMetadataView() noexcept;
    StreamMetadataView(const StreamMetadataView&) = delete;
    StreamMetadataView& operator=(const StreamMetadataView&) = delete;
    StreamMetadataView(StreamMetadataView&&) = delete;
    StreamMetadataView& operator=(StreamMetadataView&&) = delete;

    [[nodiscard]] StreamMetadataCursor cursor() const noexcept;

  private:
    StreamMetadataView(void* owner, std::uint64_t token) noexcept;

    void* owner_ = nullptr;
    std::uint64_t token_ = 0;

    friend class FixedStreamNormalizer;
};

namespace detail {

enum class StreamStatusPublishPoint { WriterBegin, FailurePayload, Barrier, Sequence, Reset };
using StreamStatusPublishHook = void (*)(void*, StreamStatusPublishPoint) noexcept;

struct StreamStatusPublishProbe {
    void* context = nullptr;
    StreamStatusPublishHook hook = nullptr;
    std::uint64_t initial_revision = 0;
};

} // namespace detail

class StreamReceiveSink {
  public:
    StreamReceiveSink() = default;
    StreamReceiveSink(const StreamReceiveSink&) = delete;
    StreamReceiveSink& operator=(const StreamReceiveSink&) = delete;
    StreamReceiveSink(StreamReceiveSink&&) = delete;
    StreamReceiveSink& operator=(StreamReceiveSink&&) = delete;
    virtual ~StreamReceiveSink() = default;

    virtual void on_item(const StreamItemView& item,
                         const StreamMetadataView& metadata) noexcept = 0;
};

class FixedStreamNormalizer {
  public:
    class Impl;

    explicit FixedStreamNormalizer(StreamReceiveSink* sink = nullptr,
                                   detail::StreamStatusPublishProbe status_probe = {});
    ~FixedStreamNormalizer();
    FixedStreamNormalizer(const FixedStreamNormalizer&) = delete;
    FixedStreamNormalizer& operator=(const FixedStreamNormalizer&) = delete;
    FixedStreamNormalizer(FixedStreamNormalizer&&) = delete;
    FixedStreamNormalizer& operator=(FixedStreamNormalizer&&) = delete;

    bool begin(std::int32_t client_id, std::uint64_t generation) noexcept;
    void on_update(std::int32_t client_id, std::uint64_t generation,
                   const core::TdValue& update) noexcept;
    void on_current_state(std::int32_t client_id, std::uint64_t generation,
                          const core::TdValue& state) noexcept;
    void on_current_state_failure(std::int32_t client_id, std::uint64_t generation) noexcept;

    [[nodiscard]] StreamNormalizationStatus status() const noexcept;

  private:
    std::unique_ptr<Impl> impl_;

    friend class StreamMetadataCursor;
};

} // namespace tgcli::daemon
