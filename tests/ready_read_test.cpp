#include "daemon/dispatch.hpp"
#include "daemon/logout_commands.hpp"
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
#include <string_view>
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
    ~PollBarrier() {
        release();
    }

    PollBarrier() = default;
    PollBarrier(const PollBarrier&) = delete;
    PollBarrier& operator=(const PollBarrier&) = delete;
    PollBarrier(PollBarrier&&) = delete;
    PollBarrier& operator=(PollBarrier&&) = delete;

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

class BarrierReleaseGuard {
  public:
    explicit BarrierReleaseGuard(PollBarrier& barrier) : barrier_(barrier) {}

    ~BarrierReleaseGuard() {
        barrier_.release();
    }

    BarrierReleaseGuard(const BarrierReleaseGuard&) = delete;
    BarrierReleaseGuard& operator=(const BarrierReleaseGuard&) = delete;
    BarrierReleaseGuard(BarrierReleaseGuard&&) = delete;
    BarrierReleaseGuard& operator=(BarrierReleaseGuard&&) = delete;

  private:
    PollBarrier& barrier_;
};

class ObservationProbe {
  public:
    using Clock = tgcli::core::TdEventClock;

    void observe(Clock::time_point observed_at) {
        const std::lock_guard lock(mutex_);
        ++count_;
        last_observed_at_ = observed_at;
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(mutex_);
        return count_;
    }

    bool await_count(std::size_t expected, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return count_ >= expected; });
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

    bool await_count(std::size_t expected, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return count_ >= expected; });
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t count_ = 0;
};

class AuditedSession {
  public:
    explicit AuditedSession(tgcli::daemon::RequestSession::Clock::time_point deadline) {
        sink_ = std::make_unique<tgcli::daemon::CallbackSink>(
            [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
            [](const nlohmann::json&) {},
            [](const std::string&, const std::string&, const nlohmann::json&, int) {});
        tgcli::proto::Request request("main");
        request.id = 2;
        request.command = {"logout"};
        request.context.timeout_seconds = 10.0;
        request.context.cwd = "/";
        session_ = std::make_unique<tgcli::daemon::RequestSession>(
            std::move(request), *sink_, 0, tgcli::daemon::RequestSession::NonceGenerator{},
            tgcli::daemon::ActivityTracker::Token{}, nullptr, deadline);
        REQUIRE(session_->begin_audited_terminal() ==
                tgcli::daemon::AuditedTerminalStatus::Designated);
    }

    ~AuditedSession() {
        session_->audit_fatal();
    }

    AuditedSession(const AuditedSession&) = delete;
    AuditedSession& operator=(const AuditedSession&) = delete;
    AuditedSession(AuditedSession&&) = delete;
    AuditedSession& operator=(AuditedSession&&) = delete;

    tgcli::daemon::RequestSession& session() {
        return *session_;
    }

  private:
    std::unique_ptr<tgcli::daemon::CallbackSink> sink_;
    std::unique_ptr<tgcli::daemon::RequestSession> session_;
};

class ReadyReadHarness {
  public:
    using Clock = tgcli::core::TdEventClock;

