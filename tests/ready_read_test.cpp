#include "daemon/dispatch.hpp"
#include "daemon/ready_read.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace {

class ManualClock {
  public:
    explicit ManualClock(tgcli::core::TdEventClock::time_point now) : now_(now) {}

    [[nodiscard]] tgcli::core::TdEventClock::time_point now() const {
        const std::lock_guard lock(mutex_);
        return now_;
    }

    void set(tgcli::core::TdEventClock::time_point now) {
        const std::lock_guard lock(mutex_);
        now_ = now;
    }

  private:
    mutable std::mutex mutex_;
    tgcli::core::TdEventClock::time_point now_;
};

class PollBarrier {
  public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }

    bool await_entry() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

class PublicationProbe {
  public:
    using Clock = tgcli::core::TdEventClock;

    void observe(Clock::time_point observed_at) {
        std::unique_lock lock(mutex_);
        ++count_;
        last_observed_at_ = observed_at;
        if (!pause_next_) {
            cv_.notify_all();
            return;
        }
        pause_next_ = false;
        paused_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
        paused_ = false;
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(mutex_);
        return count_;
    }

    bool await_count(std::size_t expected) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 2s, [this, expected] { return count_ >= expected; });
    }

    void pause_next() {
        const std::lock_guard lock(mutex_);
        pause_next_ = true;
        paused_ = false;
        released_ = false;
    }

    bool await_pause() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 2s, [this] { return paused_; });
    }

    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] std::optional<Clock::time_point> last_observed_at() const {
        const std::lock_guard lock(mutex_);
        return last_observed_at_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t count_ = 0;
    std::optional<Clock::time_point> last_observed_at_;
    bool pause_next_ = false;
    bool paused_ = false;
    bool released_ = false;
};

class CallProbe {
  public:
    void notify() {
        const std::lock_guard lock(mutex_);
        ++count_;
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(mutex_);
        return count_;
    }

    bool await_count(std::size_t expected) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 2s, [this, expected] { return count_ >= expected; });
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t count_ = 0;
};

class ReadyReadHarness {
  public:
    using Clock = tgcli::core::TdEventClock;

    explicit ReadyReadHarness(Clock::time_point deadline)
        : deadline_(deadline), clock_(deadline - 1ms) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(
            std::move(runtime), tgcli::core::TdLogConfiguration{},
            tgcli::core::TdClientEventHooks{
                .now = [this] { return clock_.now(); },
                .after_observed =
                    [this](Clock::time_point observed_at) { publication_.observe(observed_at); }});
        REQUIRE(runtime_->wait_for_sent(1));
        REQUIRE(runtime_->clients().size() == 1);
        td_client_ = runtime_->clients().front();
        runtime_->push_response(td_client_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(wait_auth_sequence(1));

        sink_ = std::make_unique<tgcli::daemon::CallbackSink>(
            [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
            [this](const nlohmann::json&) { ++terminal_count_; },
            [this](std::string code, const std::string&, const nlohmann::json&, int) {
                terminal_code_ = std::move(code);
                ++terminal_count_;
            });
        tgcli::proto::Request request("main");
        request.id = 1;
        request.command = {"resolve"};
        request.context.timeout_seconds = 1.0;
        request.context.cwd = "/";
        session_ = std::make_unique<tgcli::daemon::RequestSession>(
            std::move(request), *sink_, 0, tgcli::daemon::RequestSession::NonceGenerator{},
            tgcli::daemon::ActivityTracker::Token{}, nullptr, deadline_);
    }

    ~ReadyReadHarness() {
        if (session_) {
            session_->audit_fatal();
        }
        publication_.release();
        barrier_.release();
        if (result_.valid()) {
            result_.wait();
        }
    }

    ReadyReadHarness(const ReadyReadHarness&) = delete;
    ReadyReadHarness& operator=(const ReadyReadHarness&) = delete;
    ReadyReadHarness(ReadyReadHarness&&) = delete;
    ReadyReadHarness& operator=(ReadyReadHarness&&) = delete;

    void start() {
        result_ = std::async(std::launch::async, [this] {
            tgcli::daemon::ReadyReadSession reads(
                *client_, *session_,
                {.now = [this] { return clock_.now(); },
                 .wait = [this] { barrier_.wait(); },
                 .before_event_arbitration = [this] { arbitration_.notify(); }});
            auto snapshot = reads.current();
            return reads.read(
                [this](const std::shared_ptr<const tgcli::core::AuthStateSnapshot>& ready) {
                    return client_->get_chat(ready, -1);
                },
                snapshot);
        });
        REQUIRE(runtime_->wait_for_sent(2));
        REQUIRE(barrier_.await_entry());
    }

    void push_response(Clock::time_point observed_at) {
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == 2);
        const auto expected = publication_.count() + 1;
        clock_.set(observed_at);
        runtime_->push_response(td_client_, sent.back().query_id,
                                tgcli::core::TdValue::from(tgcli::core::TdOk{}));
        REQUIRE(publication_.await_count(expected));
    }

