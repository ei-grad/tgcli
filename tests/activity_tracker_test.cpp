#include "daemon/activity_tracker.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using tgcli::daemon::ActivityTracker;

namespace {

class ManualClock {
  public:
    using Clock = ActivityTracker::Clock;

    [[nodiscard]] Clock::time_point now() const {
        return Clock::time_point(std::chrono::nanoseconds(ticks_.load(std::memory_order_relaxed)));
    }

    void advance(std::chrono::nanoseconds amount) {
        ticks_.fetch_add(amount.count(), std::memory_order_relaxed);
    }

    [[nodiscard]] std::shared_ptr<tgcli::daemon::testing::ActivityTrackerHooks> hooks() {
        auto hooks = std::make_shared<tgcli::daemon::testing::ActivityTrackerHooks>();
        hooks->now = [this] { return now(); };
        return hooks;
    }

  private:
    std::atomic<std::int64_t> ticks_ = 0;
};

class LockedGate {
  public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return entered_; });
    }

    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

} // namespace

TEST_CASE("daemon ready creates the initial zero-activity idle transition", "[daemon][activity]") {
    ManualClock clock;
    clock.advance(7s);
    ActivityTracker tracker([] {}, clock.hooks());

    auto state = tracker.snapshot();
    CHECK_FALSE(state.daemon_ready);
    CHECK_FALSE(state.zero_since);
    CHECK_FALSE(state.deadline);
    CHECK_FALSE(tracker.try_request());
    CHECK_FALSE(tracker.try_subscription());
    CHECK(tracker.snapshot().requests == 0);
    CHECK(tracker.snapshot().subscriptions == 0);

    REQUIRE(tracker.daemon_ready(10s));
    state = tracker.snapshot();
    CHECK(state.daemon_ready);
    CHECK(state.zero_since == clock.now());
    CHECK(state.deadline == clock.now() + 10s);
    auto request = tracker.try_request();
    auto subscription = tracker.try_subscription();
    REQUIRE(request);
    REQUIRE(subscription);
    request.reset();
    subscription.reset();
    CHECK(tracker.snapshot().zero_since == clock.now());
    CHECK(tracker.snapshot().deadline == clock.now() + 10s);
    CHECK_FALSE(tracker.daemon_ready(20s));
    CHECK(tracker.snapshot().idle_exit == 10s);
}

TEST_CASE("request and subscription tokens account independently until the final release",
          "[daemon][activity]") {
    ManualClock clock;
    ActivityTracker tracker([] {}, clock.hooks());
    REQUIRE(tracker.daemon_ready(10s));

    auto request = tracker.try_request();
    auto subscription = tracker.try_subscription();
    REQUIRE(request);
    REQUIRE(subscription);
    auto state = tracker.snapshot();
    CHECK(state.requests == 1);
    CHECK(state.subscriptions == 1);
    CHECK_FALSE(state.zero_since);
    CHECK_FALSE(state.deadline);

    clock.advance(3s);
    request.reset();
    state = tracker.snapshot();
    CHECK(state.requests == 0);
    CHECK(state.subscriptions == 1);
    CHECK_FALSE(state.zero_since);

    clock.advance(2s);
    subscription.reset();
    state = tracker.snapshot();
    CHECK(state.requests == 0);
    CHECK(state.subscriptions == 0);
    CHECK(state.zero_since == clock.now());
    CHECK(state.deadline == clock.now() + 10s);
}

