#include "daemon/direct_rpc.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
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
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace {

std::optional<tgcli::secure::SensitiveString>
invite_owner(std::string_view value, tgcli::secure::WipeObserver observer = {}) {
    return std::optional<tgcli::secure::SensitiveString>{std::in_place, value, std::move(observer),
                                                         "td_join_invite"};
}

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
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    bool await_entry() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [this] { return entered_; });
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

class DirectHarness {
  public:
    using Clock = tgcli::core::TdEventClock;

    DirectHarness()
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
        request.command = {"msg", "edit"};
        request.context.timeout_seconds = 10.0;
        request.context.cwd = "/";
        session_ = std::make_unique<tgcli::daemon::RequestSession>(
            std::move(request), *sink_, 0, tgcli::daemon::RequestSession::NonceGenerator{},
            tgcli::daemon::ActivityTracker::Token{}, nullptr, tgcli::RequestDeadline{deadline_});
    }

    ~DirectHarness() {
        coordinator_.reset();
        if (!client_) {
            return;
        }
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
    DirectHarness(const DirectHarness&) = delete;
    DirectHarness& operator=(const DirectHarness&) = delete;
    DirectHarness(DirectHarness&&) = delete;
    DirectHarness& operator=(DirectHarness&&) = delete;

    std::future<tgcli::daemon::DirectOutcome> execute(tgcli::core::TdDirectRequest request,
                                                      tgcli::daemon::DirectRpcHooks hooks = {}) {
        if (!hooks.now) {
            hooks.now = [this] { return clock_.now(); };
        }
        coordinator_ = std::make_unique<tgcli::daemon::DirectRpcCoordinator>(*client_, *session_,
                                                                             std::move(hooks));
        const auto authorization = client_->auth_state();
        return std::async(std::launch::async, [this, request = std::move(request), authorization] {
            return coordinator_->execute(request, authorization);
        });
    }

    tgcli::test::SentTdFunction await_direct_send() {
        REQUIRE(runtime_->wait_for_sent(2));
        return runtime_->sent_functions().back();
    }

    static tgcli::core::TdMessageWriteResult write_message(std::int64_t id = 77) {
        return {.id = id,
                .chat_id = -1001,
                .date = 1'785'924'000,
                .sender = {.kind = tgcli::core::TdMessageSenderKind::User,
                           .id = 42,
                           .tdlib_type_id = 1},
                .is_outgoing = true,
                .topic = std::nullopt,
                .content_kind = tgcli::core::TdMessageContentKind::Text,
                .text = "edited",
                .scheduled = false};
    }

    ManualClock& clock() {
        return clock_;
    }

    Clock::time_point deadline() const {
        return deadline_;
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

  private:
    Clock::time_point deadline_;
    ManualClock clock_;
    std::unique_ptr<tgcli::test::ScriptedTdRuntime> runtime_owner_;
    tgcli::test::ScriptedTdRuntime* runtime_;
    std::unique_ptr<tgcli::core::TdClient> client_;
    tgcli::test::ScriptedClient first_{};
    std::unique_ptr<tgcli::daemon::CallbackSink> sink_;
    std::unique_ptr<tgcli::daemon::RequestSession> session_;
    std::unique_ptr<tgcli::daemon::DirectRpcCoordinator> coordinator_;
};

tgcli::core::TdDirectRequest edit_request() {
    return tgcli::core::TdEditMessageTextRequest{
        .chat_id = -1001, .message_id = 10, .text = "edited"};
}

const tgcli::daemon::DirectSuccess& require_success(tgcli::daemon::DirectOutcome& outcome) {
    const auto* success = std::get_if<tgcli::daemon::DirectSuccess>(&outcome);
    REQUIRE(success != nullptr);
    return *success;
}

std::size_t sent_count(const tgcli::test::ScriptedTdRuntime& runtime,
                       tgcli::core::TdFunctionKind function) {
    const auto sent = runtime.sent_functions();
    return static_cast<std::size_t>(std::ranges::count_if(
        sent, [&](const auto& item) { return item.function.kind() == function; }));
}

} // namespace