    void push_auth(tgcli::core::AuthState state, Clock::time_point observed_at) {
        const auto expected = publication_.count() + 1;
        clock_.set(observed_at);
        runtime_->push_update(td_client_, {}, tgcli::core::AuthStateData{state});
        REQUIRE(publication_.await_count(expected));
    }

    void pause_response(Clock::time_point observed_at) {
        publication_.pause_next();
        push_response(observed_at);
        REQUIRE(publication_.await_pause());
    }

    void pause_auth(tgcli::core::AuthState state, Clock::time_point observed_at) {
        publication_.pause_next();
        push_auth(state, observed_at);
        REQUIRE(publication_.await_pause());
    }

    void push_ready_sentinel(Clock::time_point observed_at) {
        push_auth(tgcli::core::AuthState::Ready, observed_at);
    }

    bool wait_auth_sequence(std::uint64_t sequence) const {
        const auto until = Clock::now() + 2s;
        while (Clock::now() < until) {
            if (client_->auth_state()->auth_sequence >= sequence) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return client_->auth_state()->auth_sequence >= sequence;
    }

    void consume_after_deadline() {
        const auto expected = arbitration_.count() + 1;
        clock_.set(deadline_ + 1ns);
        barrier_.release();
        REQUIRE(arbitration_.await_count(expected));
    }

    void release_before_deadline() {
        clock_.set(deadline_ - 1ns);
        barrier_.release();
    }

    void release_at_deadline() {
        clock_.set(deadline_);
        barrier_.release();
    }

    void release_publication() {
        publication_.release();
    }

    bool result_ready_within(std::chrono::milliseconds timeout) {
        return result_.wait_for(timeout) == std::future_status::ready;
    }

    tgcli::daemon::ReadyReadResult result() {
        return result_.get();
    }

    tgcli::daemon::RequestSession& session() {
        return *session_;
    }

    [[nodiscard]] int terminal_count() const {
        return terminal_count_;
    }

    [[nodiscard]] const std::optional<std::string>& terminal_code() const {
        return terminal_code_;
    }

    [[nodiscard]] std::optional<Clock::time_point> last_observed_at() const {
        return publication_.last_observed_at();
    }

  private:
    Clock::time_point deadline_;
    ManualClock clock_;
    PollBarrier barrier_;
    PublicationProbe publication_;
    CallProbe arbitration_;
    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    tgcli::test::ScriptedClient td_client_{};
    std::unique_ptr<tgcli::core::TdClient> client_;
    std::unique_ptr<tgcli::daemon::CallbackSink> sink_;
    std::unique_ptr<tgcli::daemon::RequestSession> session_;
    std::future<tgcli::daemon::ReadyReadResult> result_;
    std::optional<std::string> terminal_code_;
    int terminal_count_ = 0;
};

} // namespace

