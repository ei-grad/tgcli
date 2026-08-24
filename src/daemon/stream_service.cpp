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
        if (!service_.normalizer_.begin(client_id_, generation_)) {
            throw std::logic_error("invalid stream generation identity");
        }
    }

    ~GenerationObserver() override {
        service_.normalizer_.end(client_id_, generation_);
    }

    GenerationObserver(const GenerationObserver&) = delete;
    GenerationObserver& operator=(const GenerationObserver&) = delete;
    GenerationObserver(GenerationObserver&&) = delete;
    GenerationObserver& operator=(GenerationObserver&&) = delete;

    void on_update(const core::TdValue& update) noexcept override {
        const CallbackScope callback;
        service_.normalizer_.on_update(client_id_, generation_, update);
    }

    void on_current_state(const core::TdValue& state) noexcept override {
        const CallbackScope callback;
        service_.normalizer_.on_current_state(client_id_, generation_, state);
    }

    void on_current_state_failure(const std::exception_ptr& failure) noexcept override {
        static_cast<void>(failure);
        service_.normalizer_.on_current_state_failure(client_id_, generation_);
    }

  private:
    StreamService& service_;
    std::int32_t client_id_ = 0;
    std::uint64_t generation_ = 0;
};

StreamService::StreamService(StreamReceiveSink* sink) : normalizer_(sink) {}

StreamService::~StreamService() = default;

core::TdGenerationObserverFactory StreamService::observer_factory() noexcept {
    return [this](std::int32_t client_id, std::uint64_t generation) {
        return std::make_unique<GenerationObserver>(*this, client_id, generation);
    };
}

StreamNormalizationStatus StreamService::status() const noexcept {
    return normalizer_.status();
}

} // namespace tgcli::daemon