TEST_CASE("activity tokens are move-only and release exactly once", "[daemon][activity]") {
    ManualClock clock;
    ActivityTracker tracker([] {}, clock.hooks());
    REQUIRE(tracker.daemon_ready(10s));

    auto first = tracker.try_request();
    auto second = tracker.try_request();
    REQUIRE(first);
    REQUIRE(second);
    ActivityTracker::Token moved = std::move(*first);
    CHECK_FALSE(static_cast<bool>(*first));
    CHECK(tracker.snapshot().requests == 2);

    moved = std::move(*second);
    CHECK(tracker.snapshot().requests == 1);
    second.reset();
    CHECK(tracker.snapshot().requests == 1);
    moved.reset();
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("request activity promotes atomically to one subscription without an idle transition",
          "[daemon][activity]") {
    ManualClock clock;
    ActivityTracker tracker([] {}, clock.hooks());
    REQUIRE(tracker.daemon_ready(10s));
    auto activity = tracker.try_request();
    REQUIRE(activity);

    clock.advance(4s);
    REQUIRE(activity->promote_to_subscription());
    auto state = tracker.snapshot();
    CHECK(state.requests == 0);
    CHECK(state.subscriptions == 1);
    CHECK_FALSE(state.zero_since);
    CHECK_FALSE(state.deadline);
    CHECK_FALSE(activity->promote_to_subscription());

    ActivityTracker::Token moved = std::move(*activity);
    CHECK_FALSE(static_cast<bool>(*activity));
    activity.reset();
    CHECK(tracker.snapshot().subscriptions == 1);

    clock.advance(3s);
    moved.reset();
    state = tracker.snapshot();
    CHECK(state.requests == 0);
    CHECK(state.subscriptions == 0);
    CHECK(state.zero_since == clock.now());
    CHECK(state.deadline == clock.now() + 10s);
}

TEST_CASE("idle policy extension shortening and disable apply from the same zero transition",
          "[daemon][activity]") {
    ManualClock clock;
    std::atomic<int> callbacks = 0;
    ActivityTracker tracker([&] { callbacks.fetch_add(1, std::memory_order_relaxed); },
                            clock.hooks());
    REQUIRE(tracker.daemon_ready(10s));
    const auto zero = tracker.snapshot().zero_since;
    REQUIRE(zero);

    clock.advance(4s);
    CHECK_FALSE(tracker.update_idle_exit(20s));
    CHECK(tracker.snapshot().deadline == *zero + 20s);
    CHECK_FALSE(tracker.update_idle_exit(std::nullopt));
    CHECK_FALSE(tracker.snapshot().deadline);
    CHECK_FALSE(tracker.expire_if_due());

    CHECK_FALSE(tracker.update_idle_exit(5s));
    CHECK(tracker.snapshot().deadline == *zero + 5s);
    clock.advance(1s);
    CHECK(tracker.expire_if_due());
    CHECK(callbacks.load(std::memory_order_relaxed) == 1);
    CHECK_FALSE(tracker.expire_if_due());
    CHECK_FALSE(tracker.update_idle_exit(1s));
    CHECK(callbacks.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("shortened idle policy can request immediate expiry", "[daemon][activity]") {
    ManualClock clock;
    std::atomic<int> callbacks = 0;
    ActivityTracker tracker([&] { callbacks.fetch_add(1, std::memory_order_relaxed); },
                            clock.hooks());
    REQUIRE(tracker.daemon_ready(30s));
    clock.advance(10s);

    CHECK(tracker.update_idle_exit(5s));
    CHECK(tracker.snapshot().expired);
    CHECK(callbacks.load(std::memory_order_relaxed) == 1);
    CHECK_FALSE(tracker.try_request());
    CHECK_FALSE(tracker.try_subscription());
}

TEST_CASE("active leases prevent expiry and start a fresh idle deadline on release",
          "[daemon][activity]") {
    ManualClock clock;
    ActivityTracker tracker([] {}, clock.hooks());
    REQUIRE(tracker.daemon_ready(5s));
    auto request = tracker.try_request();
    REQUIRE(request);
    clock.advance(30s);
    CHECK_FALSE(tracker.expire_if_due());

    request.reset();
    const auto state = tracker.snapshot();
    CHECK(state.zero_since == clock.now());
    CHECK(state.deadline == clock.now() + 5s);
    CHECK_FALSE(tracker.expire_if_due());
}

TEST_CASE("expiry winning the atomic boundary rejects the racing admission",
          "[daemon][activity][race]") {
    ManualClock clock;
    LockedGate gate;
    auto hooks = clock.hooks();
    hooks->before_expire_locked = [&] { gate.wait(); };
    std::atomic<int> callbacks = 0;
    ActivityTracker tracker([&] { callbacks.fetch_add(1, std::memory_order_relaxed); }, hooks);
    REQUIRE(tracker.daemon_ready(1s));
    clock.advance(1s);

    bool expired = false;
    std::optional<ActivityTracker::Token> admission;
    std::thread expiry([&] { expired = tracker.expire_if_due(); });
    gate.wait_until_entered();
    std::thread admit([&] { admission = tracker.try_request(); });
    gate.release();
    expiry.join();
    admit.join();

    CHECK(expired);
    CHECK_FALSE(admission);
    CHECK(callbacks.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("admission winning the atomic boundary prevents the racing expiry",
          "[daemon][activity][race]") {
    ManualClock clock;
    LockedGate gate;
    auto hooks = clock.hooks();
    hooks->before_admit_locked = [&] { gate.wait(); };
    ActivityTracker tracker([] {}, hooks);
    REQUIRE(tracker.daemon_ready(1s));
    clock.advance(1s);

    bool expired = false;
    std::optional<ActivityTracker::Token> admission;
    std::thread admit([&] { admission = tracker.try_request(); });
    gate.wait_until_entered();
    std::thread expiry([&] { expired = tracker.expire_if_due(); });
    gate.release();
    admit.join();
    expiry.join();

    REQUIRE(admission);
    CHECK_FALSE(expired);
    CHECK_FALSE(tracker.snapshot().expired);
}

TEST_CASE("admit versus expiry stress has exactly one winner", "[daemon][activity][race]") {
    for (int iteration = 0; iteration < 500; ++iteration) {
        ManualClock clock;
        std::atomic<int> callbacks = 0;
        ActivityTracker tracker([&] { callbacks.fetch_add(1, std::memory_order_relaxed); },
                                clock.hooks());
        REQUIRE(tracker.daemon_ready(1s));
        clock.advance(1s);

        std::latch start(3);
        std::optional<ActivityTracker::Token> admission;
        bool expired = false;
        std::thread admit([&] {
            start.count_down();
            start.wait();
            admission = tracker.try_request();
        });
        std::thread expiry([&] {
            start.count_down();
            start.wait();
            expired = tracker.expire_if_due();
        });
        start.count_down();
        start.wait();
        admit.join();
        expiry.join();

        CHECK(static_cast<bool>(admission) != expired);
        CHECK(callbacks.load(std::memory_order_relaxed) == (expired ? 1 : 0));
    }
}

TEST_CASE("request promotion racing expiry never exposes zero activity",
          "[daemon][activity][race][stress]") {
    for (int iteration = 0; iteration < 500; ++iteration) {
        ManualClock clock;
        std::atomic<int> callbacks = 0;
        ActivityTracker tracker([&] { callbacks.fetch_add(1, std::memory_order_relaxed); },
                                clock.hooks());
        REQUIRE(tracker.daemon_ready(1s));
        auto activity = tracker.try_request();
        REQUIRE(activity);
        clock.advance(10s);

        std::latch start(3);
        bool promoted = false;
        bool expired = false;
        std::thread promote([&] {
            start.count_down();
            start.wait();
            promoted = activity->promote_to_subscription();
        });
        std::thread expiry([&] {
            start.count_down();
            start.wait();
            expired = tracker.expire_if_due();
        });
        start.count_down();
        start.wait();
        promote.join();
        expiry.join();

        CHECK(promoted);
        CHECK_FALSE(expired);
        const auto state = tracker.snapshot();
        CHECK(state.requests == 0);
        CHECK(state.subscriptions == 1);
        CHECK_FALSE(state.zero_since);
        CHECK_FALSE(state.deadline);
        CHECK(callbacks.load(std::memory_order_relaxed) == 0);
    }
}

TEST_CASE("non-positive idle policy is rejected", "[daemon][activity]") {
    ActivityTracker tracker([] {});
    CHECK_THROWS_AS(tracker.daemon_ready(0s), std::invalid_argument);
    REQUIRE(tracker.daemon_ready(std::nullopt));
    CHECK_THROWS_AS(tracker.update_idle_exit(-1s), std::invalid_argument);
}