TEST_CASE("Ready read uses event observation time at the absolute deadline",
          "[ready-read][resolver][saved][deadline][fake-boundary]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;

    SECTION("response publisher wins the gate before the deadline claim") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.pause_response(deadline - 1ns);
        harness.consume_after_deadline();
        CHECK_FALSE(harness.result_ready_within(0ms));
        harness.release_publication();
        auto result = harness.result();
        CHECK(result.status == tgcli::daemon::ReadyReadStatus::Response);
        CHECK(result.value.get_if<tgcli::core::TdOk>() != nullptr);
        REQUIRE(result.value.receive_observed_at());
        CHECK(*result.value.receive_observed_at() == deadline - 1ns);
    }

    SECTION("authorization publisher wins the gate before the deadline claim") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.pause_auth(tgcli::core::AuthState::WaitCode, deadline - 1ns);
        harness.consume_after_deadline();
        CHECK_FALSE(harness.result_ready_within(0ms));
        harness.release_publication();
        const auto result = harness.result();
        CHECK(result.status == tgcli::daemon::ReadyReadStatus::AuthorizationLost);
        REQUIRE(result.snapshot != nullptr);
        CHECK(result.snapshot->data.state == tgcli::core::AuthState::WaitCode);
        REQUIRE(result.snapshot->receive_observed_at);
        CHECK(*result.snapshot->receive_observed_at == deadline - 1ns);
    }

    SECTION("deadline claim wins before response publication") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.release_at_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::TimedOut);

        harness.push_response(deadline);
        REQUIRE(harness.last_observed_at());
        CHECK(*harness.last_observed_at() == deadline);
    }

    SECTION("deadline claim wins before authorization publication") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.release_at_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::TimedOut);

        harness.push_auth(tgcli::core::AuthState::WaitCode, deadline);
        REQUIRE(harness.wait_auth_sequence(2));
        REQUIRE(harness.last_observed_at());
        CHECK(*harness.last_observed_at() == deadline);
    }

    SECTION("response at or after the deadline loses to timeout") {
        for (const auto observed_at : {deadline, deadline + 1ns}) {
            DYNAMIC_SECTION((observed_at == deadline ? "equal" : "after")) {
                ReadyReadHarness harness(deadline);
                harness.start();
                harness.push_response(observed_at);
                harness.push_ready_sentinel(deadline + 2ns);
                REQUIRE(harness.wait_auth_sequence(2));
                harness.consume_after_deadline();
                CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::TimedOut);
            }
        }
    }

    SECTION("authorization loss at or after the deadline loses to timeout") {
        for (const auto observed_at : {deadline, deadline + 1ns}) {
            DYNAMIC_SECTION((observed_at == deadline ? "equal" : "after")) {
                ReadyReadHarness harness(deadline);
                harness.start();
                harness.push_auth(tgcli::core::AuthState::WaitCode, observed_at);
                REQUIRE(harness.wait_auth_sequence(2));
                harness.consume_after_deadline();
                CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::TimedOut);
            }
        }
    }
}

TEST_CASE("Ready read preserves eligible response and authorization event ordering",
          "[ready-read][resolver][saved][authorization][ordering][fake-boundary]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;

    SECTION("earlier authorization loss wins") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.push_auth(tgcli::core::AuthState::WaitCode, deadline - 3ns);
        harness.push_response(deadline - 2ns);
        harness.push_ready_sentinel(deadline + 1ns);
        REQUIRE(harness.wait_auth_sequence(3));
        harness.release_before_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::AuthorizationLost);
    }

    SECTION("earlier response wins") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.push_response(deadline - 3ns);
        harness.push_auth(tgcli::core::AuthState::WaitCode, deadline - 2ns);
        harness.push_ready_sentinel(deadline + 1ns);
        REQUIRE(harness.wait_auth_sequence(3));
        harness.release_before_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::Response);
    }
}

TEST_CASE("Ready read retains cancellation and shutdown terminal behavior",
          "[ready-read][resolver][saved][cancellation][shutdown][fake-boundary]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;

    SECTION("audit cancellation stops the waiter") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.session().audit_fatal();
        harness.release_before_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::Cancelled);
        CHECK(harness.terminal_count() == 0);
    }

    SECTION("shutdown emits its existing terminal once and cancels the waiter") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.session().shutdown();
        harness.release_before_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::Cancelled);
        CHECK(harness.terminal_count() == 1);
        REQUIRE(harness.terminal_code());
        CHECK(*harness.terminal_code() == "DAEMON_SHUTDOWN");
    }
}
