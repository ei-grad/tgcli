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

class StreamItemView {
  public:
    [[nodiscard]] std::array<std::span<const char>, 2> spans() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t receive_sequence() const noexcept;

  private:
    StreamItemView(std::span<const char> first, std::span<const char> second,
                   std::uint64_t sequence) noexcept;

    std::span<const char> first_;
    std::span<const char> second_;
    std::uint64_t sequence_ = 0;

    friend class FixedStreamNormalizer;
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
    [[nodiscard]] bool next(StreamMetadataItemView& item) noexcept;
    [[nodiscard]] bool username(std::size_t index, std::string_view& value) const noexcept;

  private:
    StreamMetadataCursor(const void* owner, std::size_t position) noexcept;

    const void* owner_ = nullptr;
    std::size_t position_ = 0;
    std::size_t current_ = 0;

    friend class StreamMetadataView;
};

class StreamMetadataView {
  public:
    explicit StreamMetadataView(const void* owner) noexcept;
    [[nodiscard]] StreamMetadataCursor cursor() const noexcept;

  private:
    const void* owner_ = nullptr;

    friend class FixedStreamNormalizer;
};

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

    explicit FixedStreamNormalizer(StreamReceiveSink* sink = nullptr);
    ~FixedStreamNormalizer();
    FixedStreamNormalizer(const FixedStreamNormalizer&) = delete;
    FixedStreamNormalizer& operator=(const FixedStreamNormalizer&) = delete;
    FixedStreamNormalizer(FixedStreamNormalizer&&) = delete;
    FixedStreamNormalizer& operator=(FixedStreamNormalizer&&) = delete;

    bool begin(std::int32_t client_id, std::uint64_t generation) noexcept;
    void end(std::int32_t client_id, std::uint64_t generation) noexcept;
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
