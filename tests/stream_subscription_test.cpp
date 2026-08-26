#include "common/exit_codes.hpp"
#include "daemon/request_session.hpp"
#include "daemon/stream_subscription.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using namespace tgcli;
using namespace tgcli::daemon;
using nlohmann::json;

namespace {

proto::Request request(std::uint64_t request_id = 1) {
    proto::Request value("main");
    value.id = request_id;
    value.command = {"internal-stream-test"};
    value.context.cwd = "/";
    value.context.tty = true;
    return value;
}

StreamIngressRequest ingress_request(StreamOperation operation = StreamOperation::Listen) {
    return {.client_id = 1001,
            .generation = 7,
            .operation = operation,
            .mode = StreamMode::Items,
            .type_mask = stream_event_mask(StreamEventClass::Message)};
}

StreamTerminalFrame terminal_frame(const StreamTerminalPayload& terminal,
                                   std::uint64_t delivered_count) {
    if (terminal.cause == StreamTerminalCause::PlannedSuccess) {
        return StreamTerminalResultFrame{{{"count", delivered_count}}};
    }
    return StreamTerminalErrorFrame{.code = "TERMINAL",
                                    .message = "stream terminal",
                                    .details = {{"cause", static_cast<int>(terminal.cause)}},
                                    .exit_code = kGeneric};
}

struct Captured {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<json> items;
    std::optional<json> challenge;
    std::optional<json> result;
    std::optional<proto::Error> error;
};

std::shared_ptr<CallbackSink> capturing_sink(Captured& captured) {
    return std::make_shared<CallbackSink>(
        [&captured](json item) {
            const std::lock_guard lock(captured.mutex);
            captured.items.push_back(std::move(item));
        },
        [](const json&) {},
        [&captured](json result) {
            const std::lock_guard lock(captured.mutex);
            captured.result = std::move(result);
        },
        [&captured](std::string code, std::string message, json details, int exit_code) {
            const std::lock_guard lock(captured.mutex);
            captured.error =
                proto::Error{1, std::move(code), std::move(message), std::move(details), exit_code};
            captured.cv.notify_all();
        },
        [&captured](const json& challenge) -> std::optional<json> {
            const std::lock_guard lock(captured.mutex);
            captured.challenge = challenge;
            captured.cv.notify_all();
            return std::nullopt;
        });
}

ChallengeSpec stream_challenge() {
    return {proto::ChallengeKind::AuthenticationCode,
            4,
            9,
            "Code: ",
            {{"delivery_type", "sms"}, {"expected_length", 5}, {"resend_timeout", 30}}};
}

json wait_challenge(Captured& captured) {
    std::unique_lock lock(captured.mutex);
    REQUIRE(captured.cv.wait_for(lock, 2s, [&captured] { return captured.challenge.has_value(); }));
    return *captured.challenge;
}

proto::Answer stream_answer(const json& challenge, std::uint64_t request_id) {
    return {request_id,
            {{"nonce", challenge["nonce"]},
             {"sequence", challenge["sequence"]},
             {"client_generation", challenge["client_generation"]},
             {"auth_sequence", challenge["auth_sequence"]},
             {"value", "12345"}}};
}

struct ManualPoll {
    StreamPollSchedule::Clock::time_point current;
    std::size_t sleeps = 0;
    std::vector<StreamPollSchedule::Clock::time_point> wakes;

    std::shared_ptr<testing::StreamDeliveryHooks> hooks() {
        auto result = std::make_shared<testing::StreamDeliveryHooks>();
        result->now = [this] { return current; };
        result->sleep_until = [this](auto wake) {
            CHECK(wake >= current);
            current = wake;
            wakes.push_back(wake);
            ++sleeps;
        };
        return result;
    }
};

struct SecondPollGate {
    using Clock = StreamPollSchedule::Clock;

    Clock::time_point current;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t sleeps = 0;
    bool second_entered = false;
    bool second_released = false;

    std::shared_ptr<testing::StreamDeliveryHooks> hooks() {
        auto result = std::make_shared<testing::StreamDeliveryHooks>();
        result->now = [this] {
            const std::lock_guard lock(mutex);
            return current;
        };
        result->sleep_until = [this](Clock::time_point wake) {
            std::unique_lock lock(mutex);
            current = wake;
            ++sleeps;
            if (sleeps == 2) {
                second_entered = true;
                cv.notify_all();
                cv.wait(lock, [this] { return second_released; });
            }
        };
        return result;
    }

    void wait_second() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return second_entered; });
    }

    void release_second() {
        const std::lock_guard lock(mutex);
        second_released = true;
        cv.notify_all();
    }
};

struct ActivationGate {
    testing::StreamActivationProbePoint target =
        testing::StreamActivationProbePoint::BeforeLifecycle;
    std::shared_ptr<StreamIngressHub> hub;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    bool claim_shutdown = false;

    explicit ActivationGate(std::shared_ptr<StreamIngressHub> hub_value)
        : hub(std::move(hub_value)) {}

    ActivationGate(testing::StreamActivationProbePoint target_value,
                   std::shared_ptr<StreamIngressHub> hub_value, bool claim_shutdown_value)
        : target(target_value), hub(std::move(hub_value)), claim_shutdown(claim_shutdown_value) {}

    static void notify(void* raw_context, testing::StreamActivationProbePoint point) noexcept {
        auto& gate = *static_cast<ActivationGate*>(raw_context);
        if (point != gate.target) {
            return;
        }
        if (gate.claim_shutdown) {
            gate.hub->claim_control_generation(
                1001, 7, {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
        }
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&gate] { return gate.released; });
    }

    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return entered; });
    }

    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct ProtocolTerminalRaceGate {
    enum class Block { None, ReceiveOwner, ProtocolWaiting, ProtocolOwned };

    Block block = Block::None;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    static void ingress_notify(void* raw_context,
                               tgcli::daemon::detail::StreamIngressProbePoint point,
                               std::size_t index) noexcept {
        auto& gate = *static_cast<ProtocolTerminalRaceGate*>(raw_context);
        if (gate.block != Block::ReceiveOwner ||
            point != tgcli::daemon::detail::StreamIngressProbePoint::OwnerLoad || index != 0) {
            return;
        }
        gate.pause();
    }

    static void subscription_notify(void* raw_context,
                                    testing::StreamSubscriptionProbePoint point) noexcept {
        auto& gate = *static_cast<ProtocolTerminalRaceGate*>(raw_context);
        const bool matches = (gate.block == Block::ProtocolWaiting &&
                              point == testing::StreamSubscriptionProbePoint::ClaimWaiting) ||
                             (gate.block == Block::ProtocolOwned &&
                              point == testing::StreamSubscriptionProbePoint::ClaimOwned);
        if (matches) {
            gate.pause();
        }
    }

    void pause() noexcept {
        std::unique_lock lock(mutex);
        if (entered) {
            return;
        }
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }

    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return entered; });
    }

    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct ProtocolActivationArbitrationGate {
    testing::RequestSessionProbePoint target;
    std::mutex mutex;
    std::condition_variable cv;
    bool activation_entered = false;
    bool activation_released = false;
    bool protocol_entered = false;
    bool protocol_released = false;

    explicit ProtocolActivationArbitrationGate(testing::RequestSessionProbePoint target_value)
        : target(target_value) {}

    static void activation_notify(void* raw_context,
                                  testing::StreamActivationProbePoint point) noexcept {
        if (point != testing::StreamActivationProbePoint::BeforeLifecycle) {
            return;
        }
        auto& gate = *static_cast<ProtocolActivationArbitrationGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        gate.activation_entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&gate] { return gate.activation_released; });
    }

    static void protocol_notify(void* raw_context,
                                testing::RequestSessionProbePoint point) noexcept {
        auto& gate = *static_cast<ProtocolActivationArbitrationGate*>(raw_context);
        if (point != gate.target) {
            return;
        }
        std::unique_lock lock(gate.mutex);
        gate.protocol_entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&gate] { return gate.protocol_released; });
    }

    void wait_activation() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return activation_entered; });
    }

    void wait_protocol() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return protocol_entered; });
    }

    void release_activation() {
        const std::lock_guard lock(mutex);
        activation_released = true;
        cv.notify_all();
    }

    void release_protocol() {
        const std::lock_guard lock(mutex);
        protocol_released = true;
        cv.notify_all();
    }
};

struct PublicFirstTerminalGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool activation_entered = false;
    bool activation_released = false;
    bool public_entered = false;
    bool public_released = false;
    bool protocol_routed = false;

    static void activation_notify(void* raw_context,
                                  testing::StreamActivationProbePoint point) noexcept {
        if (point != testing::StreamActivationProbePoint::BeforeLifecycle) {
            return;
        }
        auto& gate = *static_cast<PublicFirstTerminalGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        gate.activation_entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&gate] { return gate.activation_released; });
    }

    static void protocol_notify(void* raw_context,
                                testing::RequestSessionProbePoint point) noexcept {
        auto& gate = *static_cast<PublicFirstTerminalGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        if (point == testing::RequestSessionProbePoint::BeforePublicTerminalBit) {
            gate.public_entered = true;
            gate.cv.notify_all();
            gate.cv.wait(lock, [&gate] { return gate.public_released; });
        } else if (point == testing::RequestSessionProbePoint::AfterProtocolTerminalRoute) {
            gate.protocol_routed = true;
            gate.cv.notify_all();
        }
    }

    void wait_activation() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return activation_entered; });
    }

    void wait_public() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return public_entered; });
    }

    void wait_protocol() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return protocol_routed; });
    }

    void release_public() {
        const std::lock_guard lock(mutex);
        public_released = true;
        cv.notify_all();
    }

    void release_activation() {
        const std::lock_guard lock(mutex);
        activation_released = true;
        cv.notify_all();
    }
};

struct DeadlineClaimGate {
    enum class Block { BeforeClaim, AfterClaim };
    using Clock = testing::StreamDeliveryHooks::Clock;

    Block block;
    Clock::time_point current;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    DeadlineClaimGate(Block block_value, Clock::time_point current_value)
        : block(block_value), current(current_value) {}

    std::shared_ptr<testing::StreamDeliveryHooks> hooks() {
        auto result = std::make_shared<testing::StreamDeliveryHooks>();
        result->now = [this] {
            const std::lock_guard lock(mutex);
            return current;
        };
        result->sleep_until = [this](Clock::time_point wake) {
            {
                const std::lock_guard lock(mutex);
                current = wake;
            }
            if (block == Block::BeforeClaim) {
                pause();
            }
        };
        result->probe_context = this;
        result->probe = &DeadlineClaimGate::notify;
        return result;
    }

