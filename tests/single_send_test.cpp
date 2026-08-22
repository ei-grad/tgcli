#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "daemon/single_send.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace {

class ManualClock {
  public:
    explicit ManualClock(tgcli::core::TdEventClock::time_point value) : value_(value) {}

    tgcli::core::TdEventClock::time_point now() const {
        const std::lock_guard lock(mutex_);
        return value_;
    }

    void set(tgcli::core::TdEventClock::time_point value) {
        const std::lock_guard lock(mutex_);
        value_ = value;
    }

  private:
    mutable std::mutex mutex_;
    tgcli::core::TdEventClock::time_point value_;
};

template <typename Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

tgcli::core::TdMessageSummary summary(std::int64_t id) {
    return {
        .id = id,
        .chat_id = -1001,
        .date = 1'785'924'000,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = true,
        .topic = tgcli::core::TdTopic{.kind = tgcli::core::TdTopicKind::Forum,
                                      .id = 9,
                                      .tdlib_type_id = 2},
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "sent"};
}

tgcli::core::TdWriteMessage stable_message(std::int64_t id = 101, bool scheduled = false) {
    return {.message = summary(id),
            .sending_state = {},
            .scheduling_state =
                scheduled
                    ? tgcli::core::
                          TdMessageSchedulingState{.kind = tgcli::core::
                                                       TdMessageSchedulingStateKind::SendWhenOnline,
                                                   .send_date = 0,
                                                   .repeat_period = 0,
                                                   .unsupported_tdlib_type_id = std::nullopt}
                    : tgcli::core::TdMessageSchedulingState{},
            .has_reply_markup = false};
}

tgcli::core::TdWriteMessage pending_message(std::int64_t id = -77,
                                            std::int32_t sending_id = 12345) {
    return {.message = summary(id),
            .sending_state = {.kind = tgcli::core::TdMessageSendingStateKind::Pending,
                              .sending_id = sending_id,
                              .error = std::nullopt,
                              .can_retry = false,
                              .need_another_sender = false,
                              .need_another_reply_quote = false,
                              .need_drop_reply = false,
                              .required_paid_message_star_count = 0,
                              .retry_after = 0,
                              .unsupported_tdlib_type_id = std::nullopt},
            .scheduling_state = {},
            .has_reply_markup = false};
}

tgcli::core::TdWriteMessage failed_message(std::int64_t id, tgcli::core::TdError error,
                                           double retry_after = 0) {
    return {.message = summary(id),
            .sending_state = {.kind = tgcli::core::TdMessageSendingStateKind::Failed,
                              .sending_id = 0,
                              .error = std::move(error),
                              .can_retry = false,
                              .need_another_sender = false,
                              .need_another_reply_quote = false,
                              .need_drop_reply = false,
                              .required_paid_message_star_count = 0,
                              .retry_after = retry_after,
                              .unsupported_tdlib_type_id = std::nullopt},
            .scheduling_state = {},
            .has_reply_markup = false};
}

tgcli::core::TdSendMessageRequest send_request() {
    return {.chat_id = -1001,
            .topic = tgcli::core::TdTopic{.kind = tgcli::core::TdTopicKind::Forum,
                                          .id = 9,
                                          .tdlib_type_id = 0},
            .reply_to_message_id = std::nullopt,
            .options = {.disable_notification = false, .schedule = {}, .sending_id = 12345},
            .content = {.formatted_text = {.text = "send", .entities = {}, .capability = {}},
                        .parsed = false},
            .document = std::nullopt};
}

class SendHarness {
  public:
    using Clock = tgcli::core::TdEventClock;

