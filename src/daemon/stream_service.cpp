#include "daemon/stream_service.hpp"

#include <stdexcept>
#include <utility>

namespace tgcli::daemon {

namespace {

std::uint32_t& callback_depth() noexcept {
    static thread_local std::uint32_t value = 0;
    return value;
}

class CallbackScope {
  public:
    CallbackScope() noexcept {
        ++callback_depth();
    }

    ~CallbackScope() {
        --callback_depth();
    }

    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    CallbackScope(CallbackScope&&) = delete;
    CallbackScope& operator=(CallbackScope&&) = delete;
};

} // namespace

namespace detail {

bool stream_callback_active() noexcept {
    return callback_depth() != 0;
}

} // namespace detail

class StreamService::GenerationObserver final : public core::TdGenerationObserver {
  public:
    GenerationObserver(StreamService& service, std::int32_t client_id, std::uint64_t generation)
        : service_(service), client_id_(client_id), generation_(generation) {
        service_.hub_->begin_generation(client_id_, generation_);
        if (!service_.normalizer_.begin(client_id_, generation_)) {
            throw std::logic_error("invalid stream generation identity");
        }
    }

    ~GenerationObserver() override = default;

    GenerationObserver(const GenerationObserver&) = delete;
    GenerationObserver& operator=(const GenerationObserver&) = delete;
    GenerationObserver(GenerationObserver&&) = delete;
    GenerationObserver& operator=(GenerationObserver&&) = delete;

    void on_update(const core::TdValue& update) noexcept override {
        const CallbackScope callback;
        service_.normalizer_.on_update(client_id_, generation_, update);
        claim_metadata_failure();
    }

    void on_current_state(const core::TdValue& state) noexcept override {
        const CallbackScope callback;
        service_.normalizer_.on_current_state(client_id_, generation_, state);
        claim_metadata_failure();
    }

    void on_current_state_failure(const std::exception_ptr& failure) noexcept override {
        const CallbackScope callback;
        static_cast<void>(failure);
        service_.normalizer_.on_current_state_failure(client_id_, generation_);
        claim_metadata_failure();
    }

    void on_authorization_state(const core::AuthStateData& state,
                                std::uint64_t receive_sequence) noexcept override {
        const CallbackScope callback;
        authorization_sequence_ = receive_sequence;
        if (state.state == core::AuthState::Ready) {
            ready_seen_ = true;
            return;
        }
        if (!ready_seen_ || authorization_lost_) {
            return;
        }
        authorization_lost_ = true;
        service_.hub_->claim_generation(client_id_, generation_,
                                        {.cause = StreamTerminalCause::AuthorizationLost,
                                         .auth_state = static_cast<std::int32_t>(state.state),
                                         .metadata_failure = {}});
    }

    void on_receive_boundary(std::uint64_t receive_sequence) noexcept override {
        const CallbackScope callback;
        const auto status = service_.normalizer_.status();
        if (status.client_id != client_id_ || status.generation != generation_) {
            return;
        }
        if (status.phase == StreamNormalizationPhase::Failed) {
            claim_metadata_failure(status);
            return;
        }
        const bool ready = ready_seen_ && !authorization_lost_ && status.ready_for_admission() &&
                           receive_sequence >= status.receive_sequence &&
                           receive_sequence >= authorization_sequence_;
        static_cast<void>(
            service_.hub_->activate_armed(client_id_, generation_, receive_sequence, ready));
    }

  private:
    void claim_metadata_failure() noexcept {
        claim_metadata_failure(service_.normalizer_.status());
    }

    void claim_metadata_failure(const StreamNormalizationStatus& status) noexcept {
        if (status.phase == StreamNormalizationPhase::Failed) {
            service_.hub_->claim_generation(client_id_, generation_,
                                            {.cause = StreamTerminalCause::MetadataFailure,
                                             .metadata_failure = status.failure});
        }
    }

    StreamService& service_;
    std::int32_t client_id_ = 0;
    std::uint64_t generation_ = 0;
    bool ready_seen_ = false;
    bool authorization_lost_ = false;
    std::uint64_t authorization_sequence_ = 0;
};

StreamService::StreamService(StreamReceiveSink* sink, detail::StreamStatusPublishProbe status_probe)
    : external_sink_(sink), hub_(std::make_shared<StreamIngressHub>()),
      normalizer_(this, status_probe) {}

StreamService::~StreamService() = default;

core::TdGenerationObserverFactory StreamService::observer_factory() noexcept {
    return [this](std::int32_t client_id, std::uint64_t generation) {
        return std::make_unique<GenerationObserver>(*this, client_id, generation);
    };
}

StreamNormalizationStatus StreamService::status() const noexcept {
    return normalizer_.status();
}

StreamIngressHub& StreamService::ingress_hub() noexcept {
    return *hub_;
}

std::shared_ptr<StreamIngressHub> StreamService::ingress_hub_handle() const noexcept {
    return hub_;
}

void StreamService::claim_shutdown() noexcept {
    const auto current = normalizer_.status();
    hub_->claim_control_generation(
        current.client_id, current.generation,
        {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
}

void StreamService::on_item(const StreamItemView& item,
                            const StreamMetadataView& metadata) noexcept {
    hub_->publish(item);
    if (external_sink_ != nullptr) {
        external_sink_->on_item(item, metadata);
    }
}

} // namespace tgcli::daemon