    static void notify(void* raw_context, testing::StreamDeliveryHooks::ProbePoint point) noexcept {
        auto& gate = *static_cast<DeadlineClaimGate*>(raw_context);
        if (gate.block == Block::AfterClaim &&
            point == testing::StreamDeliveryHooks::ProbePoint::AfterScheduledTerminalClaim) {
            gate.pause();
        }
    }

    void pause() noexcept {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }

    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return entered; });
    }

    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct InstallingGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    static void notify(void* raw_context, tgcli::daemon::detail::StreamIngressProbePoint point,
                       std::size_t index) noexcept {
        static_cast<void>(index);
        if (point != tgcli::daemon::detail::StreamIngressProbePoint::ActivationInstalling) {
            return;
        }
        auto& gate = *static_cast<InstallingGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&gate] { return gate.released; });
    }

    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return entered; });
    }

    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct ReclaimProbe {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;

    static void notify(void* raw_context, tgcli::daemon::detail::StreamIngressProbePoint point,
                       std::size_t index) noexcept {
        static_cast<void>(index);
        if (point != tgcli::daemon::detail::StreamIngressProbePoint::ReclaimLoad) {
            return;
        }
        auto& probe = *static_cast<ReclaimProbe*>(raw_context);
        const std::lock_guard lock(probe.mutex);
        probe.entered = true;
        probe.cv.notify_all();
    }

    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return entered; });
    }
};

struct PublishRetireGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool write_entered = false;
    bool write_released = false;
    bool reclaim_entered = false;
    bool discard_entered = false;

    static void notify(void* raw_context, tgcli::daemon::detail::StreamIngressProbePoint point,
                       std::size_t index) noexcept {
        static_cast<void>(index);
        auto& gate = *static_cast<PublishRetireGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        if (point == tgcli::daemon::detail::StreamIngressProbePoint::EnqueueWriteStart) {
            gate.write_entered = true;
            gate.cv.notify_all();
            gate.cv.wait(lock, [&gate] { return gate.write_released; });
        } else if (point == tgcli::daemon::detail::StreamIngressProbePoint::ReclaimLoad) {
            gate.reclaim_entered = true;
            gate.cv.notify_all();
        } else if (point == tgcli::daemon::detail::StreamIngressProbePoint::Discard) {
            gate.discard_entered = true;
            gate.cv.notify_all();
        }
    }

    void wait_write() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return write_entered; });
    }

    void wait_reclaim() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return reclaim_entered; });
    }

    void release_write() {
        const std::lock_guard lock(mutex);
        write_released = true;
        cv.notify_all();
    }

    bool discarded() {
        const std::lock_guard lock(mutex);
        return discard_entered;
    }
};

struct ClaimRetireGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t claim_waiting = 0;
    std::size_t claim_owned = 0;
    bool release_first_claim = false;
    bool reclaim_entered = false;

    static void subscription_notify(void* raw_context,
                                    testing::StreamSubscriptionProbePoint point) noexcept {
        auto& gate = *static_cast<ClaimRetireGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        if (point == testing::StreamSubscriptionProbePoint::ClaimWaiting) {
            ++gate.claim_waiting;
            gate.cv.notify_all();
            return;
        }
        if (point == testing::StreamSubscriptionProbePoint::ClaimOwned) {
            ++gate.claim_owned;
            gate.cv.notify_all();
            if (gate.claim_owned == 1) {
                gate.cv.wait(lock, [&gate] { return gate.release_first_claim; });
            }
        }
    }

    static void ingress_notify(void* raw_context,
                               tgcli::daemon::detail::StreamIngressProbePoint point,
                               std::size_t index) noexcept {
        static_cast<void>(index);
        if (point != tgcli::daemon::detail::StreamIngressProbePoint::ReclaimLoad) {
            return;
        }
        auto& gate = *static_cast<ClaimRetireGate*>(raw_context);
        const std::lock_guard lock(gate.mutex);
        gate.reclaim_entered = true;
        gate.cv.notify_all();
    }

    void wait_first_owned() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return claim_owned == 1; });
    }

    void wait_second_waiting() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return claim_waiting >= 2; });
    }

    void release() {
        const std::lock_guard lock(mutex);
        release_first_claim = true;
        cv.notify_all();
    }

    [[nodiscard]] std::pair<std::size_t, bool> snapshot() {
        const std::lock_guard lock(mutex);
        return {claim_owned, reclaim_entered};
    }
};

struct RetireClaimGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t claim_waiting = 0;
    std::size_t claim_forwarding = 0;
    bool retire_owned = false;
    bool released = false;
    bool reclaim_entered = false;

    static void subscription_notify(void* raw_context,
                                    testing::StreamSubscriptionProbePoint point) noexcept {
        auto& gate = *static_cast<RetireClaimGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        if (point == testing::StreamSubscriptionProbePoint::ClaimWaiting) {
            ++gate.claim_waiting;
            gate.cv.notify_all();
            return;
        }
        if (point == testing::StreamSubscriptionProbePoint::ClaimForwarding) {
            ++gate.claim_forwarding;
            gate.cv.notify_all();
            return;
        }
        if (point == testing::StreamSubscriptionProbePoint::RetireOwned) {
            gate.retire_owned = true;
            gate.cv.notify_all();
            gate.cv.wait(lock, [&gate] { return gate.released; });
        }
    }

    static void ingress_notify(void* raw_context,
                               tgcli::daemon::detail::StreamIngressProbePoint point,
                               std::size_t index) noexcept {
        static_cast<void>(index);
        if (point != tgcli::daemon::detail::StreamIngressProbePoint::ReclaimLoad) {
            return;
        }
        auto& gate = *static_cast<RetireClaimGate*>(raw_context);
        const std::lock_guard lock(gate.mutex);
        gate.reclaim_entered = true;
        gate.cv.notify_all();
    }

    void wait_retire_owned() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return retire_owned; });
    }

    void wait_claim_waiting() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return claim_waiting != 0; });
    }

    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }

    [[nodiscard]] std::pair<std::size_t, bool> snapshot() {
        const std::lock_guard lock(mutex);
        return {claim_forwarding, reclaim_entered};
    }
};

struct OverflowGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    static void notify(void* raw_context, tgcli::daemon::detail::StreamIngressProbePoint point,
                       std::size_t index) noexcept {
        static_cast<void>(index);
        if (point != tgcli::daemon::detail::StreamIngressProbePoint::EnqueueOverflow) {
            return;
        }
        auto& gate = *static_cast<OverflowGate*>(raw_context);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&gate] { return gate.released; });
    }

    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return entered; });
    }

    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

StreamItemView message_item(std::string_view line, std::uint64_t sequence = 2) {
    return StreamIngressTestAccess::item(line, {}, sequence,
                                         {.event_class = StreamEventClass::Message,
                                          .chat_id = 42,
                                          .sender_kind = StreamSenderKind::User,
                                          .sender_id = 7});
}

std::string maximum_item_line() {
    std::string line(kStreamQueueItemBytes, 'x');
    line.back() = '\n';
    return line;
}

void fill_queue_near_byte_capacity(StreamIngressHub& hub, std::uint64_t first_sequence) {
    const auto line = maximum_item_line();
    for (std::uint64_t offset = 0; offset < 31; ++offset) {
        hub.publish(message_item(line, first_sequence + offset));
    }
}

void activate(RequestSession& session, const std::shared_ptr<StreamIngressHub>& hub,
              StreamActivityMode mode = StreamActivityMode::UntrackedNoDaemon) {
    auto result = session.activate_stream_subscription(hub, ingress_request(), mode);
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(result));
}

class BlockingSink final : public ResponseSink {
  public:
    explicit BlockingSink(DeliveryOutcome item_outcome = DeliveryOutcome::Complete,
                          DeliveryOutcome terminal_outcome = DeliveryOutcome::Complete,
                          bool throw_item = false, bool throw_terminal = false)
        : item_outcome_(item_outcome), terminal_outcome_(terminal_outcome), throw_item_(throw_item),
          throw_terminal_(throw_terminal) {}

    void wait_item() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return item_entered_; });
    }

    void release_item() {
        const std::lock_guard lock(mutex_);
        item_released_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t terminals() const {
        return terminals_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t items() const {
        return items_.load(std::memory_order_acquire);
    }

    json wait_challenge() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return challenge_.has_value(); });
        return *challenge_;
    }

    [[nodiscard]] std::optional<proto::Error> terminal_error() const {
        const std::lock_guard lock(mutex_);
        return error_;
    }

  private:
    DeliveryOutcome emit_item(json data) override {
        static_cast<void>(data);
        items_.fetch_add(1, std::memory_order_release);
        {
            std::unique_lock lock(mutex_);
            item_entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return item_released_; });
        }
        if (throw_item_) {
            throw std::runtime_error("item delivery failed");
        }
        return item_outcome_;
    }

    void emit_progress(json data) override {
        static_cast<void>(data);
    }

    DeliveryOutcome emit_result(json data) override {
        static_cast<void>(data);
        terminals_.fetch_add(1, std::memory_order_release);
        if (throw_terminal_) {
            throw std::runtime_error("terminal delivery failed");
        }
        return terminal_outcome_;
    }

    DeliveryOutcome emit_error(std::string code, std::string message, json details,
                               int exit_code) override {
        {
            const std::lock_guard lock(mutex_);
            error_ =
                proto::Error{1, std::move(code), std::move(message), std::move(details), exit_code};
        }
        terminals_.fetch_add(1, std::memory_order_release);
        if (throw_terminal_) {
            throw std::runtime_error("terminal delivery failed");
        }
        return terminal_outcome_;
    }

    ChallengeReply emit_challenge(json data) override {
        const std::lock_guard lock(mutex_);
        challenge_ = std::move(data);
        cv_.notify_all();
        return {};
    }

    DeliveryOutcome item_outcome_;
    DeliveryOutcome terminal_outcome_;
    bool throw_item_ = false;
    bool throw_terminal_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool item_entered_ = false;
    bool item_released_ = false;
    std::optional<json> challenge_;
    std::optional<proto::Error> error_;
    std::atomic<std::size_t> terminals_ = 0;
    std::atomic<std::size_t> items_ = 0;
};

} // namespace

