#include "common/exit_codes.hpp"
#include "daemon/request_session.hpp"
#include "daemon/stream_subscription.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

proto::Request request() {
    proto::Request value("main");
    value.id = 1;
    value.command = {"internal-stream-test"};
    value.context.cwd = "/";
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
    std::vector<json> items;
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
        });
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
        static_cast<void>(code);
        static_cast<void>(message);
        static_cast<void>(details);
        static_cast<void>(exit_code);
        terminals_.fetch_add(1, std::memory_order_release);
        if (throw_terminal_) {
            throw std::runtime_error("terminal delivery failed");
        }
        return terminal_outcome_;
    }

    ChallengeReply emit_challenge(json data) override {
        static_cast<void>(data);
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
    const RequestDeadline deadline{StreamPollSchedule::Clock::time_point{} + 2ms};
    RequestSession session(request(), capturing_sink(captured), 17, {}, {}, {}, deadline);
    activate(session, hub);
    ManualPoll poll;

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

TEST_CASE("promoted result and error terminals release subscription activity exactly once",
          "[stream][subscription][activity][terminal]") {
    for (const bool emit_error : {false, true}) {
        DYNAMIC_SECTION("terminal=" << (emit_error ? "error" : "result")) {
            ActivityTracker tracker([] {});
            REQUIRE(tracker.daemon_ready(std::nullopt));
            auto activity = tracker.try_request();
            REQUIRE(activity);
            auto hub = std::make_shared<StreamIngressHub>();
            hub->begin_generation(1001, 7);
            Captured captured;
            RequestSession session(request(), capturing_sink(captured), 17, {},
                                   std::move(*activity), {}, RequestDeadline{});
            REQUIRE_FALSE(session.deadline().expires_at);
            auto activation = session.activate_stream_subscription(
                hub, ingress_request(), StreamActivityMode::TrackedDaemon);
            REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(activation));
            CHECK(tracker.snapshot().requests == 0);
            CHECK(tracker.snapshot().subscriptions == 1);

            const auto outcome =
                emit_error ? session.error("TERMINAL", "stream terminal", json::object(), kGeneric)
                           : session.result({{"complete", true}});
            CHECK(outcome == DeliveryOutcome::Complete);
            CHECK(session.result({{"late", true}}) == DeliveryOutcome::Suppressed);
            CHECK(tracker.snapshot().requests == 0);
            CHECK(tracker.snapshot().subscriptions == 0);
            auto replacement = hub->reserve(ingress_request());
            CHECK(std::holds_alternative<StreamIngressReservation>(replacement));
        }
    }
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
        CHECK(std::get<StreamSubscriptionActivationFailure>(result) ==
              StreamSubscriptionActivationFailure::TerminalClaimed);
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
            CHECK(std::get<StreamSubscriptionActivationFailure>(result) ==
                  StreamSubscriptionActivationFailure::TerminalClaimed);
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

            CHECK(std::get<StreamSubscriptionActivationFailure>(result) ==
                  StreamSubscriptionActivationFailure::TerminalClaimed);
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