TEST_CASE("TdClient exposes exact typed direct-planning reads",
          "[core][direct][planning][fake-boundary]") {
    DirectHarness harness;
    const auto authorization = harness.client().auth_state();
    const auto check = [&](std::future<tgcli::core::TdValue> response,
                           tgcli::core::TdFunctionKind expected, std::size_t sent_count) {
        REQUIRE(harness.runtime().wait_for_sent(sent_count));
        const auto sent = harness.runtime().sent_functions().back();
        CHECK(sent.function.kind() == expected);
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(tgcli::core::TdOk{}));
        REQUIRE(response.wait_for(2s) == std::future_status::ready);
        CHECK(response.get().get_if<tgcli::core::TdOk>() != nullptr);
    };
    check(harness.client().get_message(authorization, -1001, 10),
          tgcli::core::TdFunctionKind::GetMessage, 2);
    check(harness.client().get_message_properties(authorization, -1001, 10),
          tgcli::core::TdFunctionKind::GetMessageProperties, 3);
    check(harness.client().get_message_available_reactions(authorization, -1001, 10),
          tgcli::core::TdFunctionKind::GetMessageAvailableReactions, 4);
    check(harness.client().get_unix_time(authorization), tgcli::core::TdFunctionKind::GetOption, 5);
    check(harness.client().parse_text_entities(authorization, "**bold**",
                                               tgcli::core::TdTextParseMode::MarkdownV2),
          tgcli::core::TdFunctionKind::ParseTextEntities, 6);
}

TEST_CASE("direct RPC maps all typed requests to bounded results",
          "[daemon][direct][fake-boundary]") {
    const std::vector<tgcli::core::TdDirectRequest> requests{
        tgcli::core::TdEditMessageTextRequest{.chat_id = -1001, .message_id = 10, .text = "edited"},
        tgcli::core::TdDeleteMessagesRequest{
            .chat_id = -1001, .message_ids = {10, 11}, .revoke = true},
        tgcli::core::TdMessageReactionRequest{
            .chat_id = -1001, .message_id = 10, .reaction = "👍", .remove = false, .big = true},
        tgcli::core::TdMessageReactionRequest{
            .chat_id = -1001, .message_id = 10, .reaction = "👍", .remove = true, .big = false},
        tgcli::core::TdPinMessageRequest{.chat_id = -1001, .message_id = 10, .pinned = true},
        tgcli::core::TdPinMessageRequest{.chat_id = -1001, .message_id = 10, .pinned = false},
        tgcli::core::TdViewMessagesRequest{.chat_id = -1001, .message_ids = {10}},
        tgcli::core::TdSetChatNotificationSettingsRequest{
            .chat_id = -1001, .settings = {.use_default_mute_for = false, .mute_for = 3600}},
        tgcli::core::TdSetChatNotificationSettingsRequest{
            .chat_id = -1001, .settings = {.use_default_mute_for = false, .mute_for = 0}},
        tgcli::core::TdToggleChatIsPinnedRequest{
            .chat_id = -1001, .list = tgcli::core::TdDirectChatList::Main, .pinned = true},
        tgcli::core::TdToggleChatIsPinnedRequest{
            .chat_id = -1001, .list = tgcli::core::TdDirectChatList::Archive, .pinned = false},
        tgcli::core::TdAddChatToListRequest{.chat_id = -1001,
                                            .list = tgcli::core::TdDirectChatList::Archive},
        tgcli::core::TdAddChatToListRequest{.chat_id = -1001,
                                            .list = tgcli::core::TdDirectChatList::Main},
        tgcli::core::TdJoinChatRequest{-1001, std::nullopt, std::nullopt},
        tgcli::core::TdJoinChatRequest{std::nullopt, invite_owner("https://t.me/+secret"),
                                       std::nullopt},
        tgcli::core::TdLeaveChatRequest{.chat_id = -1001},
    };
    for (const auto& request : requests) {
        DirectHarness harness;
        auto pending = harness.execute(request);
        const auto sent = harness.await_direct_send();
        if (std::holds_alternative<tgcli::core::TdEditMessageTextRequest>(request)) {
            harness.runtime().push_response(
                harness.first(), sent.query_id,
                tgcli::core::TdValue::from(DirectHarness::write_message()));
        } else if (std::holds_alternative<tgcli::core::TdJoinChatRequest>(request)) {
            harness.runtime().push_response(
                harness.first(), sent.query_id,
                tgcli::core::TdValue::from(tgcli::core::TdChatJoinResult{
                    .kind = tgcli::core::TdChatJoinResultKind::Success,
                    .chat_id = -1001,
                    .guard_bot_user_id = std::nullopt,
                    .guard_query_id = std::nullopt,
                    .unsupported_tdlib_type_id = std::nullopt}));
        } else {
            harness.runtime().push_response(harness.first(), sent.query_id,
                                            tgcli::core::TdValue::from(tgcli::core::TdOk{}));
        }
        auto outcome = pending.get();
        CHECK(require_success(outcome).mutation_state ==
              tgcli::daemon::DirectMutationState::Confirmed);
    }
}