TEST_CASE("stream delivery counts only complete items and claims planned success",
          "[stream][subscription][delivery]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, RequestDeadline{});
    activate(session, hub);
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    constexpr std::string_view line = R"({"event":"message","chat_id":42})"
                                      "\n";
    hub->publish(message_item(line));
    ManualPoll poll;

    CHECK(run_stream_delivery(
              session, {.count = 1, .terminal_builder = &terminal_frame, .hooks = poll.hooks()}) ==
          StreamDeliveryStatus::TerminalComplete);
    REQUIRE(captured.items.size() == 1);
    CHECK(captured.items.front()["chat_id"] == 42);
    REQUIRE(captured.result);
    CHECK((*captured.result)["count"] == 1);
    CHECK_FALSE(captured.error);
    CHECK(poll.sleeps >= 2);
    REQUIRE_FALSE(poll.wakes.empty());
    CHECK(poll.wakes.front() == StreamPollSchedule::Clock::time_point{} + 2ms);
}

TEST_CASE("stream terminal before an item discards the unsent queue",
          "[stream][subscription][terminal]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, RequestDeadline{});
    activate(session, hub);
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    hub->publish(message_item("{}\n"));
    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
    ManualPoll poll;

    CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                        .terminal_builder = &terminal_frame,
                                        .hooks = poll.hooks()}) ==
          StreamDeliveryStatus::TerminalComplete);
    CHECK(captured.items.empty());
    REQUIRE(captured.error);
    CHECK(captured.error->code == "TERMINAL");
}

TEST_CASE("terminal send failure disconnects once without retry",
          "[stream][subscription][terminal][disconnect]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    auto sink =
        std::make_shared<BlockingSink>(DeliveryOutcome::Complete, DeliveryOutcome::Disconnected);
    sink->release_item();
    RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
    activate(session, hub);
    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
    ManualPoll poll;

    CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                        .terminal_builder = &terminal_frame,
                                        .hooks = poll.hooks()}) ==
          StreamDeliveryStatus::Disconnected);
    CHECK(sink->terminals() == 1);
    CHECK(session.cancellation_requested());
}

TEST_CASE("deadline equality wins at the first scheduled poll",
          "[stream][subscription][deadline][schedule]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    const auto deadline_at = StreamPollSchedule::Clock::now() + 1h;
    const RequestDeadline deadline{deadline_at};
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, deadline);
    activate(session, hub);
    ManualPoll poll;
    poll.current = deadline_at - 2ms;

    CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                        .terminal_builder = &terminal_frame,
                                        .hooks = poll.hooks()}) ==
          StreamDeliveryStatus::TerminalComplete);
    CHECK(poll.sleeps == 1);
    REQUIRE(captured.error);
}

TEST_CASE("terminal during a begun complete item wins before planned count",
          "[stream][subscription][delivery][concurrency]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    auto sink = std::make_shared<BlockingSink>();
    RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
    activate(session, hub);
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    hub->publish(message_item("{}\n"));
    ManualPoll poll;
    std::atomic<std::uint64_t> count_at_terminal = 0;
    StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
    std::thread worker([&] {
        status = run_stream_delivery(
            session, {.count = 1,
                      .terminal_builder =
                          [&count_at_terminal](const StreamTerminalPayload& terminal,
                                               std::uint64_t delivered_count) {
                              count_at_terminal.store(delivered_count, std::memory_order_release);
                              return terminal_frame(terminal, delivered_count);
                          },
                      .hooks = poll.hooks()});
    });
    sink->wait_item();
    hub->claim_generation(1001, 7,
                          {.cause = StreamTerminalCause::AuthorizationLost,
                           .auth_state = 12,
                           .metadata_failure = {}});
    sink->release_item();
    worker.join();

    CHECK(status == StreamDeliveryStatus::TerminalComplete);
    CHECK(sink->terminals() == 1);
    CHECK(count_at_terminal.load(std::memory_order_acquire) == 1);
}

TEST_CASE("Nth completion and external causes retain the forced concurrent winner",
          "[stream][subscription][delivery][terminal][concurrency]") {
    const std::array external{
        StreamTerminalPayload{.cause = StreamTerminalCause::Deadline, .metadata_failure = {}},
        StreamTerminalPayload{.cause = StreamTerminalCause::AuthorizationLost,
                              .auth_state = 12,
                              .metadata_failure = {}},
        StreamTerminalPayload{.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}},
    };
    for (const auto& cause : external) {
        for (const bool external_first : {true, false}) {
            DYNAMIC_SECTION("cause=" << static_cast<int>(cause.cause)
                                     << " external_first=" << external_first) {
                auto hub = std::make_shared<StreamIngressHub>();
                hub->begin_generation(1001, 7);
                auto sink = std::make_shared<BlockingSink>();
                RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
                activate(session, hub);
                REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
                hub->publish(message_item("{}\n", 2));
                hub->publish(message_item("{}\n", 3));
                SecondPollGate poll;
                std::optional<StreamTerminalPayload> observed;
                std::uint64_t delivered = 0;
                StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
                std::thread worker([&] {
                    status = run_stream_delivery(
                        session, {.count = 1,
                                  .terminal_builder =
                                      [&observed, &delivered](const StreamTerminalPayload& terminal,
                                                              std::uint64_t delivered_count) {
                                          observed = terminal;
                                          delivered = delivered_count;
                                          return terminal_frame(terminal, delivered_count);
                                      },
                                  .hooks = poll.hooks()});
                });
                sink->wait_item();
                if (external_first) {
                    hub->claim_generation(1001, 7, cause);
                    sink->release_item();
                    poll.wait_second();
                } else {
                    sink->release_item();
                    poll.wait_second();
                    hub->claim_generation(1001, 7, cause);
                }
                poll.release_second();
                worker.join();

                REQUIRE(observed);
                CHECK(observed->cause ==
                      (external_first ? cause.cause : StreamTerminalCause::PlannedSuccess));
                if (external_first) {
                    CHECK(observed->auth_state == cause.auth_state);
                }
                CHECK(delivered == 1);
                CHECK(sink->items() == 1);
                CHECK(sink->terminals() == 1);
                CHECK(status == StreamDeliveryStatus::TerminalComplete);
                auto replacement = hub->reserve(ingress_request());
                CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
            }
        }
    }
}