    explicit SendHarness(bool unlimited = false)
        : deadline_(Clock::time_point(100s)), clock_(deadline_ - 1s),
          runtime_owner_(std::make_unique<tgcli::test::ScriptedTdRuntime>()),
          runtime_(runtime_owner_.get()),
          client_(std::make_unique<tgcli::core::TdClient>(
              std::move(runtime_owner_), tgcli::core::TdLogConfiguration{},
              tgcli::core::TdClientEventHooks{.now = [this] { return clock_.now(); },
                                              .after_observed = {},
                                              .before_lifecycle_callback_drain_wait = {},
                                              .before_closed_decisions_drain_wait = {}})) {
        REQUIRE(runtime_->wait_for_sent(1));
        REQUIRE(runtime_->clients().size() == 1);
        first_ = runtime_->clients().front();
        runtime_->push_response(first_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(eventually([this] { return client_->auth_state()->auth_sequence == 1; }));
        sink_ = std::make_unique<tgcli::daemon::CallbackSink>(
            [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
            [](const nlohmann::json&) {},
            [](const std::string&, const std::string&, const nlohmann::json&, int) {});
        tgcli::proto::Request request("main");
        request.id = 17;
        request.command = {"send"};
        request.context.timeout_seconds = 10.0;
        request.context.cwd = "/";
        session_ = std::make_unique<tgcli::daemon::RequestSession>(
            std::move(request), *sink_, 0, tgcli::daemon::RequestSession::NonceGenerator{},
            tgcli::daemon::ActivityTracker::Token{}, nullptr,
            unlimited ? tgcli::RequestDeadline{} : tgcli::RequestDeadline{deadline_});
    }

    ~SendHarness() {
        coordinator_.reset();
        const auto clients = runtime_->clients();
        if (!clients.empty()) {
            const auto current = clients.back();
            static_cast<void>(eventually([&] {
                return client_->auth_state()->client_generation == current.client_generation;
            }));
            if (client_->auth_state()->auth_sequence == 0) {
                runtime_->push_response(current, 1, {},
                                        tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
                static_cast<void>(
                    eventually([&] { return client_->auth_state()->auth_sequence != 0; }));
            }
        }
        client_->close();
    }

    SendHarness(const SendHarness&) = delete;
    SendHarness& operator=(const SendHarness&) = delete;
    SendHarness(SendHarness&&) = delete;
    SendHarness& operator=(SendHarness&&) = delete;

    std::future<tgcli::daemon::SingleSendOutcome>
    execute(tgcli::daemon::SingleSendHooks hooks = {}) {
        if (!hooks.now) {
            hooks.now = [this] { return clock_.now(); };
        }
        coordinator_ = std::make_unique<tgcli::daemon::SingleSendCoordinator>(*client_, *session_,
                                                                              std::move(hooks));
        const auto authorization = client_->auth_state();
        return std::async(std::launch::async, [this, authorization] {
            return coordinator_->execute(send_request(), authorization);
        });
    }

    tgcli::test::SentTdFunction await_send() {
        REQUIRE(runtime_->wait_for_sent(2));
        const auto sent = runtime_->sent_functions().back();
        REQUIRE(sent.function.kind() == tgcli::core::TdFunctionKind::SendMessage);
        return sent;
    }

    std::size_t send_count() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(runtime_->sent_functions(), [](const auto& sent) {
                return sent.function.kind() == tgcli::core::TdFunctionKind::SendMessage;
            }));
    }

    tgcli::test::ScriptedTdRuntime& runtime() {
        return *runtime_;
    }

    tgcli::test::ScriptedClient first() const {
        return first_;
    }

    tgcli::daemon::RequestSession& session() {
        return *session_;
    }

    tgcli::core::TdClient& client() {
        return *client_;
    }

    ManualClock& clock() {
        return clock_;
    }

    Clock::time_point deadline() const {
        return deadline_;
    }

  private:
    Clock::time_point deadline_;
    ManualClock clock_;
    std::unique_ptr<tgcli::test::ScriptedTdRuntime> runtime_owner_;
    tgcli::test::ScriptedTdRuntime* runtime_;
    std::unique_ptr<tgcli::core::TdClient> client_;
    tgcli::test::ScriptedClient first_{};
    std::unique_ptr<tgcli::daemon::CallbackSink> sink_;
    std::unique_ptr<tgcli::daemon::RequestSession> session_;
    std::unique_ptr<tgcli::daemon::SingleSendCoordinator> coordinator_;
};

} // namespace