TEST_CASE("direct RPC chooses response or first non-Ready by receive order",
          "[daemon][direct][arbitration][fake-boundary]") {
    SECTION("response first") {
        DirectHarness harness;
        harness.runtime().set_receive_paused(true);
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(DirectHarness::write_message()));
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::WaitCode});
        harness.runtime().set_receive_paused(false);
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectSuccess>(outcome));
    }

    SECTION("authorization first") {
        DirectHarness harness;
        harness.runtime().set_receive_paused(true);
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::WaitCode});
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(DirectHarness::write_message()));
        harness.runtime().set_receive_paused(false);
        auto outcome = pending.get();
        const auto* lost = std::get_if<tgcli::daemon::DirectAuthorizationLost>(&outcome);
        REQUIRE(lost != nullptr);
        REQUIRE(lost->snapshot != nullptr);
        CHECK(lost->snapshot->data.state == tgcli::core::AuthState::WaitCode);
    }

    SECTION("same Ready is non-terminal") {
        DirectHarness harness;
        harness.runtime().set_receive_paused(true);
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(DirectHarness::write_message()));
        harness.runtime().set_receive_paused(false);
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectSuccess>(outcome));
        CHECK(harness.runtime().sent_functions().size() == 2);
    }

    SECTION("old generation and late response are cleaned without retry") {
        DirectHarness harness;
        harness.runtime().set_receive_paused(true);
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::Closed});
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(DirectHarness::write_message()));
        harness.runtime().set_receive_paused(false);
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectAuthorizationLost>(outcome));
        REQUIRE(harness.runtime().wait_for_received(3));
        REQUIRE(harness.runtime().wait_for_clients(2));
        CHECK(harness.runtime().sent_functions().size() == 3);
    }
}

TEST_CASE("direct RPC gives deadline equality to timeout", "[daemon][direct][arbitration]") {
    DirectHarness harness;
    PollBarrier barrier;
    auto pending = harness.execute(
        edit_request(),
        tgcli::daemon::DirectRpcHooks{.now = [&] { return harness.clock().now(); },
                                      .wait = {},
                                      .before_request = {},
                                      .before_submit = {},
                                      .before_event_arbitration = [&] { barrier.wait(); },
                                      .before_wait = {}});
    const auto sent = harness.await_direct_send();
    REQUIRE(barrier.await_entry());
    harness.clock().set(harness.deadline());
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(DirectHarness::write_message()));
    REQUIRE(harness.runtime().wait_for_received(2));
    barrier.release();
    auto outcome = pending.get();
    CHECK(std::holds_alternative<tgcli::daemon::DirectTimedOut>(outcome));
}

