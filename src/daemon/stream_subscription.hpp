#pragma once

#include "daemon/dispatch.hpp"
#include "daemon/stream_ingress.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace tgcli::daemon {

class RequestSession;

namespace detail {

class StreamSubscriptionState;
class StreamDeliveryRunner;

bool stream_subscription_claim(const std::shared_ptr<StreamSubscriptionState>& state,
                               StreamTerminalPayload payload) noexcept;
void stream_subscription_teardown(const std::shared_ptr<StreamSubscriptionState>& state) noexcept;

} // namespace detail

enum class StreamActivityMode : std::uint8_t { TrackedDaemon, UntrackedNoDaemon };

enum class StreamSubscriptionActivationFailure : std::uint8_t {
    RequestClosed,
    ActivityUnavailable,
    TerminalClaimed,
    PublicationFailed
};

struct StreamSubscriptionActivated {};

enum class StreamDeliveryStatus : std::uint8_t;
struct StreamDeliveryOptions;

class StreamSubscriptionLease {
  public:
    StreamSubscriptionLease() = default;
    ~StreamSubscriptionLease();
    StreamSubscriptionLease(const StreamSubscriptionLease&) = delete;
    StreamSubscriptionLease& operator=(const StreamSubscriptionLease&) = delete;
    StreamSubscriptionLease(StreamSubscriptionLease&& other) noexcept;
    StreamSubscriptionLease& operator=(StreamSubscriptionLease&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

  private:
    explicit StreamSubscriptionLease(std::shared_ptr<detail::StreamSubscriptionState> state);

    std::shared_ptr<detail::StreamSubscriptionState> state_;

    friend class RequestSession;
    friend class detail::StreamDeliveryRunner;
    friend StreamDeliveryStatus run_stream_delivery(RequestSession& session,
                                                    const StreamDeliveryOptions& options);
};

using StreamSubscriptionActivationResult =
    std::variant<StreamSubscriptionActivated, StreamIngressAdmissionFailure,
                 StreamIngressInvalidRequest, StreamSubscriptionActivationFailure>;

struct StreamTerminalResultFrame {
    nlohmann::json data = nlohmann::json::object();
};

struct StreamTerminalErrorFrame {
    std::string code;
    std::string message;
    nlohmann::json details = nlohmann::json::object();
    int exit_code = 1;
};

using StreamTerminalFrame =
    std::variant<std::monostate, StreamTerminalResultFrame, StreamTerminalErrorFrame>;
using StreamTerminalBuilder =
    std::function<StreamTerminalFrame(const StreamTerminalPayload&, std::uint64_t)>;

namespace testing {

enum class StreamActivationProbePoint : std::uint8_t { BeforeLifecycle, AfterPromotion };
using StreamActivationProbeHook = void (*)(void*, StreamActivationProbePoint) noexcept;
enum class StreamSubscriptionProbePoint : std::uint8_t {
    ClaimWaiting,
    ClaimOwned,
    ClaimForwarding,
    RetireWaiting,
    RetireOwned
};
using StreamSubscriptionProbeHook = void (*)(void*, StreamSubscriptionProbePoint) noexcept;

struct StreamActivationProbe {
    void* context = nullptr;
    StreamActivationProbeHook hook = nullptr;
    StreamSubscriptionProbeHook subscription_hook = nullptr;
};

struct StreamDeliveryHooks {
    using Clock = std::chrono::steady_clock;

    std::function<Clock::time_point()> now;
    std::function<void(Clock::time_point)> sleep_until;
};

} // namespace testing

struct StreamDeliveryOptions {
    std::optional<std::uint64_t> count;
    StreamTerminalBuilder terminal_builder;
    std::shared_ptr<const testing::StreamDeliveryHooks> hooks;
};

enum class StreamDeliveryStatus : std::uint8_t {
    TerminalComplete,
    Disconnected,
    Suppressed,
    InvalidLease
};

StreamDeliveryStatus run_stream_delivery(RequestSession& session,
                                         const StreamDeliveryOptions& options);

} // namespace tgcli::daemon