TEST_CASE("single send accepts only an authoritative stable immediate Message",
          "[daemon][send][arbitration]") {
    SendHarness harness;
    auto outcome = harness.execute();
    const auto sent = harness.await_send();
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(stable_message(101, true)));
    auto result = outcome.get();
    const auto* success = std::get_if<tgcli::daemon::SingleSendSucceeded>(&result);
    REQUIRE(success != nullptr);
    CHECK_FALSE(success->temporary);
    CHECK(success->result.id == 101);
    CHECK(success->result.scheduled);
    CHECK_FALSE(success->result.date);
    CHECK(success->authoritative_message.message.id == 101);
    CHECK(harness.send_count() == 1);
}

TEST_CASE("single send buffers success before the pending response and emits temp first",
          "[daemon][send][arbitration][fake-boundary]") {
    SendHarness harness;
    std::mutex observed_mutex;
    std::vector<tgcli::daemon::SingleSendTemporaryId> observed;
    tgcli::daemon::SingleSendHooks hooks;
    hooks.on_temporary_id = [&](const auto& temporary) {
        const std::lock_guard lock(observed_mutex);
        observed.push_back(temporary);
    };
    auto outcome = harness.execute(std::move(hooks));
    const auto sent = harness.await_send();
    harness.runtime().push_message_send_succeeded(harness.first(), -77, stable_message(101));
    REQUIRE(harness.runtime().wait_for_received(2));
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(pending_message()));
    auto result = outcome.get();
    const auto* success = std::get_if<tgcli::daemon::SingleSendSucceeded>(&result);
    REQUIRE(success != nullptr);
    REQUIRE(success->temporary);
    CHECK(success->temporary->temporary_message_id == -77);
    CHECK(success->result.id == 101);
    {
        const std::lock_guard lock(observed_mutex);
        REQUIRE(observed.size() == 1);
        CHECK(observed.front() == *success->temporary);
    }
    CHECK(harness.send_count() == 1);
}

TEST_CASE("single send chooses earliest matching failure delete or success update",
          "[daemon][send][arbitration]") {
    SECTION("delete before success is ambiguous") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message()));
        REQUIRE(harness.runtime().wait_for_received(2));
        harness.runtime().push_delete_messages(harness.first(), -1001, {-77}, true, false);
        harness.runtime().push_message_send_succeeded(harness.first(), -77, stable_message());
        auto result = outcome.get();
        CHECK(std::get_if<tgcli::daemon::SingleSendDeletedBeforeConfirmation>(&result) != nullptr);
    }

    SECTION("explicit failure before success wins") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message()));
        REQUIRE(harness.runtime().wait_for_received(2));
        harness.runtime().push_message_send_failed(
            harness.first(), -77, failed_message(-77, tgcli::core::TdError{400, "SEND_FAILED"}),
            tgcli::core::TdError{400, "SEND_FAILED"});
        harness.runtime().push_message_send_succeeded(harness.first(), -77, stable_message());
        auto result = outcome.get();
        const auto* failed = std::get_if<tgcli::daemon::SingleSendFailed>(&result);
        REQUIRE(failed != nullptr);
        CHECK(failed->error.code == 400);
        CHECK(failed->mutation_state == tgcli::daemon::SingleSendMutationState::None);
    }

    SECTION("success before delete remains authoritative") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message()));
        REQUIRE(harness.runtime().wait_for_received(2));
        harness.runtime().push_message_send_succeeded(harness.first(), -77, stable_message(101));
        harness.runtime().push_delete_messages(harness.first(), -1001, {-77}, true, false);
        auto result = outcome.get();
        const auto* success = std::get_if<tgcli::daemon::SingleSendSucceeded>(&result);
        REQUIRE(success != nullptr);
        CHECK(success->result.id == 101);
    }
}

TEST_CASE("single send ignores wrong generation IDs duplicates and late updates",
          "[daemon][send][arbitration][generation]") {
    SendHarness harness;
    auto outcome = harness.execute();
    const auto sent = harness.await_send();
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(pending_message()));
    REQUIRE(harness.runtime().wait_for_received(2));
    const tgcli::test::ScriptedClient old{.client_id = harness.first().client_id,
                                          .client_generation = 0};
    harness.runtime().push_message_send_succeeded(old, -77, stable_message(901));
    harness.runtime().push_message_send_succeeded(harness.first(), -88, stable_message(902));
    harness.runtime().push_delete_messages(harness.first(), -2002, {-77}, true, false);
    harness.runtime().push_message_send_succeeded(harness.first(), -77, stable_message(101));
    harness.runtime().push_message_send_succeeded(harness.first(), -77, stable_message(102));
    auto result = outcome.get();
    const auto* success = std::get_if<tgcli::daemon::SingleSendSucceeded>(&result);
    REQUIRE(success != nullptr);
    CHECK(success->result.id == 101);
    CHECK(harness.send_count() == 1);
}

