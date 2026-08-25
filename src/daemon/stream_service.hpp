#pragma once

#include "core/td_client.hpp"
#include "daemon/stream_ingress.hpp"
#include "daemon/stream_storage.hpp"

namespace tgcli::daemon {

namespace detail {

[[nodiscard]] bool stream_callback_active() noexcept;

} // namespace detail

class StreamService final : private StreamReceiveSink {
  public:
    explicit StreamService(StreamReceiveSink* sink = nullptr,
                           detail::StreamStatusPublishProbe status_probe = {});
    ~StreamService() override;
    StreamService(const StreamService&) = delete;
    StreamService& operator=(const StreamService&) = delete;
    StreamService(StreamService&&) = delete;
    StreamService& operator=(StreamService&&) = delete;

    [[nodiscard]] core::TdGenerationObserverFactory observer_factory() noexcept;
    [[nodiscard]] StreamNormalizationStatus status() const noexcept;
    [[nodiscard]] StreamIngressHub& ingress_hub() noexcept;
    void claim_shutdown() noexcept;

  private:
    class GenerationObserver;
    void on_item(const StreamItemView& item, const StreamMetadataView& metadata) noexcept override;

    StreamReceiveSink* external_sink_ = nullptr;
    StreamIngressHub hub_;
    FixedStreamNormalizer normalizer_;

    friend class GenerationObserver;
};

} // namespace tgcli::daemon