    explicit ReadyReadHarness(tgcli::RequestDeadline request_deadline)
        : request_deadline_(request_deadline),
          deadline_(request_deadline_.expires_at.value_or(Clock::time_point(5s))),
          clock_(deadline_ - 1ms) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(
            std::move(runtime), tgcli::core::TdLogConfiguration{},
            tgcli::core::TdClientEventHooks{
                .now = [this] { return clock_.now(); },
                .after_observed =
                    [this](Clock::time_point observed_at) { observation_.observe(observed_at); },
                .before_lifecycle_callback_drain_wait =
                    [this] { lifecycle_callback_wait_.notify(); },
                .before_closed_decisions_drain_wait = [this] { closed_decisions_wait_.notify(); }});
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
            tgcli::daemon::ActivityTracker::Token{}, nullptr, request_deadline_);
    }

    ~ReadyReadHarness() {
        if (session_) {
            session_->audit_fatal();
        }
        barrier_.release();
        if (result_.valid()) {
            result_.wait();
        }
        const auto current = client_->auth_state();
        if (current && current->data.state == tgcli::core::AuthState::Closed) {
            static_cast<void>(runtime_->wait_for_clients(current->client_generation + 1));
        }
        const auto bootstrap = client_->auth_state();
        if (bootstrap && bootstrap->auth_sequence == 0) {
            const auto clients = runtime_->clients();
            if (!clients.empty()) {
                const auto replacement = clients.back();
                runtime_->push_response(
                    replacement, 1, {},
                    tgcli::core::AuthStateData{tgcli::core::AuthState::WaitPhoneNumber});
                static_cast<void>(wait_auth(replacement.client_generation, 1));
            }
        }
        client_->close();
    }

    ReadyReadHarness(const ReadyReadHarness&) = delete;
    ReadyReadHarness& operator=(const ReadyReadHarness&) = delete;
    ReadyReadHarness(ReadyReadHarness&&) = delete;
    ReadyReadHarness& operator=(ReadyReadHarness&&) = delete;

    void start() {
        start_with([this](const std::shared_ptr<const tgcli::core::AuthStateSnapshot>& ready) {
            return client_->get_chat(ready, -1);
        });
        REQUIRE(runtime_->wait_for_sent(2));
        REQUIRE(barrier_.await_entry());
    }

    void start_with(tgcli::daemon::ReadyReadStart start) {
        result_ = std::async(std::launch::async, [this, start = std::move(start)] {
            tgcli::daemon::ReadyReadSession reads(
                *client_, *session_,
                {.now = [this] { return clock_.now(); },
                 .wait = [this](const auto&, const auto&) { barrier_.wait(); },
                 .before_event_arbitration = [this] { arbitration_.notify(); },
                 .before_wait = {}});
            auto snapshot = reads.current();
            return reads.read(start, snapshot);
        });
    }

    void start_default() {
        result_ = std::async(std::launch::async, [this] {
            tgcli::daemon::ReadyReadSession reads(
                *client_, *session_,
                {.now = [this] { return clock_.now(); },
                 .wait = {},
                 .before_event_arbitration = {},
                 .before_wait = [this] { default_waits_.notify(); }});
            auto snapshot = reads.current();
            return reads.read([this](const auto& ready) { return client_->get_chat(ready, -1); },
                              snapshot);
        });
        REQUIRE(runtime_->wait_for_sent(2));
        REQUIRE(default_waits_.await_count(1));
    }

    void await_wait() {
        REQUIRE(barrier_.await_entry());
    }

    void push_response(Clock::time_point observed_at) {
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == 2);
        const auto expected = observation_.count() + 1;
        clock_.set(observed_at);
        runtime_->push_response(td_client_, sent.back().query_id,
                                tgcli::core::TdValue::from(tgcli::core::TdOk{}));
        REQUIRE(observation_.await_count(expected));
    }

    void push_auth(tgcli::core::AuthState state, Clock::time_point observed_at) {
        const auto expected = observation_.count() + 1;
        enqueue_auth(state, observed_at);
        REQUIRE(observation_.await_count(expected));
    }

    void enqueue_auth(tgcli::core::AuthState state, Clock::time_point observed_at) {
        clock_.set(observed_at);
        runtime_->push_update(td_client_, {}, tgcli::core::AuthStateData{state});
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

    bool result_ready_within(std::chrono::milliseconds timeout) {
        return result_.wait_for(timeout) == std::future_status::ready;
    }

    bool await_default_waits(std::size_t expected, std::chrono::milliseconds timeout) {
        return default_waits_.await_count(expected, timeout);
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
        return observation_.last_observed_at();
    }

    tgcli::core::TdClient& client() {
        return *client_;
    }

    tgcli::test::ScriptedTdRuntime& runtime() {
        return *runtime_;
    }

    bool await_lifecycle_callback_wait(std::size_t expected) {
        return lifecycle_callback_wait_.await_count(expected);
    }

    bool await_closed_decisions_wait(std::size_t expected) {
        return closed_decisions_wait_.await_count(expected);
    }

    [[nodiscard]] std::size_t lifecycle_callback_wait_count() const {
        return lifecycle_callback_wait_.count();
    }

    [[nodiscard]] std::size_t closed_decisions_wait_count() const {
        return closed_decisions_wait_.count();
    }

    bool wait_auth(std::uint64_t generation, std::uint64_t sequence) const {
        const auto until = Clock::now() + 2s;
        while (Clock::now() < until) {
            const auto snapshot = client_->auth_state();
            if (snapshot->client_generation == generation && snapshot->auth_sequence >= sequence) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        const auto snapshot = client_->auth_state();
        return snapshot->client_generation == generation && snapshot->auth_sequence >= sequence;
    }

  private:
    tgcli::RequestDeadline request_deadline_;
    Clock::time_point deadline_;
    ManualClock clock_;
    PollBarrier barrier_;
    ObservationProbe observation_;
    CallProbe arbitration_;
    CallProbe lifecycle_callback_wait_;
    CallProbe closed_decisions_wait_;
    CallProbe default_waits_;
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

TEST_CASE("Unlimited Ready read default waits are event driven",
          "[ready-read][deadline][unlimited][event-driven]") {
    ReadyReadHarness harness(tgcli::RequestDeadline{});
    harness.start_default();
    CHECK_FALSE(harness.await_default_waits(2, 100ms));

    harness.push_response(tgcli::core::TdEventClock::time_point(5s) - 1ns);
    const auto result = harness.result();
    CHECK(result.status == tgcli::daemon::ReadyReadStatus::Response);
    CHECK(result.value.get_if<tgcli::core::TdOk>() != nullptr);
}

TEST_CASE("Ready read uses event observation time at the absolute deadline",
          "[ready-read][resolver][saved][deadline][fake-boundary]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;

    SECTION("published response wins before the deadline claim") {
        ReadyReadHarness harness(deadline);
        harness.start();
        harness.push_response(deadline - 1ns);
        harness.push_ready_sentinel(deadline + 1ns);
        REQUIRE(harness.wait_auth_sequence(2));
        harness.consume_after_deadline();
        auto result = harness.result();
        CHECK(result.status == tgcli::daemon::ReadyReadStatus::Response);
        CHECK(result.value.get_if<tgcli::core::TdOk>() != nullptr);
        REQUIRE(result.value.receive_observed_at());
        CHECK(*result.value.receive_observed_at() == deadline - 1ns);
    }

    SECTION("published authorization loss wins before the deadline claim") {
        for (int iteration = 0; iteration < 32; ++iteration) {
            CAPTURE(iteration);
            ReadyReadHarness harness(deadline);
            harness.start();
            harness.push_auth(tgcli::core::AuthState::WaitCode, deadline - 1ns);
            REQUIRE(harness.wait_auth_sequence(2));
            harness.consume_after_deadline();
            const auto result = harness.result();
            CHECK(result.status == tgcli::daemon::ReadyReadStatus::AuthorizationLost);
            REQUIRE(result.snapshot != nullptr);
            CHECK(result.snapshot->data.state == tgcli::core::AuthState::WaitCode);
            REQUIRE(result.snapshot->receive_observed_at);
            CHECK(*result.snapshot->receive_observed_at == deadline - 1ns);
        }
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

TEST_CASE("Ready read deadline is not blocked by an outbound auth publication prerequisite",
          "[ready-read][resolver][saved][deadline][outbound][concurrency]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;
    ReadyReadHarness harness(deadline);
    harness.start();

    PollBarrier outbound;
    const BarrierReleaseGuard release_outbound(outbound);
    harness.runtime().set_before_send([&](const tgcli::core::TdFunctionData& function) {
        if (function.has_type("getChat")) {
            outbound.wait();
        }
    });
    const auto ready = harness.client().auth_state();
    auto blocked_send =
        std::async(std::launch::async, [&] { return harness.client().get_chat(ready, -2); });
    REQUIRE(outbound.await_entry());

    const auto received = harness.runtime().received_count();
    harness.enqueue_auth(tgcli::core::AuthState::Ready, deadline);
    REQUIRE(harness.runtime().wait_for_received(received + 1));

    harness.release_at_deadline();
    const auto deadline_won = harness.result_ready_within(500ms);
    CHECK(deadline_won);
    CHECK(blocked_send.wait_for(0ms) != std::future_status::ready);
    std::optional<tgcli::daemon::ReadyReadResult> result;
    if (deadline_won) {
        result = harness.result();
        CHECK(result->status == tgcli::daemon::ReadyReadStatus::TimedOut);
    }

    harness.runtime().set_before_send({});
    outbound.release();
    auto response = blocked_send.get();
    REQUIRE(harness.runtime().wait_for_sent(3));
    const auto sent = harness.runtime().sent_functions();
    REQUIRE(sent.back().client_id == ready->client_id);
    harness.runtime().push_response(
        tgcli::test::ScriptedClient{sent.back().client_id, sent.back().client_generation},
        sent.back().query_id, tgcli::core::TdValue::from(tgcli::core::TdOk{}));
    CHECK(response.get().get_if<tgcli::core::TdOk>() != nullptr);
    REQUIRE(harness.wait_auth_sequence(2));
    const auto advanced = harness.client().auth_state();
    CHECK(advanced->data.state == tgcli::core::AuthState::Ready);
    CHECK(advanced->auth_sequence == 2);
    REQUIRE(advanced->receive_observed_at);
    CHECK(*advanced->receive_observed_at == deadline);
    if (!result) {
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::TimedOut);
    }
}

TEST_CASE("Ready read retries only pre-boundary authorization races",
          "[ready-read][resolver][saved][authorization][retry]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;
    const auto ready_value = [deadline] {
        std::promise<tgcli::core::TdValue> promise;
        auto future = promise.get_future();
        auto value = tgcli::core::TdValue::from(tgcli::core::TdOk{});
        value.set_receive_event_metadata(100, deadline - 1ns);
        promise.set_value(std::move(value));
        return future;
    };

    for (const auto failure : {tgcli::core::TdAuthorizationFailure::GenerationMismatch,
                               tgcli::core::TdAuthorizationFailure::AuthSequenceMismatch,
                               tgcli::core::TdAuthorizationFailure::GenerationClosed}) {
        DYNAMIC_SECTION("retryable " << tgcli::core::authorization_failure_name(failure)) {
            ReadyReadHarness harness(deadline);
            std::promise<tgcli::core::TdValue> first;
            auto first_future = first.get_future();
            std::size_t attempts = 0;
            harness.start_with([&](const auto&) mutable {
                ++attempts;
                if (attempts == 1) {
                    return std::move(first_future);
                }
                return ready_value();
            });
            harness.await_wait();
            harness.push_ready_sentinel(deadline - 2ns);
            first.set_exception(
                std::make_exception_ptr(tgcli::core::TdAuthorizationError(failure)));
            harness.release_before_deadline();
            const auto result = harness.result();
            CHECK(result.status == tgcli::daemon::ReadyReadStatus::Response);
            CHECK(result.value.get_if<tgcli::core::TdOk>() != nullptr);
            CHECK(attempts == 2);
        }
    }

    for (const auto failure : {tgcli::core::TdAuthorizationFailure::FunctionMismatch,
                               tgcli::core::TdAuthorizationFailure::TierMismatch,
                               tgcli::core::TdAuthorizationFailure::OwnerMismatch,
                               tgcli::core::TdAuthorizationFailure::AuthStateMismatch,
                               tgcli::core::TdAuthorizationFailure::FunctionDenied}) {
        DYNAMIC_SECTION("terminal " << tgcli::core::authorization_failure_name(failure)) {
            ReadyReadHarness harness(deadline);
            std::promise<tgcli::core::TdValue> first;
            auto first_future = first.get_future();
            std::size_t attempts = 0;
            harness.start_with([&](const auto&) mutable {
                ++attempts;
                return std::move(first_future);
            });
            harness.await_wait();
            harness.push_ready_sentinel(deadline - 2ns);
            first.set_exception(
                std::make_exception_ptr(tgcli::core::TdAuthorizationError(failure)));
            harness.release_before_deadline();
            const auto result = harness.result();
            CHECK(result.status == tgcli::daemon::ReadyReadStatus::Failed);
            CHECK(result.authorization_failure == failure);
            CHECK(attempts == 1);
        }
    }

    SECTION("generic runtime error is terminal") {
        ReadyReadHarness harness(deadline);
        std::promise<tgcli::core::TdValue> first;
        auto first_future = first.get_future();
        std::size_t attempts = 0;
        harness.start_with([&](const auto&) mutable {
            ++attempts;
            return std::move(first_future);
        });
        harness.await_wait();
        harness.push_ready_sentinel(deadline - 2ns);
        first.set_exception(std::make_exception_ptr(std::runtime_error("scripted failure")));
        harness.release_before_deadline();
        const auto result = harness.result();
        CHECK(result.status == tgcli::daemon::ReadyReadStatus::Failed);
        CHECK_FALSE(result.authorization_failure.has_value());
        CHECK(attempts == 1);
    }
}

TEST_CASE("Ready read deadline arbitration excludes blocking lifecycle drains",
          "[ready-read][lifecycle][deadline][fake-boundary]") {
    const auto deadline = tgcli::core::TdEventClock::now() + 5s;

    SECTION("an in-flight lifecycle callback does not hold the publication gate") {
        ReadyReadHarness harness(deadline);
        harness.start();
        AuditedSession lifecycle(tgcli::daemon::RequestSession::Clock::now() + 5s);
        PollBarrier terminal_claim;
        CallProbe terminal_claims;
        auto decision = tgcli::daemon::LogoutLifecycle::begin(
            harness.client(), harness.client().auth_state(), lifecycle.session(), [&] {
                terminal_claims.notify();
                terminal_claim.wait();
            });
        REQUIRE(decision);
        auto settlement =
            std::async(std::launch::async, [&] { return decision.settle_terminal(); });
        const BarrierReleaseGuard release_terminal_claim(terminal_claim);
        REQUIRE(terminal_claim.await_entry());

        harness.enqueue_auth(tgcli::core::AuthState::Closing, deadline - 1ns);
        REQUIRE(harness.await_lifecycle_callback_wait(1));
        harness.release_at_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::TimedOut);

        terminal_claim.release();
        CHECK(settlement.get() == tgcli::core::TdClosedDecisionStatus::Pending);
        REQUIRE(harness.wait_auth(1, 2));
        const auto snapshot = harness.client().auth_state();
        CHECK(snapshot->data.state == tgcli::core::AuthState::Closing);
        CHECK(snapshot->auth_sequence == 2);
        REQUIRE(snapshot->receive_observed_at);
        CHECK(*snapshot->receive_observed_at == deadline);
        CHECK(terminal_claims.count() == 2);
        CHECK(harness.lifecycle_callback_wait_count() == 1);
    }

    SECTION("a pending closed decision does not hold the publication gate") {
        ReadyReadHarness harness(deadline);
        harness.start();
        AuditedSession lifecycle(tgcli::daemon::RequestSession::Clock::now() + 5s);
        auto decision = tgcli::daemon::LogoutLifecycle::begin(
            harness.client(), harness.client().auth_state(), lifecycle.session());
        REQUIRE(decision);

        harness.enqueue_auth(tgcli::core::AuthState::Closed, deadline);
        REQUIRE(harness.await_closed_decisions_wait(1));
        harness.release_at_deadline();
        CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::TimedOut);
        CHECK(decision.status() == tgcli::core::TdClosedDecisionStatus::Rejected);
        const auto closed = harness.client().auth_state();
        CHECK(closed->data.state == tgcli::core::AuthState::Closed);
        REQUIRE(closed->receive_observed_at);
        CHECK(*closed->receive_observed_at == deadline);

        decision = tgcli::core::TdClosedDecision{};
        REQUIRE(harness.runtime().wait_for_clients(2));
        const auto clients = harness.runtime().clients();
        REQUIRE(clients.size() == 2);
        harness.runtime().push_response(
            clients.back(), 1, {},
            tgcli::core::AuthStateData{tgcli::core::AuthState::WaitPhoneNumber});
        REQUIRE(harness.wait_auth(2, 1));
        CHECK(harness.closed_decisions_wait_count() == 1);
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

TEST_CASE("Unlimited Ready reads terminate only through non-time cancellation",
          "[ready-read][deadline][unlimited][cancellation][fake-boundary]") {
    for (const auto cause : {std::string_view("audit"), std::string_view("disconnect"),
                             std::string_view("shutdown")}) {
        DYNAMIC_SECTION(cause) {
            ReadyReadHarness harness(tgcli::RequestDeadline{});
            harness.start_default();
            CHECK_FALSE(harness.result_ready_within(0ms));

            if (cause == "audit") {
                harness.session().audit_fatal();
            } else if (cause == "disconnect") {
                harness.session().disconnect();
            } else {
                harness.session().shutdown();
            }
            CHECK(harness.result().status == tgcli::daemon::ReadyReadStatus::Cancelled);
            CHECK(harness.terminal_count() == (cause == "shutdown" ? 1 : 0));
        }
    }
}