TEST_CASE("direct RPC maps TD errors malformed values cancellation and result bounds",
          "[daemon][direct][arbitration][fake-boundary]") {
    SECTION("TD error") {
        DirectHarness harness;
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        harness.runtime().push_response(
            harness.first(), sent.query_id,
            tgcli::core::TdValue::from(tgcli::core::TdError{429, "retry after 1.25"}));
        auto outcome = pending.get();
        const auto* error = std::get_if<tgcli::daemon::DirectTdError>(&outcome);
        REQUIRE(error != nullptr);
        CHECK(error->error.code == 429);
        CHECK(error->mutation_state == tgcli::daemon::DirectMutationState::Possible);
    }

    SECTION("null response") {
        DirectHarness harness;
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        harness.runtime().push_response(harness.first(), sent.query_id);
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectMalformed>(outcome));
    }

    SECTION("out of int53 result") {
        DirectHarness harness;
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        harness.runtime().push_response(
            harness.first(), sent.query_id,
            tgcli::core::TdValue::from(DirectHarness::write_message(tgcli::core::kTdInt53Max + 1)));
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectMalformed>(outcome));
    }

    SECTION("out of persistence text bounds") {
        DirectHarness harness;
        auto pending = harness.execute(edit_request());
        const auto sent = harness.await_direct_send();
        auto result = DirectHarness::write_message();
        result.text = std::string(4'097, 'x');
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(std::move(result)));
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectMalformed>(outcome));
    }

    SECTION("cancellation") {
        DirectHarness harness;
        auto pending = harness.execute(edit_request());
        static_cast<void>(harness.await_direct_send());
        harness.session().disconnect();
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectCancelled>(outcome));
    }
}

TEST_CASE("direct RPC never retries a pre-boundary authorization rejection",
          "[daemon][direct][authorization][fake-boundary]") {
    DirectHarness harness;
    PollBarrier barrier;
    harness.runtime().set_before_make([&](tgcli::core::TdFunctionKind function) {
        if (function == tgcli::core::TdFunctionKind::EditMessageText) {
            barrier.wait();
        }
    });
    auto pending = harness.execute(edit_request());
    REQUIRE(barrier.await_entry());
    harness.runtime().push_update(harness.first(), {},
                                  tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
    REQUIRE(eventually([&] { return harness.runtime().received_count() >= 2; }));
    barrier.release();
    auto outcome = pending.get();
    const auto* rejected = std::get_if<tgcli::daemon::DirectRejected>(&outcome);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->authorization_failure ==
          tgcli::core::TdAuthorizationFailure::AuthSequenceMismatch);
    CHECK(rejected->mutation_state == tgcli::daemon::DirectMutationState::None);
    CHECK(harness.runtime().sent_functions().size() == 1);
}

TEST_CASE("direct RPC keeps pre-boundary authorization loss mutation-free",
          "[daemon][direct][authorization][fake-boundary]") {
    SECTION("non-Ready before send") {
        DirectHarness harness;
        PollBarrier barrier;
        harness.runtime().set_before_make([&](tgcli::core::TdFunctionKind function) {
            if (function == tgcli::core::TdFunctionKind::EditMessageText) {
                barrier.wait();
            }
        });
        auto pending = harness.execute(edit_request());
        REQUIRE(barrier.await_entry());
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::WaitCode});
        REQUIRE(eventually([&] {
            return harness.client().auth_state()->data.state == tgcli::core::AuthState::WaitCode;
        }));
        barrier.release();
        auto outcome = pending.get();
        const auto* lost = std::get_if<tgcli::daemon::DirectAuthorizationLost>(&outcome);
        REQUIRE(lost != nullptr);
        CHECK(lost->mutation_state == tgcli::daemon::DirectMutationState::None);
        CHECK(sent_count(harness.runtime(), tgcli::core::TdFunctionKind::EditMessageText) == 0);
    }

    SECTION("replacement bootstrap snapshot is not a malformed receive event") {
        DirectHarness harness;
        PollBarrier barrier;
        harness.runtime().set_before_make([&](tgcli::core::TdFunctionKind function) {
            if (function == tgcli::core::TdFunctionKind::EditMessageText) {
                barrier.wait();
            }
        });
        auto pending = harness.execute(edit_request());
        REQUIRE(barrier.await_entry());
        harness.runtime().push_update(harness.first(), {},
                                      tgcli::core::AuthStateData{tgcli::core::AuthState::Closed});
        REQUIRE(harness.runtime().wait_for_clients(2));
        REQUIRE(eventually([&] {
            return harness.client().auth_state()->client_generation >
                   harness.first().client_generation;
        }));
        barrier.release();
        auto outcome = pending.get();
        const auto* lost = std::get_if<tgcli::daemon::DirectAuthorizationLost>(&outcome);
        REQUIRE(lost != nullptr);
        REQUIRE(lost->snapshot != nullptr);
        CHECK(lost->snapshot->data.state == tgcli::core::AuthState::Closed);
        CHECK(lost->mutation_state == tgcli::daemon::DirectMutationState::None);
        CHECK(sent_count(harness.runtime(), tgcli::core::TdFunctionKind::EditMessageText) == 0);
    }
}