TEST_CASE("Nth completion races the actual queue byte overflow in both orders",
          "[stream][subscription][delivery][terminal][overflow][concurrency]") {
    for (const bool overflow_first : {true, false}) {
        DYNAMIC_SECTION("overflow_first=" << overflow_first) {
            OverflowGate overflow;
            auto hub = std::make_shared<StreamIngressHub>(
                tgcli::daemon::detail::StreamIngressProbe{.context = &overflow,
                                                          .hook = &OverflowGate::notify,
                                                          .forced_lock_free_failure = {}});
            hub->begin_generation(1001, 7);
            auto sink = std::make_shared<BlockingSink>();
            RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
            activate(session, hub);
            REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
            hub->publish(message_item("{}\n", 2));
            hub->publish(message_item("{}\n", 3));
            fill_queue_near_byte_capacity(*hub, 4);
            SecondPollGate poll;
            std::optional<StreamTerminalPayload> observed;
            std::uint64_t delivered = 0;
            StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
            std::thread worker([&] {
                status = run_stream_delivery(
                    session, {.count = 1,
                              .terminal_builder =
                                  [&observed, &delivered](const StreamTerminalPayload& terminal,
                                                          std::uint64_t delivered_count) {
                                      observed = terminal;
                                      delivered = delivered_count;
                                      return terminal_frame(terminal, delivered_count);
                                  },
                              .hooks = poll.hooks()});
            });
            sink->wait_item();
            const auto overflow_line = maximum_item_line();
            std::thread publisher([&] { hub->publish(message_item(overflow_line, 100)); });
            overflow.wait();
            if (overflow_first) {
                overflow.release();
                publisher.join();
                sink->release_item();
                poll.wait_second();
            } else {
                sink->release_item();
                poll.wait_second();
                overflow.release();
                publisher.join();
            }
            poll.release_second();
            worker.join();

            REQUIRE(observed);
            CHECK(observed->cause == (overflow_first ? StreamTerminalCause::QueueBytes
                                                     : StreamTerminalCause::PlannedSuccess));
            if (overflow_first) {
                CHECK(observed->incoming_bytes == kStreamQueueItemBytes);
                CHECK(observed->queued_bytes <= kStreamQueueBytes);
                CHECK(observed->queued_bytes + observed->incoming_bytes > kStreamQueueBytes);
            }
            CHECK(delivered == 1);
            CHECK(sink->items() == 1);
            CHECK(sink->terminals() == 1);
            CHECK(status == StreamDeliveryStatus::TerminalComplete);
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("incomplete suppressed and exceptional items disconnect without terminal or count",
          "[stream][subscription][delivery][disconnect]") {
    struct FailureMode {
        const char* name;
        DeliveryOutcome outcome;
        bool throws;
    };
    constexpr std::array modes{
        FailureMode{"incomplete transport", DeliveryOutcome::Disconnected, false},
        FailureMode{"suppressed transport", DeliveryOutcome::Suppressed, false},
        FailureMode{"exception", DeliveryOutcome::Complete, true},
    };
    for (const auto& mode : modes) {
        DYNAMIC_SECTION("mode=" << mode.name) {
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            auto sink = std::make_shared<BlockingSink>(mode.outcome, DeliveryOutcome::Complete,
                                                       mode.throws);
            RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
            activate(session, hub);
            REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
            hub->publish(message_item("{}\n"));
            ManualPoll poll;
            StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
            std::thread worker([&] {
                status = run_stream_delivery(
                    session,
                    {.count = 1, .terminal_builder = &terminal_frame, .hooks = poll.hooks()});
            });
            sink->wait_item();
            sink->release_item();
            worker.join();
            CHECK(status == (mode.outcome == DeliveryOutcome::Suppressed
                                 ? StreamDeliveryStatus::Suppressed
                                 : StreamDeliveryStatus::Disconnected));
            CHECK(sink->terminals() == 0);
            CHECK(session.cancellation_requested());
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("terminal builder and transport exceptions disconnect after one attempt",
          "[stream][subscription][terminal][disconnect][exception]") {
    for (const bool builder_throws : {false, true}) {
        DYNAMIC_SECTION("source=" << (builder_throws ? "builder" : "transport")) {
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            auto sink = std::make_shared<BlockingSink>(
                DeliveryOutcome::Complete, DeliveryOutcome::Complete, false, !builder_throws);
            sink->release_item();
            RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
            activate(session, hub);
            hub->claim_control_generation(
                1001, 7, {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
            ManualPoll poll;
            std::size_t builder_calls = 0;

            const auto status = run_stream_delivery(
                session,
                {.count = std::nullopt,
                 .terminal_builder =
                     [&builder_calls, builder_throws](const StreamTerminalPayload& terminal,
                                                      std::uint64_t delivered_count) {
                         ++builder_calls;
                         if (builder_throws) {
                             throw std::runtime_error("terminal builder failed");
                         }
                         return terminal_frame(terminal, delivered_count);
                     },
                 .hooks = poll.hooks()});
            CHECK(status == StreamDeliveryStatus::Disconnected);
            CHECK(builder_calls == 1);
            CHECK(sink->terminals() == static_cast<std::size_t>(!builder_throws));
            CHECK(session.cancellation_requested());
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("stream activation promotes tracked activity and requires an explicit mode",
          "[stream][subscription][activity]") {
    ActivityTracker tracker([] {});
    REQUIRE(tracker.daemon_ready(std::nullopt));
    auto activity = tracker.try_request();
    REQUIRE(activity);
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity), {},
                           RequestDeadline{});
    auto result = session.activate_stream_subscription(hub, ingress_request(),
                                                       StreamActivityMode::TrackedDaemon);
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(result));
    auto state = tracker.snapshot();
    CHECK(state.requests == 0);
    CHECK(state.subscriptions == 1);
    CHECK_FALSE(state.zero_since);
    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
    ManualPoll poll;
    CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                        .terminal_builder = &terminal_frame,
                                        .hooks = poll.hooks()}) ==
          StreamDeliveryStatus::TerminalComplete);
    state = tracker.snapshot();
    CHECK(state.requests == 0);
    CHECK(state.subscriptions == 0);

    auto untracked_hub = std::make_shared<StreamIngressHub>();
    untracked_hub->begin_generation(1001, 7);
    RequestSession untracked(request(), capturing_sink(captured), 18, {}, {}, {},
                             RequestDeadline{});
    auto invalid = untracked.activate_stream_subscription(untracked_hub, ingress_request(),
                                                          StreamActivityMode::TrackedDaemon);
    CHECK(std::get<StreamSubscriptionActivationFailure>(invalid) ==
          StreamSubscriptionActivationFailure::ActivityUnavailable);

    ActivityTracker second_tracker([] {});
    REQUIRE(second_tracker.daemon_ready(std::nullopt));
    auto second_activity = second_tracker.try_request();
    REQUIRE(second_activity);
    auto tagged_hub = std::make_shared<StreamIngressHub>();
    tagged_hub->begin_generation(1001, 7);
    RequestSession wrongly_untracked(request(), capturing_sink(captured), 19, {},
                                     std::move(*second_activity), {}, RequestDeadline{});
    auto wrong_tag = wrongly_untracked.activate_stream_subscription(
        tagged_hub, ingress_request(), StreamActivityMode::UntrackedNoDaemon);
    CHECK(std::get<StreamSubscriptionActivationFailure>(wrong_tag) ==
          StreamSubscriptionActivationFailure::ActivityUnavailable);
    CHECK(second_tracker.snapshot().requests == 1);
    static_cast<void>(wrongly_untracked.result(json::object()));
}

TEST_CASE("direct active terminals are suppressed until runner authorization",
          "[stream][subscription][activity][terminal][authorization]") {
    ActivityTracker tracker([] {});
    REQUIRE(tracker.daemon_ready(std::nullopt));
    auto activity = tracker.try_request();
    REQUIRE(activity);
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity), {},
                           RequestDeadline{});
    REQUIRE_FALSE(session.deadline().expires_at);
    auto activation = session.activate_stream_subscription(hub, ingress_request(),
                                                           StreamActivityMode::TrackedDaemon);
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
    CHECK(tracker.snapshot().requests == 0);
    CHECK(tracker.snapshot().subscriptions == 1);

    CHECK(session.result({{"direct", true}}) == DeliveryOutcome::Suppressed);
    CHECK(session.error("DIRECT", "direct", json::object(), kGeneric) ==
          DeliveryOutcome::Suppressed);
    CHECK_FALSE(session.has_terminal());
    CHECK_FALSE(captured.result);
    CHECK_FALSE(captured.error);
    CHECK(tracker.snapshot().subscriptions == 1);

    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
    ManualPoll poll;
    CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                        .terminal_builder = &terminal_frame,
                                        .hooks = poll.hooks()}) ==
          StreamDeliveryStatus::TerminalComplete);
    REQUIRE(captured.error);
    CHECK(captured.error->code == "TERMINAL");
    CHECK(session.result({{"late", true}}) == DeliveryOutcome::Suppressed);
    CHECK(tracker.snapshot().requests == 0);
    CHECK(tracker.snapshot().subscriptions == 0);
    auto replacement = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
}

