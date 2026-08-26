#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "daemon/stream_coordinator.hpp"
#include "daemon/stream_service.hpp"
#include "support/scripted_td_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::daemon;
using namespace std::chrono_literals;
using nlohmann::json;

static_assert(std::is_constructible_v<StreamCoordinator, tgcli::core::TdClient&, StreamService&,
                                      std::string, StreamActivityMode>);

TEST_CASE("stream commands register with unlimited default deadlines", "[stream][coordinator]") {
    CHECK(m2_operation_name(M2Operation::Listen) == "listen");
    CHECK(m2_operation_name(M2Operation::WaitFor) == "wait_for");
}

namespace {

template <typename Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

tgcli::core::TdUserSummary user(bool bot = false) {
    return {.id = 42,
            .first_name = bot ? "Bot" : "Ada",
            .last_name = "",
            .usernames = {bot ? "build_bot" : "ada"},
            .phone_number = bot ? "" : "12025550123",
            .is_bot = bot,
            .is_premium = false,
            .presence = tgcli::core::TdUserPresence::Online};
}

tgcli::core::TdChat chat() {
    return {.id = -1001,
            .title = "Project",
            .kind = tgcli::core::TdChatKind::Private,
            .related_id = 42,
            .tdlib_type_id = 1,
            .positions = {},
            .chat_lists = {{.kind = tgcli::core::TdChatListKind::Main, .folder_id = 0}},
            .is_marked_unread = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt,
            .notification_settings = std::nullopt};
}

tgcli::core::TdCurrentState current_state() {
    tgcli::core::TdCurrentState state;
    state.updates.push_back(tgcli::core::TdValue::from(tgcli::core::TdUpdateUser{.user = user()}));
    state.updates.push_back(
        tgcli::core::TdValue::from(tgcli::core::TdUpdateNewChat{.chat = chat()}));
    return state;
}

tgcli::core::TdMessageSummary message(std::int64_t id, std::string text) {
    return {
        .id = id,
        .chat_id = -1001,
        .date = 1'700'000'000,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = false,
        .topic = std::nullopt,
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = std::move(text)};
}

struct Outcome {
    std::vector<json> items;
    std::optional<json> result;
    std::optional<json> error;
};

class CoordinatorFixture {
  public:
    explicit CoordinatorFixture(testing::StreamCoordinatorProbe probe = {}) {
        auto runtime = std::make_unique<tgcli::test::ScriptedTdRuntime>();
        runtime_ = runtime.get();
        client_ = std::make_unique<tgcli::core::TdClient>(
            std::move(runtime), tgcli::core::TdLogConfiguration{},
            tgcli::core::TdClientEventHooks{}, service_.observer_factory());
        REQUIRE(runtime_->wait_for_sent(2));
        client_id_ = runtime_->clients().front();
        const auto sent = runtime_->sent_functions();
        for (const auto& call : sent) {
            if (call.function.kind() == tgcli::core::TdFunctionKind::GetCurrentState) {
                runtime_->push_response(client_id_, call.query_id,
                                        tgcli::core::TdValue::from(current_state()));
            } else {
                runtime_->push_response(client_id_, call.query_id, {},
                                        tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
            }
        }
        REQUIRE(eventually([&] {
            return client_->auth_state() &&
                   client_->auth_state()->data.state == tgcli::core::AuthState::Ready &&
                   service_.status().ready_for_admission();
        }));
        coordinator_ = std::make_unique<StreamCoordinator>(
            *client_, service_, "main", StreamActivityMode::UntrackedNoDaemon, probe);
        register_stream_commands(dispatcher_, *coordinator_);
    }

    ~CoordinatorFixture() {
        client_->close();
    }

    CoordinatorFixture(const CoordinatorFixture&) = delete;
    CoordinatorFixture& operator=(const CoordinatorFixture&) = delete;
    CoordinatorFixture(CoordinatorFixture&&) = delete;
    CoordinatorFixture& operator=(CoordinatorFixture&&) = delete;

    std::future<void> dispatch(RequestSession& session) {
        return std::async(std::launch::async, [&] { dispatcher_.dispatch(session); });
    }

    tgcli::core::TdFunctionData respond(tgcli::core::TdFunctionKind kind,
                                        tgcli::core::TdValue value) {
        REQUIRE(runtime_->wait_for_sent(sent_count_ + 1));
        const auto sent = runtime_->sent_functions();
        REQUIRE(sent.size() == sent_count_ + 1);
        REQUIRE(sent.back().function.kind() == kind);
        const auto descriptor = sent.back().function;
        runtime_->push_response(client_id_, sent.back().query_id, std::move(value));
        ++sent_count_;
        return descriptor;
    }

    void publish_boundary() {
        runtime_->push_response(client_id_, 999'999, {});
    }

    void publish_message(std::int64_t id, std::string text) {
        runtime_->push_update(client_id_,
                              tgcli::core::TdValue::from(tgcli::core::TdUpdateNewMessage{
                                  .message = message(id, std::move(text))}));
    }

    void replace_generation() {
        const auto previous_sent = runtime_->sent_functions().size();
        const auto previous = client_id_;
        runtime_->push_update(previous, {},
                              tgcli::core::AuthStateData{tgcli::core::AuthState::Closed});
        REQUIRE(runtime_->wait_for_clients(2));
        REQUIRE(runtime_->wait_for_sent(previous_sent + 2));
        client_id_ = runtime_->clients().back();
        const auto sent = runtime_->sent_functions();
        for (std::size_t index = previous_sent; index < sent.size(); ++index) {
            if (sent[index].function.kind() == tgcli::core::TdFunctionKind::GetCurrentState) {
                runtime_->push_response(client_id_, sent[index].query_id,
                                        tgcli::core::TdValue::from(current_state()));
            } else {
                REQUIRE(sent[index].function.kind() ==
                        tgcli::core::TdFunctionKind::GetAuthorizationState);
                runtime_->push_response(client_id_, sent[index].query_id, {},
                                        tgcli::core::AuthStateData{tgcli::core::AuthState::Ready});
            }
        }
        sent_count_ = previous_sent + 2;
        REQUIRE(eventually([&] {
            const auto snapshot = client_->auth_state();
            return snapshot && snapshot->client_generation == client_id_.client_generation &&
                   snapshot->data.state == tgcli::core::AuthState::Ready &&
                   service_.status().ready_for_admission();
        }));
    }

    tgcli::test::ScriptedTdRuntime& runtime() {
        return *runtime_;
    }

    std::size_t ingress_state_count(StreamIngressState state) noexcept {
        return StreamIngressTestAccess::state_count(service_.ingress_hub(), state);
    }

  private:
    StreamService service_;
    tgcli::test::ScriptedTdRuntime* runtime_ = nullptr;
    std::unique_ptr<tgcli::core::TdClient> client_;
    tgcli::test::ScriptedClient client_id_{};
    std::unique_ptr<StreamCoordinator> coordinator_;
    Dispatcher dispatcher_;
    std::size_t sent_count_ = 2;
};

struct ResolutionGate {
    testing::StreamCoordinatorProbePoint target =
        testing::StreamCoordinatorProbePoint::AfterResolve;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    static void notify(void* context, testing::StreamCoordinatorProbePoint point) noexcept {
        auto& gate = *static_cast<ResolutionGate*>(context);
        if (point != gate.target) {
            return;
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

std::shared_ptr<CallbackSink> sink(Outcome& outcome) {
    return std::make_shared<CallbackSink>(
        [&](json value) { outcome.items.push_back(std::move(value)); }, [](const json&) {},
        [&](json value) { outcome.result = std::move(value); },
        [&](std::string code, std::string message_value, json details, int) {
            outcome.error = json{{"code", std::move(code)},
                                 {"message", std::move(message_value)},
                                 {"details", std::move(details)}};
        });
}

tgcli::proto::Request listen_request() {
    tgcli::proto::Request value("main");
    value.id = 1;
    value.command = {"listen"};
    value.args = {
        {"chats", json::array({"-1001"})}, {"types", json::array({"message"})}, {"count", 1}};
    value.context.cwd = "/";
    return value;
}

tgcli::proto::Request wait_request() {
    tgcli::proto::Request value("main");
    value.id = 2;
    value.command = {"wait-for"};
    value.args = {{"chat", "-1001"}, {"from", "42"}, {"regex", "^target$"}, {"after", 150}};
    value.context.cwd = "/";
    return value;
}

} // namespace

TEST_CASE("listen coordinator resolves atomically then publishes and counts one item",
          "[stream][coordinator][fake-boundary]") {
    CoordinatorFixture fixture;
    Outcome outcome;
    RequestSession session(listen_request(), sink(outcome));
    auto running = fixture.dispatch(session);
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdValue::from(user()));
    fixture.respond(tgcli::core::TdFunctionKind::GetChat, tgcli::core::TdValue::from(chat()));
    fixture.respond(tgcli::core::TdFunctionKind::GetUser, tgcli::core::TdValue::from(user()));
    REQUIRE(eventually(
        [&] { return testing::RequestSessionTestAccess::has_stream_subscription(session); }));
    fixture.publish_boundary();
    REQUIRE(eventually([&] { return session.stream_activation_projection().has_value(); }));
    fixture.publish_message(200, "live");
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    running.get();
    REQUIRE(outcome.items.size() == 1);
    CHECK(outcome.items.front()["event"] == "message");
    REQUIRE(outcome.result);
    CHECK(outcome.result->empty());
    CHECK_FALSE(outcome.error);
}

TEST_CASE("wait-for coordinator publishes before exact local history and returns history match",
          "[stream][coordinator][wait-scanner][fake-boundary]") {
    CoordinatorFixture fixture;
    Outcome outcome;
    RequestSession session(wait_request(), sink(outcome));
    auto running = fixture.dispatch(session);
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdValue::from(user()));
    fixture.respond(tgcli::core::TdFunctionKind::GetChat, tgcli::core::TdValue::from(chat()));
    fixture.respond(tgcli::core::TdFunctionKind::GetUser, tgcli::core::TdValue::from(user()));
    fixture.respond(tgcli::core::TdFunctionKind::GetUser, tgcli::core::TdValue::from(user()));
    REQUIRE(eventually(
        [&] { return testing::RequestSessionTestAccess::has_stream_subscription(session); }));
    CHECK(std::ranges::none_of(fixture.runtime().sent_functions(), [](const auto& sent) {
        return sent.function.kind() == tgcli::core::TdFunctionKind::GetChatHistory;
    }));
    fixture.publish_boundary();
    REQUIRE(eventually([&] { return session.stream_activation_projection().has_value(); }));
    const auto history = fixture.respond(
        tgcli::core::TdFunctionKind::GetChatHistory,
        tgcli::core::TdValue::from(tgcli::core::TdMessages{
            .total_count = 2, .messages = {message(200, "target"), message(100, "old")}}));
    CHECK(history.fields().size() == 5);
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    running.get();
    CHECK(outcome.items.empty());
    REQUIRE(outcome.result);
    CHECK(outcome.result->at("id") == 200);
    CHECK(outcome.result->at("text") == "target");
    CHECK_FALSE(outcome.error);
}

TEST_CASE("wait-for bot after rejects before chat user or history resolution",
          "[stream][coordinator][bot][fake-boundary]") {
    CoordinatorFixture fixture;
    Outcome outcome;
    RequestSession session(wait_request(), sink(outcome));
    auto running = fixture.dispatch(session);
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdValue::from(user(true)));
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    running.get();
    CHECK_FALSE(testing::RequestSessionTestAccess::has_stream_subscription(session));
    REQUIRE(outcome.error);
    CHECK(outcome.error->at("code") == "BOT_UNSUPPORTED");
    CHECK(outcome.error->at("details") == json{{"operation", "wait_for"}});
    CHECK(fixture.runtime().sent_functions().size() == 3);
}

TEST_CASE("listen resolves every chat before activation and fails atomically",
          "[stream][coordinator][resolver][fake-boundary]") {
    CoordinatorFixture fixture;
    auto request = listen_request();
    request.args["chats"] = json::array({"-1001", "-1002"});
    Outcome outcome;
    RequestSession session(std::move(request), sink(outcome));
    auto running = fixture.dispatch(session);
    fixture.respond(tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdValue::from(user()));
    fixture.respond(tgcli::core::TdFunctionKind::GetChat, tgcli::core::TdValue::from(chat()));
    fixture.respond(tgcli::core::TdFunctionKind::GetUser, tgcli::core::TdValue::from(user()));
    fixture.respond(tgcli::core::TdFunctionKind::GetChat,
                    tgcli::core::TdValue::from(tgcli::core::TdError{404, "not found"}));
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    running.get();
    CHECK_FALSE(testing::RequestSessionTestAccess::has_stream_subscription(session));
    CHECK(outcome.items.empty());
    CHECK_FALSE(outcome.result);
    REQUIRE(outcome.error);
    CHECK(outcome.error->at("code") == "NOT_FOUND");
    CHECK(outcome.error->at("details") == json{{"selector", "-1002"}});
}

TEST_CASE("stream setup rejects generation replacement after the final resolver response",
          "[stream][coordinator][resolver][generation][fake-boundary]") {
    for (const auto point : {testing::StreamCoordinatorProbePoint::AfterResolve,
                             testing::StreamCoordinatorProbePoint::AfterAuthorizationLookup}) {
        for (const bool wait_for : {false, true}) {
            CAPTURE(point, wait_for);
            ResolutionGate gate;
            gate.target = point;
            CoordinatorFixture fixture({.context = &gate, .hook = &ResolutionGate::notify});
            Outcome outcome;
            RequestSession session(wait_for ? wait_request() : listen_request(), sink(outcome));
            auto running = fixture.dispatch(session);
            fixture.respond(tgcli::core::TdFunctionKind::GetMe, tgcli::core::TdValue::from(user()));
            fixture.respond(tgcli::core::TdFunctionKind::GetChat,
                            tgcli::core::TdValue::from(chat()));
            fixture.respond(tgcli::core::TdFunctionKind::GetUser,
                            tgcli::core::TdValue::from(user()));
            if (wait_for) {
                fixture.respond(tgcli::core::TdFunctionKind::GetUser,
                                tgcli::core::TdValue::from(user()));
            }
            gate.wait();
            fixture.replace_generation();
            gate.release();

            REQUIRE(eventually([&] {
                return running.wait_for(0ms) == std::future_status::ready ||
                       testing::RequestSessionTestAccess::has_stream_subscription(session);
            }));
            if (testing::RequestSessionTestAccess::has_stream_subscription(session)) {
                fixture.publish_boundary();
                if (wait_for) {
                    fixture.respond(
                        tgcli::core::TdFunctionKind::GetChatHistory,
                        tgcli::core::TdValue::from(tgcli::core::TdMessages{
                            .total_count = 2,
                            .messages = {message(200, "target"), message(100, "old")}}));
                } else {
                    fixture.publish_message(200, "live");
                }
            }
            REQUIRE(running.wait_for(2s) == std::future_status::ready);
            running.get();
            CHECK_FALSE(testing::RequestSessionTestAccess::has_stream_subscription(session));
            CHECK(outcome.items.empty());
            CHECK_FALSE(outcome.result);
            REQUIRE(outcome.error);
            CHECK(outcome.error->at("code") == "NOT_AUTHED");
            CHECK(outcome.error->at("details") ==
                  json{{"account", "main"}, {"state", "closed"}, {"reason", "authorization_lost"}});
            CHECK(fixture.ingress_state_count(StreamIngressState::Armed) == 0);
            CHECK(fixture.ingress_state_count(StreamIngressState::Published) == 0);
            CHECK(std::ranges::none_of(fixture.runtime().sent_functions(), [](const auto& sent) {
                return sent.function.kind() == tgcli::core::TdFunctionKind::GetChatHistory;
            }));
        }
    }
}
