#pragma once

#include "core/td_client.hpp"
#include "daemon/stream_storage.hpp"

namespace tgcli::daemon {

namespace detail {

[[nodiscard]] bool stream_callback_active() noexcept;

} // namespace detail

class StreamService {
  public:
    explicit StreamService(StreamReceiveSink* sink = nullptr);
    ~StreamService();
    StreamService(const StreamService&) = delete;
    StreamService& operator=(const StreamService&) = delete;
    StreamService(StreamService&&) = delete;
    StreamService& operator=(StreamService&&) = delete;

    [[nodiscard]] core::TdGenerationObserverFactory observer_factory() noexcept;
    [[nodiscard]] StreamNormalizationStatus status() const noexcept;

  private:
    class GenerationObserver;
    FixedStreamNormalizer normalizer_;

    friend class GenerationObserver;
};

} // namespace tgcli::daemon