TEST_CASE("protocol classification and stream promotion share one lifecycle decision",
          "[stream][subscription][challenge][protocol-answer][activation][concurrency]") {
    for (const bool protocol_first : {true, false}) {
        DYNAMIC_SECTION("protocol_first=" << protocol_first) {
            ProtocolActivationArbitrationGate gate{
                protocol_first ? testing::RequestSessionProbePoint::AfterProtocolTerminalRoute
                               : testing::RequestSessionProbePoint::BeforeProtocolTerminalRoute};
            ActivityTracker tracker([] {});
            REQUIRE(tracker.daemon_ready(std::nullopt));
            auto activity = tracker.try_request();
            REQUIRE(activity);
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            std::vector<StreamIngressReservation> occupied;
            occupied.reserve(kStreamSubscriberSlots - 1);
            for (std::size_t index = 1; index < kStreamSubscriberSlots; ++index) {
                auto admission = hub->reserve(ingress_request());
                REQUIRE(std::holds_alternative<StreamIngressReservation>(admission));
                occupied.push_back(std::move(std::get<StreamIngressReservation>(admission)));
            }
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {},
                                   std::move(*activity), {}, RequestDeadline{});
            testing::RequestSessionTestAccess::install_probe(
                session, &gate, &ProtocolActivationArbitrationGate::protocol_notify);
            auto challenge = std::async(
                std::launch::async, [&session] { return session.challenge(stream_challenge()); });
            auto invalid = stream_answer(wait_challenge(captured), 1);
            invalid.answer.erase("nonce");

            StreamSubscriptionActivationResult activation_result =
                StreamSubscriptionActivationFailure::PublicationFailed;
            std::thread activation([&] {
                activation_result = session.activate_stream_subscription(
                    hub, ingress_request(), StreamActivityMode::TrackedDaemon,
                    {.context = &gate,
                     .hook = &ProtocolActivationArbitrationGate::activation_notify,
                     .subscription_hook = nullptr});
            });
            gate.wait_activation();

            AnswerDisposition disposition = AnswerDisposition::RequestTerminated;
            std::thread protocol([&] { disposition = session.receive_answer(std::move(invalid)); });
            gate.wait_protocol();
            gate.release_activation();
            activation.join();
            gate.release_protocol();
            protocol.join();
            const auto challenge_status = challenge.get().status();

            CHECK(disposition == AnswerDisposition::Rejected);
            CHECK(challenge_status == ChallengeStatus::ProtocolError);
            if (protocol_first) {
                CHECK_FALSE(std::holds_alternative<StreamSubscriptionActivated>(activation_result));
                REQUIRE(captured.error);
                CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
                CHECK(captured.error->message == "invalid challenge answer");
                CHECK(captured.error->details == json{{"request_id", 1}, {"reason", "malformed"}});
                CHECK(captured.error->exit_code == kUsage);
                CHECK(session.has_terminal());
                CHECK(tracker.snapshot().requests == 0);
                CHECK(tracker.snapshot().subscriptions == 0);
            } else {
                REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation_result));
                CHECK_FALSE(captured.error);
                CHECK(tracker.snapshot().requests == 0);
                CHECK(tracker.snapshot().subscriptions == 1);
                ManualPoll poll;
                std::size_t builder_calls = 0;
                const auto delivery = run_stream_delivery(
                    session, {.count = std::nullopt,
                              .terminal_builder =
                                  [&builder_calls](const StreamTerminalPayload& terminal,
                                                   std::uint64_t delivered) {
                                      static_cast<void>(terminal);
                                      static_cast<void>(delivered);
                                      ++builder_calls;
                                      return StreamTerminalResultFrame{{{"unexpected", true}}};
                                  },
                              .hooks = poll.hooks()});
                CHECK(delivery == StreamDeliveryStatus::TerminalComplete);
                CHECK(builder_calls == 0);
                REQUIRE(captured.error);
                CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
                CHECK(captured.error->details == json{{"request_id", 1}, {"reason", "malformed"}});
                CHECK(tracker.snapshot().requests == 0);
                CHECK(tracker.snapshot().subscriptions == 0);
            }
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("protocol fallback excludes concurrent public terminals",
          "[stream][subscription][challenge][protocol-answer][activation][terminal][concurrency]") {
    for (const bool public_result : {true, false}) {
        DYNAMIC_SECTION("public_result=" << public_result) {
            ProtocolActivationArbitrationGate gate{
                testing::RequestSessionProbePoint::AfterProtocolTerminalRoute};
            ActivityTracker tracker([] {});
            REQUIRE(tracker.daemon_ready(std::nullopt));
            auto activity = tracker.try_request();
            REQUIRE(activity);
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            std::vector<StreamIngressReservation> occupied;
            occupied.reserve(kStreamSubscriberSlots - 1);
            for (std::size_t index = 1; index < kStreamSubscriberSlots; ++index) {
                auto admission = hub->reserve(ingress_request());
                REQUIRE(std::holds_alternative<StreamIngressReservation>(admission));
                occupied.push_back(std::move(std::get<StreamIngressReservation>(admission)));
            }
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {},
                                   std::move(*activity), {}, RequestDeadline{});
            testing::RequestSessionTestAccess::install_probe(
                session, &gate, &ProtocolActivationArbitrationGate::protocol_notify);
            auto challenge = std::async(
                std::launch::async, [&session] { return session.challenge(stream_challenge()); });
            auto invalid = stream_answer(wait_challenge(captured), 1);
            invalid.answer.erase("nonce");

            StreamSubscriptionActivationResult activation_result =
                StreamSubscriptionActivationFailure::PublicationFailed;
            std::thread activation([&] {
                activation_result = session.activate_stream_subscription(
                    hub, ingress_request(), StreamActivityMode::TrackedDaemon,
                    {.context = &gate,
                     .hook = &ProtocolActivationArbitrationGate::activation_notify,
                     .subscription_hook = nullptr});
            });
            gate.wait_activation();
            AnswerDisposition disposition = AnswerDisposition::RequestTerminated;
            std::thread protocol([&] { disposition = session.receive_answer(std::move(invalid)); });
            gate.wait_protocol();

            const auto public_outcome =
                public_result
                    ? session.result({{"source", "public"}})
                    : session.error("PUBLIC", "public terminal", {{"source", "public"}}, kGeneric);
            const bool public_set_terminal = session.has_terminal();
            bool public_emitted_result = false;
            bool public_emitted_error = false;
            {
                const std::lock_guard lock(captured.mutex);
                public_emitted_result = captured.result.has_value();
                public_emitted_error = captured.error.has_value();
            }

            gate.release_activation();
            activation.join();
            gate.release_protocol();
            protocol.join();
            const auto challenge_status = challenge.get().status();

            CHECK(public_outcome == DeliveryOutcome::Suppressed);
            CHECK_FALSE(public_set_terminal);
            CHECK_FALSE(public_emitted_result);
            CHECK_FALSE(public_emitted_error);
            CHECK_FALSE(std::holds_alternative<StreamSubscriptionActivated>(activation_result));
            CHECK(disposition == AnswerDisposition::Rejected);
            CHECK(challenge_status == ChallengeStatus::ProtocolError);
            REQUIRE(captured.error);
            CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
            CHECK(captured.error->message == "invalid challenge answer");
            CHECK(captured.error->details == json{{"request_id", 1}, {"reason", "malformed"}});
            CHECK(captured.error->exit_code == kUsage);
            CHECK_FALSE(captured.result);
            CHECK(tracker.snapshot().requests == 0);
            CHECK(tracker.snapshot().subscriptions == 0);
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("public terminal claim precedes protocol routing without a split decision",
          "[stream][subscription][challenge][protocol-answer][activation][terminal][concurrency]") {
    for (const bool public_result : {true, false}) {
        DYNAMIC_SECTION("public_result=" << public_result) {
            PublicFirstTerminalGate gate;
            ActivityTracker tracker([] {});
            REQUIRE(tracker.daemon_ready(std::nullopt));
            auto activity = tracker.try_request();
            REQUIRE(activity);
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            std::vector<StreamIngressReservation> occupied;
            occupied.reserve(kStreamSubscriberSlots - 1);
            for (std::size_t index = 1; index < kStreamSubscriberSlots; ++index) {
                auto admission = hub->reserve(ingress_request());
                REQUIRE(std::holds_alternative<StreamIngressReservation>(admission));
                occupied.push_back(std::move(std::get<StreamIngressReservation>(admission)));
            }
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {},
                                   std::move(*activity), {}, RequestDeadline{});
            testing::RequestSessionTestAccess::install_probe(
                session, &gate, &PublicFirstTerminalGate::protocol_notify);
            auto challenge = std::async(
                std::launch::async, [&session] { return session.challenge(stream_challenge()); });
            auto invalid = stream_answer(wait_challenge(captured), 1);
            invalid.answer.erase("nonce");

            StreamSubscriptionActivationResult activation_result =
                StreamSubscriptionActivationFailure::PublicationFailed;
            std::thread activation([&] {
                activation_result = session.activate_stream_subscription(
                    hub, ingress_request(), StreamActivityMode::TrackedDaemon,
                    {.context = &gate,
                     .hook = &PublicFirstTerminalGate::activation_notify,
                     .subscription_hook = nullptr});
            });
            gate.wait_activation();

            DeliveryOutcome public_outcome = DeliveryOutcome::Suppressed;
            std::thread public_terminal([&] {
                public_outcome = public_result ? session.result({{"source", "public"}})
                                               : session.error("PUBLIC", "public terminal",
                                                               {{"source", "public"}}, kGeneric);
            });
            gate.wait_public();
            AnswerDisposition disposition = AnswerDisposition::RequestTerminated;
            std::thread protocol([&] { disposition = session.receive_answer(std::move(invalid)); });
            gate.wait_protocol();
            gate.release_public();
            public_terminal.join();
            protocol.join();
            gate.release_activation();
            activation.join();
            const auto challenge_status = challenge.get().status();

            CHECK(public_outcome == DeliveryOutcome::Complete);
            CHECK(session.has_terminal());
            CHECK_FALSE(std::holds_alternative<StreamSubscriptionActivated>(activation_result));
            CHECK(disposition == AnswerDisposition::Rejected);
            CHECK(challenge_status == ChallengeStatus::ProtocolError);
            if (public_result) {
                REQUIRE(captured.result);
                CHECK(*captured.result == json{{"source", "public"}});
                CHECK_FALSE(captured.error);
            } else {
                CHECK_FALSE(captured.result);
                REQUIRE(captured.error);
                CHECK(captured.error->code == "PUBLIC");
                CHECK(captured.error->message == "public terminal");
                CHECK(captured.error->details == json{{"source", "public"}});
                CHECK(captured.error->exit_code == kGeneric);
            }
            CHECK(tracker.snapshot().requests == 0);
            CHECK(tracker.snapshot().subscriptions == 0);
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("active malformed and future answers retain exact fixed protocol terminals",
          "[stream][subscription][challenge][protocol-answer]") {
    struct Case {
        std::uint64_t request_id;
        const char* reason;
        void (*mutate)(proto::Answer&);
    };
    const std::array cases{
        Case{0, "malformed", [](proto::Answer& answer) { answer.answer.erase("nonce"); }},
        Case{std::numeric_limits<std::uint64_t>::max(), "future_sequence",
             [](proto::Answer& answer) {
                 ++answer.answer["sequence"].get_ref<json::number_unsigned_t&>();
             }},
    };
    for (const auto& test : cases) {
        DYNAMIC_SECTION(test.reason << " request_id=" << test.request_id) {
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            RequestSession session(request(test.request_id), capturing_sink(captured), 17, {}, {},
                                   {}, RequestDeadline{});
            activate(session, hub);
            auto challenge = std::async(
                std::launch::async, [&session] { return session.challenge(stream_challenge()); });
            auto invalid = stream_answer(wait_challenge(captured), test.request_id);
            test.mutate(invalid);
            const auto disposition = session.receive_answer(std::move(invalid));
            CHECK(disposition == AnswerDisposition::Rejected);
            CHECK(challenge.get().status() == ChallengeStatus::ProtocolError);
            CHECK_FALSE(captured.error);
            CHECK_FALSE(captured.result);

            ManualPoll poll;
            std::size_t builder_calls = 0;
            const auto status = run_stream_delivery(
                session, {.count = std::nullopt,
                          .terminal_builder =
                              [&builder_calls](const StreamTerminalPayload& terminal,
                                               std::uint64_t delivered) {
                                  static_cast<void>(terminal);
                                  static_cast<void>(delivered);
                                  ++builder_calls;
                                  return StreamTerminalResultFrame{{{"unexpected", true}}};
                              },
                          .hooks = poll.hooks()});
            CHECK(status == StreamDeliveryStatus::TerminalComplete);
            CHECK(builder_calls == 0);
            REQUIRE(captured.error);
            CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
            CHECK(captured.error->message == "invalid challenge answer");
            CHECK(captured.error->details ==
                  json{{"request_id", test.request_id}, {"reason", test.reason}});
            CHECK(captured.error->exit_code == kUsage);
            CHECK_FALSE(captured.result);
            CHECK(session.error("LATE", "late", json::object(), kGeneric) ==
                  DeliveryOutcome::Suppressed);
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("an incumbent auth or deadline terminal suppresses the protocol answer error",
          "[stream][subscription][challenge][protocol-answer][terminal]") {
    for (const auto cause :
         {StreamTerminalCause::AuthorizationLost, StreamTerminalCause::Deadline}) {
        DYNAMIC_SECTION("cause=" << static_cast<int>(cause)) {
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {},
                                   RequestDeadline{});
            activate(session, hub);
            auto challenge = std::async(
                std::launch::async, [&session] { return session.challenge(stream_challenge()); });
            auto invalid = stream_answer(wait_challenge(captured), 1);
            invalid.answer.erase("nonce");
            if (cause == StreamTerminalCause::AuthorizationLost) {
                hub->claim_generation(1001, 7,
                                      {.cause = cause, .auth_state = 12, .metadata_failure = {}});
            } else {
                hub->claim_control_generation(1001, 7, {.cause = cause, .metadata_failure = {}});
            }
            const auto disposition = session.receive_answer(std::move(invalid));
            CHECK(disposition == AnswerDisposition::Rejected);
            CHECK(challenge.get().status() == ChallengeStatus::ProtocolError);
            CHECK_FALSE(captured.error);

            ManualPoll poll;
            std::optional<StreamTerminalPayload> observed;
            CHECK(run_stream_delivery(
                      session, {.count = std::nullopt,
                                .terminal_builder =
                                    [&observed](const StreamTerminalPayload& terminal,
                                                std::uint64_t delivered) {
                                        static_cast<void>(delivered);
                                        observed = terminal;
                                        return terminal_frame(terminal, 0);
                                    },
                                .hooks = poll.hooks()}) == StreamDeliveryStatus::TerminalComplete);
            REQUIRE(observed);
            CHECK(observed->cause == cause);
            REQUIRE(captured.error);
            CHECK(captured.error->code == "TERMINAL");
            CHECK(captured.error->details["cause"] == static_cast<int>(cause));
        }
    }
}

TEST_CASE("protocol and authorization loss preserve both forced first-cause orders",
          "[stream][subscription][challenge][protocol-answer][auth][concurrency]") {
    for (const bool protocol_wins : {true, false}) {
        DYNAMIC_SECTION("protocol_wins=" << protocol_wins) {
            ProtocolTerminalRaceGate gate;
            auto hub = std::make_shared<StreamIngressHub>(tgcli::daemon::detail::StreamIngressProbe{
                .context = &gate,
                .hook = &ProtocolTerminalRaceGate::ingress_notify,
                .forced_lock_free_failure = {}});
            hub->begin_generation(1001, 7);
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {},
                                   RequestDeadline{});
            auto activation = session.activate_stream_subscription(
                hub, ingress_request(), StreamActivityMode::UntrackedNoDaemon,
                {.context = &gate,
                 .hook = nullptr,
                 .subscription_hook = &ProtocolTerminalRaceGate::subscription_notify});
            REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
            auto challenge = std::async(
                std::launch::async, [&session] { return session.challenge(stream_challenge()); });
            auto invalid = stream_answer(wait_challenge(captured), 1);
            invalid.answer.erase("nonce");
            AnswerDisposition disposition = AnswerDisposition::RequestTerminated;

            if (protocol_wins) {
                gate.block = ProtocolTerminalRaceGate::Block::ReceiveOwner;
                std::thread auth([&] {
                    hub->claim_generation(1001, 7,
                                          {.cause = StreamTerminalCause::AuthorizationLost,
                                           .auth_state = 12,
                                           .metadata_failure = {}});
                });
                gate.wait();
                disposition = session.receive_answer(std::move(invalid));
                gate.release();
                auth.join();
            } else {
                gate.block = ProtocolTerminalRaceGate::Block::ProtocolOwned;
                std::thread protocol(
                    [&] { disposition = session.receive_answer(std::move(invalid)); });
                gate.wait();
                hub->claim_generation(1001, 7,
                                      {.cause = StreamTerminalCause::AuthorizationLost,
                                       .auth_state = 12,
                                       .metadata_failure = {}});
                gate.release();
                protocol.join();
            }
            CHECK(disposition == AnswerDisposition::Rejected);
            CHECK(challenge.get().status() == ChallengeStatus::ProtocolError);
            CHECK_FALSE(captured.error);

            ManualPoll poll;
            std::optional<StreamTerminalPayload> observed;
            std::size_t builder_calls = 0;
            const auto status = run_stream_delivery(
                session, {.count = std::nullopt,
                          .terminal_builder =
                              [&observed, &builder_calls](const StreamTerminalPayload& terminal,
                                                          std::uint64_t delivered) {
                                  static_cast<void>(delivered);
                                  observed = terminal;
                                  ++builder_calls;
                                  return terminal_frame(terminal, 0);
                              },
                          .hooks = poll.hooks()});
            CHECK(status == StreamDeliveryStatus::TerminalComplete);
            if (protocol_wins) {
                CHECK(builder_calls == 0);
                REQUIRE(captured.error);
                CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
            } else {
                CHECK(builder_calls == 1);
                REQUIRE(observed);
                CHECK(observed->cause == StreamTerminalCause::AuthorizationLost);
                CHECK(observed->auth_state == 12);
                REQUIRE(captured.error);
                CHECK(captured.error->code == "TERMINAL");
            }
        }
    }
}

TEST_CASE("protocol and deadline preserve both forced first-cause orders without sleep",
          "[stream][subscription][challenge][protocol-answer][deadline][concurrency]") {
    for (const bool protocol_wins : {true, false}) {
        DYNAMIC_SECTION("protocol_wins=" << protocol_wins) {
            ProtocolTerminalRaceGate protocol_gate;
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            const RequestDeadline deadline{RequestSession::Clock::now() + 1h};
            RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, deadline);
            auto activation = session.activate_stream_subscription(
                hub, ingress_request(), StreamActivityMode::UntrackedNoDaemon,
                {.context = &protocol_gate,
                 .hook = nullptr,
                 .subscription_hook = &ProtocolTerminalRaceGate::subscription_notify});
            REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
            auto challenge = std::async(
                std::launch::async, [&session] { return session.challenge(stream_challenge()); });
            auto invalid = stream_answer(wait_challenge(captured), 1);
            invalid.answer.erase("nonce");
            DeadlineClaimGate deadline_gate{protocol_wins ? DeadlineClaimGate::Block::BeforeClaim
                                                          : DeadlineClaimGate::Block::AfterClaim,
                                            *deadline.expires_at};
            std::optional<StreamTerminalPayload> observed;
            std::size_t builder_calls = 0;
            StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
            std::thread worker([&] {
                status = run_stream_delivery(
                    session, {.count = std::nullopt,
                              .terminal_builder =
                                  [&observed, &builder_calls](const StreamTerminalPayload& terminal,
                                                              std::uint64_t delivered) {
                                      static_cast<void>(delivered);
                                      observed = terminal;
                                      ++builder_calls;
                                      return terminal_frame(terminal, 0);
                                  },
                              .hooks = deadline_gate.hooks()});
            });
            AnswerDisposition disposition = AnswerDisposition::RequestTerminated;
            if (protocol_wins) {
                deadline_gate.wait();
                disposition = session.receive_answer(std::move(invalid));
                deadline_gate.release();
            } else {
                deadline_gate.wait();
                protocol_gate.block = ProtocolTerminalRaceGate::Block::ProtocolWaiting;
                std::thread protocol(
                    [&] { disposition = session.receive_answer(std::move(invalid)); });
                protocol_gate.wait();
                protocol_gate.release();
                protocol.join();
                deadline_gate.release();
            }
            worker.join();
            CHECK(disposition == AnswerDisposition::Rejected);
            CHECK(challenge.get().status() == ChallengeStatus::ProtocolError);
            CHECK(status == StreamDeliveryStatus::TerminalComplete);
            REQUIRE(captured.error);
            if (protocol_wins) {
                CHECK(builder_calls == 0);
                CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
            } else {
                CHECK(builder_calls == 1);
                REQUIRE(observed);
                CHECK(observed->cause == StreamTerminalCause::Deadline);
                CHECK(captured.error->code == "TERMINAL");
            }
        }
    }
}

TEST_CASE("a begun complete item counts before its protocol terminal and planned loses",
          "[stream][subscription][challenge][protocol-answer][delivery][count]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    auto sink = std::make_shared<BlockingSink>();
    RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
    activate(session, hub);
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    hub->publish(message_item("{}\n"));
    auto challenge = std::async(std::launch::async,
                                [&session] { return session.challenge(stream_challenge()); });
    auto invalid = stream_answer(sink->wait_challenge(), 1);
    invalid.answer.erase("nonce");
    ManualPoll poll;
    std::size_t builder_calls = 0;
    StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
    std::thread worker([&] {
        status = run_stream_delivery(
            session,
            {.count = 1,
             .terminal_builder =
                 [&builder_calls](const StreamTerminalPayload& terminal, std::uint64_t delivered) {
                     static_cast<void>(terminal);
                     static_cast<void>(delivered);
                     ++builder_calls;
                     return StreamTerminalResultFrame{{{"unexpected", true}}};
                 },
             .hooks = poll.hooks()});
    });
    sink->wait_item();
    const auto disposition = session.receive_answer(std::move(invalid));
    CHECK(disposition == AnswerDisposition::Rejected);
    CHECK(challenge.get().status() == ChallengeStatus::ProtocolError);
    sink->release_item();
    worker.join();

    CHECK(status == StreamDeliveryStatus::TerminalComplete);
    CHECK(sink->items() == 1);
    CHECK(sink->terminals() == 1);
    CHECK(builder_calls == 0);
    const auto error = sink->terminal_error();
    REQUIRE(error);
    CHECK(error->code == "PROTOCOL_ANSWER_INVALID");
    CHECK(error->details == json{{"request_id", 1}, {"reason", "malformed"}});
}

TEST_CASE("a failed begun item disconnects without forwarding its protocol terminal",
          "[stream][subscription][challenge][protocol-answer][delivery][disconnect]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    auto sink = std::make_shared<BlockingSink>(DeliveryOutcome::Disconnected);
    RequestSession session(request(), sink, 17, {}, {}, {}, RequestDeadline{});
    activate(session, hub);
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    hub->publish(message_item("{}\n"));
    auto challenge = std::async(std::launch::async,
                                [&session] { return session.challenge(stream_challenge()); });
    auto invalid = stream_answer(sink->wait_challenge(), 1);
    invalid.answer.erase("nonce");
    ManualPoll poll;
    std::size_t builder_calls = 0;
    StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
    std::thread worker([&] {
        status = run_stream_delivery(
            session,
            {.count = 1,
             .terminal_builder =
                 [&builder_calls](const StreamTerminalPayload& terminal, std::uint64_t delivered) {
                     static_cast<void>(terminal);
                     static_cast<void>(delivered);
                     ++builder_calls;
                     return StreamTerminalResultFrame{{{"unexpected", true}}};
                 },
             .hooks = poll.hooks()});
    });
    sink->wait_item();
    const auto disposition = session.receive_answer(std::move(invalid));
    CHECK(disposition == AnswerDisposition::Rejected);
    CHECK(challenge.get().status() == ChallengeStatus::ProtocolError);
    sink->release_item();
    worker.join();

    CHECK(status == StreamDeliveryStatus::Disconnected);
    CHECK(sink->terminals() == 0);
    CHECK(builder_calls == 0);
    CHECK(session.cancellation_requested());
    auto replacement = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
}

TEST_CASE("stream activation closes Open loss capacity and post-promotion terminal",
          "[stream][subscription][activation][activity][concurrency]") {
    SECTION("Open lost before promotion") {
        ActivityTracker tracker([] {});
        REQUIRE(tracker.daemon_ready(std::nullopt));
        auto activity = tracker.try_request();
        REQUIRE(activity);
        auto hub = std::make_shared<StreamIngressHub>();
        hub->begin_generation(1001, 7);
        Captured captured;
        RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity),
                               {}, RequestDeadline{});
        ActivationGate gate(hub);
        StreamSubscriptionActivationResult result{StreamIngressInvalidRequest{}};
        std::thread activation([&] {
            result = session.activate_stream_subscription(
                hub, ingress_request(), StreamActivityMode::TrackedDaemon,
                {.context = &gate, .hook = &ActivationGate::notify});
        });
        gate.wait();
        session.disconnect();
        gate.release();
        activation.join();
        CHECK(std::get<StreamSubscriptionActivationFailure>(result) ==
              StreamSubscriptionActivationFailure::RequestClosed);
        CHECK(tracker.snapshot().requests == 0);
        CHECK(tracker.snapshot().subscriptions == 0);
    }

    SECTION("capacity fails before promotion") {
        ActivityTracker tracker([] {});
        REQUIRE(tracker.daemon_ready(std::nullopt));
        auto activity = tracker.try_request();
        REQUIRE(activity);
        auto hub = std::make_shared<StreamIngressHub>();
        hub->begin_generation(1001, 7);
        std::vector<StreamIngressReservation> occupied;
        occupied.reserve(kStreamSubscriberSlots);
        for (std::size_t index = 0; index < kStreamSubscriberSlots; ++index) {
            auto admission = hub->reserve(ingress_request());
            REQUIRE(std::holds_alternative<StreamIngressReservation>(admission));
            occupied.push_back(std::move(std::get<StreamIngressReservation>(admission)));
        }
        Captured captured;
        RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity),
                               {}, RequestDeadline{});
        auto result = session.activate_stream_subscription(hub, ingress_request(),
                                                           StreamActivityMode::TrackedDaemon);
        REQUIRE(std::holds_alternative<StreamIngressAdmissionFailure>(result));
        CHECK(tracker.snapshot().requests == 1);
        CHECK(tracker.snapshot().subscriptions == 0);
        static_cast<void>(session.result(json::object()));
        CHECK(tracker.snapshot().requests == 0);
    }

    SECTION("terminal before promotion prevents accounting promotion") {
        ActivityTracker tracker([] {});
        REQUIRE(tracker.daemon_ready(std::nullopt));
        auto activity = tracker.try_request();
        REQUIRE(activity);
        auto hub = std::make_shared<StreamIngressHub>();
        hub->begin_generation(1001, 7);
        Captured captured;
        RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity),
                               {}, RequestDeadline{});
        ActivationGate gate(hub);
        gate.claim_shutdown = true;
        StreamSubscriptionActivationResult result{StreamIngressInvalidRequest{}};
        std::thread activation([&] {
            result = session.activate_stream_subscription(
                hub, ingress_request(), StreamActivityMode::TrackedDaemon,
                {.context = &gate, .hook = &ActivationGate::notify});
        });
        gate.wait();
        gate.release();
        activation.join();
        REQUIRE(std::holds_alternative<StreamSubscriptionTerminalClaimed>(result));
        CHECK(std::get<StreamSubscriptionTerminalClaimed>(result).terminal.cause ==
              StreamTerminalCause::Shutdown);
        CHECK(tracker.snapshot().requests == 1);
        CHECK(tracker.snapshot().subscriptions == 0);
        static_cast<void>(
            session.error("TERMINAL", "activation terminal", json::object(), kGeneric));
        CHECK(tracker.snapshot().requests == 0);
    }

    SECTION("terminal after promotion still publishes without an idle gap") {
        ActivityTracker tracker([] {});
        REQUIRE(tracker.daemon_ready(std::nullopt));
        auto activity = tracker.try_request();
        REQUIRE(activity);
        auto hub = std::make_shared<StreamIngressHub>();
        hub->begin_generation(1001, 7);
        Captured captured;
        RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity),
                               {}, RequestDeadline{});
        ActivationGate gate(testing::StreamActivationProbePoint::AfterPromotion, hub, true);
        StreamSubscriptionActivationResult result{StreamIngressInvalidRequest{}};
        std::thread activation([&] {
            result = session.activate_stream_subscription(
                hub, ingress_request(), StreamActivityMode::TrackedDaemon,
                {.context = &gate, .hook = &ActivationGate::notify});
        });
        gate.wait();
        auto state = tracker.snapshot();
        CHECK(state.requests == 0);
        CHECK(state.subscriptions == 1);
        CHECK_FALSE(state.zero_since);
        std::atomic<bool> control_complete = false;
        std::thread disconnect([&] { session.disconnect(); });
        std::thread control([&] {
            hub->poll_control();
            control_complete.store(true, std::memory_order_release);
        });
        control.join();
        CHECK(control_complete.load(std::memory_order_acquire));
        gate.release();
        activation.join();
        disconnect.join();
        REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(result));
        ManualPoll poll;
        CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                            .terminal_builder = &terminal_frame,
                                            .hooks = poll.hooks()}) ==
              StreamDeliveryStatus::TerminalComplete);
        CHECK(tracker.snapshot().subscriptions == 0);
    }
}