TEST_CASE("single send exposes explicit rate limit without mutation retry",
          "[daemon][send][arbitration][rate-limit]") {
    SendHarness harness;
    auto outcome = harness.execute();
    const auto sent = harness.await_send();
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(failed_message(
                                        -77, tgcli::core::TdError{429, "retry after 1"}, 1.25)));
    auto result = outcome.get();
    const auto* rate = std::get_if<tgcli::daemon::SingleSendRateLimited>(&result);
    REQUIRE(rate != nullptr);
    CHECK(rate->retry_after == 2);
    CHECK(rate->mutation_state == tgcli::daemon::SingleSendMutationState::None);
    CHECK(harness.send_count() == 1);

    SendHarness dispatch_harness;
    auto dispatch_outcome = dispatch_harness.execute();
    const auto dispatch_sent = dispatch_harness.await_send();
    dispatch_harness.runtime().push_response(
        dispatch_harness.first(), dispatch_sent.query_id,
        tgcli::core::TdValue::from(tgcli::core::TdError{429, "retry after 3"}));
    auto dispatch_result = dispatch_outcome.get();
    const auto* dispatch_rate = std::get_if<tgcli::daemon::SingleSendRateLimited>(&dispatch_result);
    REQUIRE(dispatch_rate != nullptr);
    CHECK(dispatch_rate->retry_after == 3);
    CHECK(dispatch_rate->mutation_state == tgcli::daemon::SingleSendMutationState::Possible);
    CHECK(dispatch_harness.send_count() == 1);

    struct RateLimitCase {
        std::string message;
        std::int32_t expected;
    };
    const std::vector cases{
        RateLimitCase{"Too Many Requests: ReTrY\tAfTeR   17 seconds", 17},
        RateLimitCase{"DC5 flood_wait_19 trailing text", 19},
        RateLimitCase{"retry after 7 trailing text", 7},
        RateLimitCase{"retry after 21474836499999999999 seconds",
                      std::numeric_limits<std::int32_t>::max()},
        RateLimitCase{"notretry after 29 seconds", 0},
        RateLimitCase{"prefix_FLOOD_WAIT_31", 0},
    };
    for (const auto& entry : cases) {
        CAPTURE(entry.message);
        SendHarness parser_harness;
        auto parser_outcome = parser_harness.execute();
        const auto parser_sent = parser_harness.await_send();
        parser_harness.runtime().push_response(
            parser_harness.first(), parser_sent.query_id,
            tgcli::core::TdValue::from(tgcli::core::TdError{429, entry.message}));
        auto parser_result = parser_outcome.get();
        const auto* parser_rate = std::get_if<tgcli::daemon::SingleSendRateLimited>(&parser_result);
        REQUIRE(parser_rate != nullptr);
        CHECK(parser_rate->retry_after == entry.expected);
        CHECK(parser_harness.send_count() == 1);
    }
}

TEST_CASE("single send deadline equality retains the observed temporary ID",
          "[daemon][send][arbitration][deadline]") {
    SendHarness harness;
    std::mutex mutex;
    std::condition_variable condition;
    bool observed = false;
    tgcli::daemon::SingleSendHooks hooks;
    hooks.on_temporary_id = [&](const auto&) {
        {
            const std::lock_guard lock(mutex);
            observed = true;
        }
        condition.notify_all();
    };
    auto outcome = harness.execute(std::move(hooks));
    const auto sent = harness.await_send();
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(pending_message()));
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&] { return observed; }));
    }
    harness.clock().set(harness.deadline());
    auto result = outcome.get();
    const auto* timeout = std::get_if<tgcli::daemon::SingleSendTimedOut>(&result);
    REQUIRE(timeout != nullptr);
    REQUIRE(timeout->temporary);
    CHECK(timeout->temporary->temporary_message_id == -77);
}