TEST_CASE("direct join guard and decline are explicit mutation-none outcomes",
          "[daemon][direct][join][fake-boundary]") {
    const auto request = tgcli::core::TdJoinChatRequest{
        std::nullopt, invite_owner("https://t.me/+secret"), std::nullopt};
    SECTION("guard") {
        DirectHarness harness;
        auto pending = harness.execute(request);
        const auto sent = harness.await_direct_send();
        harness.runtime().push_response(
            harness.first(), sent.query_id,
            tgcli::core::TdValue::from(tgcli::core::TdChatJoinResult{
                .kind = tgcli::core::TdChatJoinResultKind::GuardBotApprovalRequired,
                .chat_id = std::nullopt,
                .guard_bot_user_id = 42,
                .guard_query_id = 73,
                .unsupported_tdlib_type_id = std::nullopt}));
        auto outcome = pending.get();
        const auto* guard = std::get_if<tgcli::daemon::DirectJoinGuardRequired>(&outcome);
        REQUIRE(guard != nullptr);
        CHECK(guard->bot_user_id == 42);
        CHECK(guard->query_id == 73);
        CHECK(guard->mutation_state == tgcli::daemon::DirectMutationState::None);
    }

    SECTION("declined") {
        DirectHarness harness;
        auto pending = harness.execute(request);
        const auto sent = harness.await_direct_send();
        harness.runtime().push_response(harness.first(), sent.query_id,
                                        tgcli::core::TdValue::from(tgcli::core::TdChatJoinResult{
                                            .kind = tgcli::core::TdChatJoinResultKind::Declined,
                                            .chat_id = std::nullopt,
                                            .guard_bot_user_id = std::nullopt,
                                            .guard_query_id = std::nullopt,
                                            .unsupported_tdlib_type_id = std::nullopt}));
        auto outcome = pending.get();
        CHECK(std::holds_alternative<tgcli::daemon::DirectJoinDeclined>(outcome));
    }
}

TEST_CASE("invite request-sent preserves correlation metadata without changing the TD function",
          "[daemon][direct][join][fake-boundary]") {
    DirectHarness harness;
    const auto request = tgcli::core::TdJoinChatRequest{
        std::nullopt, invite_owner("https://t.me/+known-secret"), -1001};
    auto pending = harness.execute(request);
    const auto sent = harness.await_direct_send();
    CHECK(sent.function.kind() == tgcli::core::TdFunctionKind::JoinChatByInviteLink);
    harness.runtime().push_response(harness.first(), sent.query_id,
                                    tgcli::core::TdValue::from(tgcli::core::TdChatJoinResult{
                                        .kind = tgcli::core::TdChatJoinResultKind::RequestSent,
                                        .chat_id = std::nullopt,
                                        .guard_bot_user_id = std::nullopt,
                                        .guard_query_id = std::nullopt,
                                        .unsupported_tdlib_type_id = std::nullopt}));
    auto outcome = pending.get();
    const auto& success = require_success(outcome);
    const auto* result = std::get_if<tgcli::daemon::DirectJoinResult>(&success.result);
    REQUIRE(result != nullptr);
    CHECK(result->status == tgcli::daemon::DirectJoinStatus::RequestSent);
    CHECK(result->chat_id == -1001);
}