TEST_CASE("stream activation rechecks deadline and cancellation before every promotion",
          "[stream][subscription][activation][deadline][cancellation]") {
    for (const auto mode :
         {StreamActivityMode::TrackedDaemon, StreamActivityMode::UntrackedNoDaemon}) {
        DYNAMIC_SECTION("expired at entry mode=" << static_cast<int>(mode)) {
            ActivityTracker tracker([] {});
            REQUIRE(tracker.daemon_ready(std::nullopt));
            ActivityTracker::Token token;
            if (mode == StreamActivityMode::TrackedDaemon) {
                auto request_activity = tracker.try_request();
                REQUIRE(request_activity);
                token = std::move(*request_activity);
            }
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(token),
                                   {}, RequestDeadline{RequestSession::Clock::now()});

            auto result = session.activate_stream_subscription(hub, ingress_request(), mode);
            REQUIRE(std::holds_alternative<StreamSubscriptionTerminalClaimed>(result));
            CHECK(std::get<StreamSubscriptionTerminalClaimed>(result).terminal.cause ==
                  StreamTerminalCause::Deadline);
            if (mode == StreamActivityMode::TrackedDaemon) {
                CHECK(tracker.snapshot().requests == 1);
                CHECK(tracker.snapshot().subscriptions == 0);
                static_cast<void>(
                    session.error("TIMEOUT", "request timed out", json::object(), kTimeout));
                CHECK(tracker.snapshot().requests == 0);
            }
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }

        DYNAMIC_SECTION("expires before promotion mode=" << static_cast<int>(mode)) {
            ActivityTracker tracker([] {});
            REQUIRE(tracker.daemon_ready(std::nullopt));
            ActivityTracker::Token token;
            if (mode == StreamActivityMode::TrackedDaemon) {
                auto request_activity = tracker.try_request();
                REQUIRE(request_activity);
                token = std::move(*request_activity);
            }
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            const RequestDeadline deadline{RequestSession::Clock::now() + 10ms};
            RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(token),
                                   {}, deadline);
            ActivationGate gate(hub);
            StreamSubscriptionActivationResult result{StreamIngressInvalidRequest{}};
            std::thread activation([&] {
                result = session.activate_stream_subscription(
                    hub, ingress_request(), mode,
                    {.context = &gate, .hook = &ActivationGate::notify});
            });
            gate.wait();
            while (!deadline_expired(deadline)) {
                std::this_thread::yield();
            }
            gate.release();
            activation.join();

            REQUIRE(std::holds_alternative<StreamSubscriptionTerminalClaimed>(result));
            CHECK(std::get<StreamSubscriptionTerminalClaimed>(result).terminal.cause ==
                  StreamTerminalCause::Deadline);
            if (mode == StreamActivityMode::TrackedDaemon) {
                CHECK(tracker.snapshot().requests == 1);
                CHECK(tracker.snapshot().subscriptions == 0);
                static_cast<void>(
                    session.error("TIMEOUT", "request timed out", json::object(), kTimeout));
                CHECK(tracker.snapshot().requests == 0);
            }
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }

        DYNAMIC_SECTION("cancelled before promotion mode=" << static_cast<int>(mode)) {
            ActivityTracker tracker([] {});
            REQUIRE(tracker.daemon_ready(std::nullopt));
            ActivityTracker::Token token;
            if (mode == StreamActivityMode::TrackedDaemon) {
                auto request_activity = tracker.try_request();
                REQUIRE(request_activity);
                token = std::move(*request_activity);
            }
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(token),
                                   {}, RequestDeadline{});
            session.disconnect();

            auto result = session.activate_stream_subscription(hub, ingress_request(), mode);
            CHECK(std::get<StreamSubscriptionActivationFailure>(result) ==
                  StreamSubscriptionActivationFailure::RequestClosed);
            CHECK(tracker.snapshot().requests == 0);
            CHECK(tracker.snapshot().subscriptions == 0);
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
}