TEST_CASE("single send distinguishes authorization loss generation close and cancellation",
          "[daemon][send][arbitration][auth]") {
    SECTION("authorization loss") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message()));
        REQUIRE(harness.runtime().wait_for_received(2));
        harness.runtime().push_update(
            harness.first(), {}, tgcli::core::AuthStateData{tgcli::core::AuthState::LoggingOut});
        harness.runtime().push_message_send_succeeded(harness.first(), -77, stable_message());
        auto result = outcome.get();
        const auto* lost = std::get_if<tgcli::daemon::SingleSendAuthorizationLost>(&result);
        REQUIRE(lost != nullptr);
        CHECK(lost->state == tgcli::core::AuthState::LoggingOut);
    }

    SECTION("generation close") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message()));
        REQUIRE(harness.runtime().wait_for_received(2));
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::Closed});
        auto result = outcome.get();
        CHECK(std::get_if<tgcli::daemon::SingleSendGenerationClosed>(&result) != nullptr);
    }

    SECTION("cancellation after dispatch") {
        SendHarness harness;
        std::mutex mutex;
        std::condition_variable condition;
        bool temporary_observed = false;
        tgcli::daemon::SingleSendHooks hooks;
        hooks.on_temporary_id = [&](const auto&) {
            {
                const std::lock_guard lock(mutex);
                temporary_observed = true;
            }
            condition.notify_all();
        };
        auto outcome = harness.execute(std::move(hooks));
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message()));
        {
            std::unique_lock lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&] { return temporary_observed; }));
        }
        harness.session().disconnect();
        auto result = outcome.get();
        const auto* cancelled = std::get_if<tgcli::daemon::SingleSendCancelled>(&result);
        REQUIRE(cancelled != nullptr);
        REQUIRE(cancelled->temporary);
    }
}

TEST_CASE("single send preserves a pre-request authorization rejection as mutation-free",
          "[daemon][send][authorization][fake-boundary]") {
    SendHarness harness;
    tgcli::daemon::SingleSendHooks hooks;
    hooks.before_request = [&] {
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        if (!eventually([&] { return harness.client().auth_state()->auth_sequence == 2; })) {
            throw std::runtime_error("authorization update was not observed");
        }
    };
    auto pending = harness.execute(std::move(hooks));
    auto outcome = pending.get();
    const auto* rejected = std::get_if<tgcli::daemon::SingleSendRejected>(&outcome);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->authorization_failure ==
          tgcli::core::TdAuthorizationFailure::AuthSequenceMismatch);
    CHECK(rejected->mutation_state == tgcli::daemon::SingleSendMutationState::None);
    CHECK(harness.send_count() == 0);
}

TEST_CASE("single send observes authorization loss without a response under unlimited wait",
          "[daemon][send][arbitration][auth][unlimited]") {
    SendHarness harness(true);
    auto outcome = harness.execute();
    const auto sent = harness.await_send();
    harness.runtime().push_update(harness.first(), {},
                                  tgcli::core::AuthStateData{tgcli::core::AuthState::LoggingOut});
    REQUIRE(eventually([&] { return harness.client().auth_state()->auth_sequence == 2; }));
    const auto status = outcome.wait_for(100ms);
    CHECK(status == std::future_status::ready);
    if (status != std::future_status::ready) {
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(stable_message()));
    }
    auto result = outcome.get();
    const auto* lost = std::get_if<tgcli::daemon::SingleSendAuthorizationLost>(&result);
    REQUIRE(lost != nullptr);
    CHECK_FALSE(lost->temporary);
    CHECK(lost->state == tgcli::core::AuthState::LoggingOut);
}

