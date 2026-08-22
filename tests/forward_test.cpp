#include "daemon/forward.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace {

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
        .chat_id = -1002,
        .date = 1'785'924'000,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = true,
        .topic = std::nullopt,
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "forwarded"};
}

tgcli::core::TdWriteMessage pending(std::int64_t id) {
    return {.message = summary(id),
            .sending_state = {.kind = tgcli::core::TdMessageSendingStateKind::Pending,
                              .sending_id = 24680,
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

tgcli::core::TdWriteMessage stable(std::int64_t id) {
    return {.message = summary(id),
            .sending_state = {},
            .scheduling_state = {},
            .has_reply_markup = false};
}

tgcli::core::TdWriteMessage failed(std::int64_t id, std::int32_t code, double retry_after = 0) {
    tgcli::core::TdError error{code, "failed"};
    return {.message = summary(id),
            .sending_state = {.kind = tgcli::core::TdMessageSendingStateKind::Failed,
                              .sending_id = 0,
                              .error = error,
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

tgcli::core::TdForwardMessagesRequest request() {
    return {.from_chat_id = -1001,
            .to_chat_id = -1002,
            .message_ids = {1, 2},
            .sending_id = 24680,
            .drop_author = false};
}

class ForwardHarness {
  public:
    ForwardHarness()
        : runtime_owner_(std::make_unique<tgcli::test::ScriptedTdRuntime>()),
          runtime_(runtime_owner_.get()),
          client_(std::make_unique<tgcli::core::TdClient>(std::move(runtime_owner_))) {
        REQUIRE(runtime_->wait_for_sent(1));
        first_ = runtime_->clients().front();
        runtime_->push_response(first_, 1, {},
                                tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        REQUIRE(eventually([this] { return client_->auth_state()->auth_sequence == 1; }));
        sink_ = std::make_unique<tgcli::daemon::CallbackSink>(
            [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
            [](const nlohmann::json&) {},
            [](const std::string&, const std::string&, const nlohmann::json&, int) {});
        tgcli::proto::Request frame("main");
        frame.id = 9;
        frame.command = {"msg", "forward"};
        frame.context.timeout_seconds = 10.0;
        frame.context.cwd = "/";
        session_ = std::make_unique<tgcli::daemon::RequestSession>(
            std::move(frame), *sink_, 0, tgcli::daemon::RequestSession::NonceGenerator{},
            tgcli::daemon::ActivityTracker::Token{}, nullptr,
            tgcli::RequestDeadline{std::chrono::steady_clock::now() + 10s});
    }

    ~ForwardHarness() {
        coordinator_.reset();
        client_->close();
    }

    std::future<tgcli::daemon::ForwardOutcome> execute(tgcli::daemon::ForwardHooks hooks = {}) {
        coordinator_ = std::make_unique<tgcli::daemon::ForwardCoordinator>(*client_, *session_,
                                                                           std::move(hooks));
        const auto authorization = client_->auth_state();
        return std::async(std::launch::async, [this, authorization] {
            return coordinator_->execute(request(), authorization);
        });
    }

    tgcli::test::SentTdFunction await_forward() {
        REQUIRE(runtime_->wait_for_sent(2));
        const auto sent = runtime_->sent_functions().back();
        REQUIRE(sent.function.kind() == tgcli::core::TdFunctionKind::ForwardMessages);
        return sent;
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

  private:
    std::unique_ptr<tgcli::test::ScriptedTdRuntime> runtime_owner_;
    tgcli::test::ScriptedTdRuntime* runtime_;
    std::unique_ptr<tgcli::core::TdClient> client_;
    tgcli::test::ScriptedClient first_{};
    std::unique_ptr<tgcli::daemon::CallbackSink> sink_;
    std::unique_ptr<tgcli::daemon::RequestSession> session_;
    std::unique_ptr<tgcli::daemon::ForwardCoordinator> coordinator_;
};

} // namespace

TEST_CASE("forward coordinator durably observes ordered full vectors",
          "[daemon][forward][arbitration][progress]") {
    ForwardHarness harness;
    std::mutex mutex;
    std::vector<std::vector<tgcli::daemon::ForwardItem>> progress;
    std::vector<std::int64_t> temporary_ids;
    tgcli::daemon::ForwardHooks hooks;
    hooks.on_temporary_ids = [&](const auto& ids) { temporary_ids = ids; };
    hooks.on_progress = [&](const auto& items) {
        const std::lock_guard lock(mutex);
        progress.push_back(items);
    };
    auto outcome = harness.execute(std::move(hooks));
    const auto sent = harness.await_forward();
    tgcli::core::TdForwardMessages immediate;
    immediate.messages.emplace_back(pending(-77));
    immediate.messages.emplace_back(std::nullopt);
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(std::move(immediate)));
    REQUIRE(eventually([&] {
        const std::lock_guard lock(mutex);
        return progress.size() == 1;
    }));
    harness.runtime().push_message_send_succeeded(harness.first(), -77, stable(101));

    auto result = outcome.get();
    const auto* completed = std::get_if<tgcli::daemon::ForwardCompleted>(&result);
    REQUIRE(completed != nullptr);
    CHECK(completed->mutation_state == tgcli::daemon::ForwardMutationState::Confirmed);
    REQUIRE(completed->items.size() == 2);
    CHECK(std::holds_alternative<tgcli::daemon::ForwardSent>(completed->items[0]));
    CHECK(std::holds_alternative<tgcli::daemon::ForwardFailed>(completed->items[1]));
    CHECK(temporary_ids == std::vector<std::int64_t>{-77});
    const std::lock_guard lock(mutex);
    REQUIRE(progress.size() == 2);
    CHECK(std::holds_alternative<tgcli::daemon::ForwardPending>(progress[0][0]));
    CHECK(std::holds_alternative<tgcli::daemon::ForwardSent>(progress[1][0]));
}

TEST_CASE("forward coordinator rejects a malformed immediate vector after dispatch",
          "[daemon][forward][arbitration][malformed]") {
    ForwardHarness harness;
    auto outcome = harness.execute();
    const auto sent = harness.await_forward();
    tgcli::core::TdForwardMessages immediate;
    immediate.messages.emplace_back(stable(101));
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(std::move(immediate)));
    const auto result = outcome.get();
    const auto* malformed = std::get_if<tgcli::daemon::ForwardMalformed>(&result);
    REQUIRE(malformed != nullptr);
    CHECK(malformed->mutation_state == tgcli::daemon::ForwardMutationState::Possible);
    CHECK(malformed->items.empty());
}

TEST_CASE("forward coordinator retains exact explicit failures and maximum retry",
          "[daemon][forward][arbitration][failure]") {
    ForwardHarness harness;
    auto outcome = harness.execute();
    const auto sent = harness.await_forward();
    tgcli::core::TdForwardMessages immediate;
    immediate.messages.emplace_back(failed(-70, 429, 1.25));
    immediate.messages.emplace_back(failed(-71, 429, 3.01));
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(std::move(immediate)));

    const auto result = outcome.get();
    const auto* completed = std::get_if<tgcli::daemon::ForwardCompleted>(&result);
    REQUIRE(completed != nullptr);
    CHECK(completed->mutation_state == tgcli::daemon::ForwardMutationState::None);
    const auto& first = std::get<tgcli::daemon::ForwardFailed>(completed->items[0]);
    const auto& second = std::get<tgcli::daemon::ForwardFailed>(completed->items[1]);
    CHECK(first.tdlib_code == 429);
    CHECK(first.retry_after == 2);
    CHECK(second.retry_after == 4);
}

TEST_CASE("forward coordinator keeps pending state on post-dispatch auth loss and cancellation",
          "[daemon][forward][arbitration][unknown]") {
    SECTION("authorization loss") {
        ForwardHarness harness;
        auto outcome = harness.execute();
        const auto sent = harness.await_forward();
        tgcli::core::TdForwardMessages immediate;
        immediate.messages.emplace_back(pending(-77));
        immediate.messages.emplace_back(std::nullopt);
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(std::move(immediate)));
        REQUIRE(harness.runtime().wait_for_received(2));
        harness.runtime().push_update(
            harness.first(), {}, tgcli::core::AuthStateData{tgcli::core::AuthState::LoggingOut});
        const auto result = outcome.get();
        const auto* lost = std::get_if<tgcli::daemon::ForwardAuthorizationLost>(&result);
        REQUIRE(lost != nullptr);
        REQUIRE(lost->items.size() == 2);
        CHECK(lost->mutation_state == tgcli::daemon::ForwardMutationState::Possible);
    }

    SECTION("cancellation") {
        ForwardHarness harness;
        std::promise<void> observed;
        auto observed_future = observed.get_future();
        tgcli::daemon::ForwardHooks hooks;
        hooks.on_progress = [&](const auto&) { observed.set_value(); };
        auto outcome = harness.execute(std::move(hooks));
        const auto sent = harness.await_forward();
        tgcli::core::TdForwardMessages immediate;
        immediate.messages.emplace_back(pending(-77));
        immediate.messages.emplace_back(std::nullopt);
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(std::move(immediate)));
        REQUIRE(observed_future.wait_for(2s) == std::future_status::ready);
        harness.session().disconnect();
        const auto result = outcome.get();
        const auto* cancelled = std::get_if<tgcli::daemon::ForwardCancelled>(&result);
        REQUIRE(cancelled != nullptr);
        REQUIRE(cancelled->items.size() == 2);
        CHECK(cancelled->mutation_state == tgcli::daemon::ForwardMutationState::Possible);
    }
}