TEST_CASE("disconnect tears down Armed Installing and Published subscription states",
          "[stream][subscription][disconnect][activation][concurrency]") {
    for (const auto phase : {StreamIngressState::Armed, StreamIngressState::Published}) {
        DYNAMIC_SECTION("phase=" << static_cast<int>(phase)) {
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {},
                                   RequestDeadline{});
            activate(session, hub);
            if (phase == StreamIngressState::Published) {
                REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
            }
            session.disconnect();
            ManualPoll poll;
            CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                                .terminal_builder = &terminal_frame,
                                                .hooks = poll.hooks()}) ==
                  StreamDeliveryStatus::Disconnected);
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }

    SECTION("Installing") {
        InstallingGate gate;
        auto hub = std::make_shared<StreamIngressHub>(tgcli::daemon::detail::StreamIngressProbe{
            .context = &gate, .hook = &InstallingGate::notify, .forced_lock_free_failure = {}});
        hub->begin_generation(1001, 7);
        Captured captured;
        RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {},
                               RequestDeadline{});
        activate(session, hub);
        std::size_t activated = 1;
        std::thread receive([&] { activated = hub->activate_armed(1001, 7, 1); });
        gate.wait();
        session.disconnect();
        gate.release();
        receive.join();
        CHECK(activated == 0);
        ManualPoll poll;
        CHECK(run_stream_delivery(session, {.count = std::nullopt,
                                            .terminal_builder = &terminal_frame,
                                            .hooks = poll.hooks()}) ==
              StreamDeliveryStatus::Disconnected);
        auto replacement = hub->reserve(ingress_request());
        CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
    }
}

TEST_CASE("subscription activity remains held until publisher-quiescent reclamation",
          "[stream][subscription][activity][reclaim][concurrency]") {
    ReclaimProbe reclaim;
    auto hub = std::make_shared<StreamIngressHub>(tgcli::daemon::detail::StreamIngressProbe{
        .context = &reclaim, .hook = &ReclaimProbe::notify, .forced_lock_free_failure = {}});
    hub->begin_generation(1001, 7);
    ActivityTracker tracker([] {});
    REQUIRE(tracker.daemon_ready(std::nullopt));
    auto activity = tracker.try_request();
    REQUIRE(activity);
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity), {},
                           RequestDeadline{});
    auto activation = session.activate_stream_subscription(hub, ingress_request(),
                                                           StreamActivityMode::TrackedDaemon);
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
    StreamIngressTestAccess::hold_publisher(*hub, true);
    ManualPoll poll;
    StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
    std::thread worker([&] {
        status = run_stream_delivery(
            session,
            {.count = std::nullopt, .terminal_builder = &terminal_frame, .hooks = poll.hooks()});
    });
    reclaim.wait();
    CHECK(tracker.snapshot().subscriptions == 1);
    StreamIngressTestAccess::hold_publisher(*hub, false);
    worker.join();
    CHECK(status == StreamDeliveryStatus::TerminalComplete);
    CHECK(tracker.snapshot().subscriptions == 0);
}