TEST_CASE("single send refreshes committed auth while callback publication is blocked",
          "[daemon][send][arbitration][auth][publication-race]") {
    SendHarness harness(true);
    std::mutex mutex;
    std::condition_variable condition;
    bool callback_entered = false;
    bool callback_release = false;
    bool callback_finished = false;
    const auto blocker = harness.client().subscribe_auth_states(
        [&](const std::shared_ptr<const tgcli::core::AuthStateSnapshot>& snapshot) {
            if (!snapshot || snapshot->auth_sequence != 2) {
                return;
            }
            std::unique_lock lock(mutex);
            callback_entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return callback_release; });
            callback_finished = true;
            condition.notify_all();
        });
    tgcli::daemon::SingleSendHooks hooks;
    hooks.wait = [](const tgcli::RequestDeadline&, const std::stop_token&) {
        std::this_thread::yield();
    };
    auto outcome = harness.execute(std::move(hooks));
    const auto sent = harness.await_send();
    harness.runtime().push_update(harness.first(), {},
                                  tgcli::core::AuthStateData{tgcli::core::AuthState::LoggingOut});
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&] { return callback_entered; }));
    }
    const auto status = outcome.wait_for(100ms);
    CHECK(status == std::future_status::ready);
    {
        const std::lock_guard lock(mutex);
        callback_release = true;
    }
    condition.notify_all();
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&] { return callback_finished; }));
    }
    if (status != std::future_status::ready) {
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(stable_message()));
    }
    auto result = outcome.get();
    CHECK(std::get_if<tgcli::daemon::SingleSendAuthorizationLost>(&result) != nullptr);
    harness.client().unsubscribe_auth_states(blocker);
}

TEST_CASE("single send gives earlier auth priority over a response observed at deadline equality",
          "[daemon][send][arbitration][auth][deadline]") {
    SendHarness harness;
    std::mutex mutex;
    std::condition_variable condition;
    bool arbitration_entered = false;
    bool arbitration_release = false;
    bool blocked_once = false;
    tgcli::daemon::SingleSendHooks hooks;
    hooks.before_event_arbitration = [&] {
        std::unique_lock lock(mutex);
        if (blocked_once) {
            return;
        }
        blocked_once = true;
        arbitration_entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return arbitration_release; });
    };
    auto outcome = harness.execute(std::move(hooks));
    const auto sent = harness.await_send();
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&] { return arbitration_entered; }));
    }
    harness.runtime().push_update(harness.first(), {},
                                  tgcli::core::AuthStateData{tgcli::core::AuthState::LoggingOut});
    REQUIRE(eventually([&] { return harness.client().auth_state()->auth_sequence == 2; }));
    bool response_completed = false;
    const auto completion = harness.client().subscribe_response_completions([&](std::uint64_t) {
        const std::lock_guard lock(mutex);
        response_completed = true;
        condition.notify_all();
    });
    harness.clock().set(harness.deadline());
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(stable_message()));
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&] { return response_completed; }));
        arbitration_release = true;
    }
    condition.notify_all();
    harness.client().unsubscribe_response_completions(completion);
    auto result = outcome.get();
    const auto* lost = std::get_if<tgcli::daemon::SingleSendAuthorizationLost>(&result);
    REQUIRE(lost != nullptr);
    CHECK(lost->state == tgcli::core::AuthState::LoggingOut);
}

TEST_CASE("single send fails closed on null unknown and malformed matching variants",
          "[daemon][send][arbitration][malformed]") {
    SECTION("null immediate response") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id);
        auto result = outcome.get();
        CHECK(std::get_if<tgcli::daemon::SingleSendMalformed>(&result) != nullptr);
    }

    SECTION("unknown immediate sending state") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        auto message = pending_message();
        message.sending_state.kind = tgcli::core::TdMessageSendingStateKind::Unknown;
        message.sending_state.unsupported_tdlib_type_id = 700'000'101;
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(std::move(message)));
        auto result = outcome.get();
        const auto* malformed = std::get_if<tgcli::daemon::SingleSendMalformed>(&result);
        REQUIRE(malformed != nullptr);
        CHECK(malformed->tdlib_type_id == 700'000'101);
    }

    SECTION("wrong pending sending id") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message(-77, 9)));
        auto result = outcome.get();
        CHECK(std::get_if<tgcli::daemon::SingleSendMalformed>(&result) != nullptr);
    }

    SECTION("null matching success Message") {
        SendHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(pending_message()));
        REQUIRE(harness.runtime().wait_for_received(2));
        harness.runtime().push_message_send_succeeded(harness.first(), -77, std::nullopt);
        auto result = outcome.get();
        const auto* malformed = std::get_if<tgcli::daemon::SingleSendMalformed>(&result);
        REQUIRE(malformed != nullptr);
        REQUIRE(malformed->temporary);
    }
}