TEST_CASE("terminal teardown waits for an admitted publisher before poison and reuse",
          "[stream][subscription][terminal][publisher][reclaim][concurrency]") {
    PublishRetireGate gate;
    auto hub = std::make_shared<StreamIngressHub>(tgcli::daemon::detail::StreamIngressProbe{
        .context = &gate, .hook = &PublishRetireGate::notify, .forced_lock_free_failure = {}});
    hub->begin_generation(1001, 7);
    std::vector<StreamIngressReservation> occupied;
    occupied.reserve(kStreamSubscriberSlots - 1);
    for (std::size_t index = 1; index < kStreamSubscriberSlots; ++index) {
        auto admission = hub->reserve(ingress_request());
        REQUIRE(std::holds_alternative<StreamIngressReservation>(admission));
        occupied.push_back(std::move(std::get<StreamIngressReservation>(admission)));
    }
    ActivityTracker tracker([] {});
    REQUIRE(tracker.daemon_ready(std::nullopt));
    auto activity = tracker.try_request();
    REQUIRE(activity);
    auto sink = std::make_shared<BlockingSink>();
    sink->release_item();
    RequestSession session(request(), sink, 17, {}, std::move(*activity), {}, RequestDeadline{});
    auto activation = session.activate_stream_subscription(hub, ingress_request(),
                                                           StreamActivityMode::TrackedDaemon);
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);

    std::thread publisher([&] { hub->publish(message_item("{}\n")); });
    gate.wait_write();
    CHECK(StreamIngressTestAccess::publisher_count(*hub) == 1);
    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});
    ManualPoll poll;
    StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
    std::thread worker([&] {
        status = run_stream_delivery(
            session,
            {.count = std::nullopt, .terminal_builder = &terminal_frame, .hooks = poll.hooks()});
    });
    gate.wait_reclaim();

    CHECK(StreamIngressTestAccess::publisher_count(*hub) == 1);
    CHECK(tracker.snapshot().subscriptions == 1);
    CHECK_FALSE(gate.discarded());
    auto blocked = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressAdmissionFailure>(blocked));

    gate.release_write();
    publisher.join();
    worker.join();
    CHECK(status == StreamDeliveryStatus::TerminalComplete);
    CHECK(sink->items() == 0);
    CHECK(sink->terminals() == 1);
    CHECK(tracker.snapshot().subscriptions == 0);
    auto replacement = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
}

TEST_CASE("terminal claim owns its reservation through concurrent reclaim",
          "[stream][subscription][terminal][reclaim][concurrency]") {
    ClaimRetireGate gate;
    auto hub = std::make_shared<StreamIngressHub>(
        tgcli::daemon::detail::StreamIngressProbe{.context = &gate,
                                                  .hook = &ClaimRetireGate::ingress_notify,
                                                  .forced_lock_free_failure = {}});
    hub->begin_generation(1001, 7);
    std::vector<StreamIngressReservation> occupied;
    occupied.reserve(kStreamSubscriberSlots - 1);
    for (std::size_t index = 1; index < kStreamSubscriberSlots; ++index) {
        auto admission = hub->reserve(ingress_request());
        REQUIRE(std::holds_alternative<StreamIngressReservation>(admission));
        occupied.push_back(std::move(std::get<StreamIngressReservation>(admission)));
    }
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, RequestDeadline{});
    auto activation = session.activate_stream_subscription(
        hub, ingress_request(), StreamActivityMode::UntrackedNoDaemon,
        {.context = &gate,
         .hook = nullptr,
         .subscription_hook = &ClaimRetireGate::subscription_notify});
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});

    std::thread disconnect([&] { session.disconnect(); });
    gate.wait_first_owned();
    ManualPoll poll;
    StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
    std::thread worker([&] {
        status = run_stream_delivery(
            session,
            {.count = std::nullopt, .terminal_builder = &terminal_frame, .hooks = poll.hooks()});
    });
    gate.wait_second_waiting();

    const auto [owned_before_release, reclaimed_before_release] = gate.snapshot();
    CHECK(owned_before_release == 1);
    CHECK_FALSE(reclaimed_before_release);
    auto blocked = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressAdmissionFailure>(blocked));

    gate.release();
    disconnect.join();
    worker.join();
    CHECK(status == StreamDeliveryStatus::TerminalComplete);
    REQUIRE(captured.error);
    CHECK(captured.error->details["cause"] == static_cast<int>(StreamTerminalCause::Shutdown));
    CHECK(gate.snapshot().second);
    auto replacement = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
}

TEST_CASE("retired subscription rejects a late claim before ingress access",
          "[stream][subscription][terminal][reclaim][concurrency]") {
    RetireClaimGate gate;
    auto hub = std::make_shared<StreamIngressHub>(
        tgcli::daemon::detail::StreamIngressProbe{.context = &gate,
                                                  .hook = &RetireClaimGate::ingress_notify,
                                                  .forced_lock_free_failure = {}});
    hub->begin_generation(1001, 7);
    std::vector<StreamIngressReservation> occupied;
    occupied.reserve(kStreamSubscriberSlots - 1);
    for (std::size_t index = 1; index < kStreamSubscriberSlots; ++index) {
        auto admission = hub->reserve(ingress_request());
        REQUIRE(std::holds_alternative<StreamIngressReservation>(admission));
        occupied.push_back(std::move(std::get<StreamIngressReservation>(admission)));
    }
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, RequestDeadline{});
    auto activation = session.activate_stream_subscription(
        hub, ingress_request(), StreamActivityMode::UntrackedNoDaemon,
        {.context = &gate,
         .hook = nullptr,
         .subscription_hook = &RetireClaimGate::subscription_notify});
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    hub->claim_control_generation(1001, 7,
                                  {.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}});

    ManualPoll poll;
    StreamDeliveryStatus status = StreamDeliveryStatus::InvalidLease;
    std::thread worker([&] {
        status = run_stream_delivery(
            session,
            {.count = std::nullopt, .terminal_builder = &terminal_frame, .hooks = poll.hooks()});
    });
    gate.wait_retire_owned();
    std::thread disconnect([&] { session.disconnect(); });
    gate.wait_claim_waiting();

    const auto [forwarded_before_release, reclaimed_before_release] = gate.snapshot();
    CHECK(forwarded_before_release == 0);
    CHECK_FALSE(reclaimed_before_release);
    auto blocked = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressAdmissionFailure>(blocked));

    gate.release();
    worker.join();
    disconnect.join();
    CHECK(status == StreamDeliveryStatus::TerminalComplete);
    REQUIRE(captured.error);
    CHECK(captured.error->details["cause"] == static_cast<int>(StreamTerminalCause::Shutdown));
    const auto [forwarded_after_reclaim, reclaimed_after_release] = gate.snapshot();
    CHECK(forwarded_after_reclaim == 0);
    CHECK(reclaimed_after_release);
    auto replacement = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
}

TEST_CASE("request session destruction tears down its owned subscription lease exactly once",
          "[stream][subscription][activity][destructor]") {
    ActivityTracker tracker([] {});
    REQUIRE(tracker.daemon_ready(std::nullopt));
    auto activity = tracker.try_request();
    REQUIRE(activity);
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    {
        RequestSession session(request(), capturing_sink(captured), 17, {}, std::move(*activity),
                               {}, RequestDeadline{});
        auto activation = session.activate_stream_subscription(hub, ingress_request(),
                                                               StreamActivityMode::TrackedDaemon);
        REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
        CHECK(tracker.snapshot().subscriptions == 1);
    }
    CHECK(tracker.snapshot().requests == 0);
    CHECK(tracker.snapshot().subscriptions == 0);
    auto replacement = hub->reserve(ingress_request());
    CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
}

TEST_CASE("request session observes only the actual published activation projection",
          "[stream][subscription][activation][publication]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, RequestDeadline{});
    auto activation = session.activate_stream_subscription(
        hub, ingress_request(StreamOperation::WaitFor), StreamActivityMode::UntrackedNoDaemon);
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
    CHECK_FALSE(session.stream_activation_projection());
    REQUIRE(hub->activate_armed(1001, 7, 41) == 1);
    const auto projection = session.stream_activation_projection();
    REQUIRE(projection);
    CHECK(projection->client_id == 1001);
    CHECK(projection->generation == 7);
    CHECK(projection->activation_receive_sequence == 41);
    CHECK(projection->operation == StreamOperation::WaitFor);
}

TEST_CASE("match delivery preserves the ingress descriptor and returns only the message result",
          "[stream][subscription][match][routing]") {
    auto hub = std::make_shared<StreamIngressHub>();
    hub->begin_generation(1001, 7);
    Captured captured;
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, RequestDeadline{});
    auto wait_request = ingress_request(StreamOperation::WaitFor);
    wait_request.mode = StreamMode::Match;
    auto activation = session.activate_stream_subscription(hub, wait_request,
                                                           StreamActivityMode::UntrackedNoDaemon);
    REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
    REQUIRE(hub->activate_armed(1001, 7, 1) == 1);
    const json message{{"id", 77}, {"chat_id", 42}, {"text", "target"}};
    hub->publish(message_item(json{{"event", "message"}, {"message", message}}.dump() + "\n", 9));

    ManualPoll poll;
    std::optional<StreamIngressDescriptor> observed;
    CHECK(run_stream_match_delivery(
              session, {.initial_match = std::nullopt,
                        .item_matcher = [&](const StreamCopiedItem& item) -> std::optional<json> {
                            observed = item.descriptor;
                            CHECK(item.wire_bytes != 0);
                            return item.data.at("message");
                        },
                        .terminal_builder = &terminal_frame,
                        .hooks = poll.hooks()}) == StreamDeliveryStatus::TerminalComplete);
    REQUIRE(observed);
    CHECK(observed->receive_sequence == 9);
    CHECK(observed->chat_id == 42);
    CHECK(observed->sender_kind == StreamSenderKind::User);
    CHECK(observed->sender_id == 7);
    CHECK(captured.items.empty());
    REQUIRE(captured.result);
    CHECK(*captured.result == message);
}
