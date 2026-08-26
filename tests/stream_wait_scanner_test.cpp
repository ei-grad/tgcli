#include "daemon/request_session.hpp"
#include "daemon/stream_wait_scanner.hpp"

#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::daemon;

namespace {

tgcli::proto::Request request() {
    tgcli::proto::Request value("main");
    value.id = 1;
    value.command = {"wait-for"};
    value.context.cwd = "/";
    return value;
}

std::future<tgcli::core::TdValue> ready(tgcli::core::TdValue value) {
    std::promise<tgcli::core::TdValue> promise;
    promise.set_value(std::move(value));
    return promise.get_future();
}

tgcli::core::TdMessageSummary message(std::int64_t id, std::string text = "target") {
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

struct SessionFixture {
    std::shared_ptr<StreamIngressHub> hub = std::make_shared<StreamIngressHub>();
    CallbackSink sink{[](const nlohmann::json&) {}, [](const nlohmann::json&) {},
                      [](const nlohmann::json&) {},
                      [](const std::string&, const std::string&, const nlohmann::json&, int) {}};
    RequestSession session{request(), sink};

    explicit SessionFixture(bool publish = true) {
        hub->begin_generation(1001, 7);
        StreamIngressRequest ingress{.client_id = 1001,
                                     .generation = 7,
                                     .operation = StreamOperation::WaitFor,
                                     .mode = StreamMode::Match,
                                     .type_mask = stream_event_mask(StreamEventClass::Message)};
        ingress.chat_ids[0] = -1001;
        ingress.chat_count = 1;
        REQUIRE(std::holds_alternative<StreamSubscriptionActivated>(
            session.activate_stream_subscription(hub, ingress,
                                                 StreamActivityMode::UntrackedNoDaemon)));
        if (publish) {
            REQUIRE(hub->activate_armed(1001, 7, 20) == 1);
        }
    }
};

void publish_live(StreamIngressHub& hub, std::int64_t id, std::string text,
                  std::uint64_t sequence) {
    const auto materialized = materialize_message_summary(message(id, std::move(text)));
    REQUIRE(materialized);
    const auto line =
        nlohmann::json{{"event", "message"}, {"message", message_summary_json(*materialized)}}
            .dump() +
        "\n";
    auto sidecar = StreamRoutingSidecar{.event_class = StreamEventClass::Message,
                                        .chat_id = -1001,
                                        .sender_kind = StreamSenderKind::User,
                                        .sender_id = 42};
    sidecar.json_size = static_cast<std::uint32_t>(line.size());
    hub.publish(StreamIngressTestAccess::item(line, {}, sequence, sidecar));
}

StreamCopiedItem copied_message(std::int64_t id, std::string text = "target") {
    const auto materialized = materialize_message_summary(message(id, std::move(text)));
    REQUIRE(materialized);
    auto data =
        nlohmann::json{{"event", "message"}, {"message", message_summary_json(*materialized)}};
    const auto wire_bytes = data.dump().size() + 1;
    return {.descriptor = {.receive_sequence = static_cast<std::uint64_t>(id),
                           .chat_id = -1001,
                           .sender_id = 42,
                           .event_class = StreamEventClass::Message,
                           .sender_kind = StreamSenderKind::User},
            .data = std::move(data),
            .wire_bytes = wire_bytes};
}

struct PublicationGate {
    using Clock = StreamPollSchedule::Clock;
    std::mutex mutex;
    std::condition_variable cv;
    Clock::time_point current;
    bool entered = false;
    bool released = false;

    std::shared_ptr<StreamWaitScannerHooks> hooks() {
        auto result = std::make_shared<StreamWaitScannerHooks>();
        result->now = [this] {
            const std::lock_guard lock(mutex);
            return current;
        };
        result->sleep_until = [this](Clock::time_point wake) {
            std::unique_lock lock(mutex);
            current = wake;
            if (!entered) {
                entered = true;
                cv.notify_all();
                cv.wait(lock, [this] { return released; });
            }
        };
        return result;
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

} // namespace

TEST_CASE("wait history scanner uses exact local tuples and continues short pages",
          "[stream][wait-scanner]") {
    SessionFixture fixture;
    auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
    std::vector<StreamHistoryRequest> calls;
    std::size_t page = 0;
    auto scan = scan_wait_history(
        fixture.session, worker,
        {.chat_id = -1001,
         .after = 105,
         .matcher = StreamMessageMatcher{},
         .start_history =
             [&](const StreamHistoryRequest& call) {
                 calls.push_back(call);
                 ++page;
                 if (page == 1) {
                     return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                         .total_count = 2, .messages = {message(130), message(120)}}));
                 }
                 return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                     .total_count = 3, .messages = {message(120), message(110), message(100)}}));
             },
         .hooks = nullptr});
    REQUIRE(scan.state);
    REQUIRE(scan.state->initial_match());
    CHECK(scan.state->initial_match()->at("id") == 110);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0] == StreamHistoryRequest{-1001, 0, 0, 100, true});
    CHECK(calls[1] == StreamHistoryRequest{-1001, 120, 0, 100, true});
}

TEST_CASE("wait history scanner claims exact overlap capacity terminal",
          "[stream][wait-scanner][capacity]") {
    SessionFixture fixture;
    auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
    std::int64_t next = 10'000;
    auto scan = scan_wait_history(
        fixture.session, worker,
        {.chat_id = -1001,
         .after = 1,
         .matcher = StreamMessageMatcher{},
         .start_history =
             [&](const StreamHistoryRequest& call) {
                 std::vector<std::optional<tgcli::core::TdMessageSummary>> messages;
                 if (call.from_message_id != 0) {
                     messages.emplace_back(message(call.from_message_id));
                 }
                 while (messages.size() < 100 && next > 1) {
                     messages.emplace_back(message(next--, std::string(9'000, 'x')));
                 }
                 return ready(tgcli::core::TdValue::from(
                     tgcli::core::TdMessages{.total_count = 100, .messages = std::move(messages)}));
             },
         .hooks = nullptr});
    CHECK_FALSE(scan.state);
    const auto terminal = worker.terminal_snapshot();
    REQUIRE(terminal);
    CHECK(terminal->cause == StreamTerminalCause::HistoryOverlap);
    CHECK(terminal->operation == StreamOperation::WaitFor);
}

TEST_CASE("wait history scanner does not call history before actual publication",
          "[stream][wait-scanner][publication]") {
    SessionFixture fixture(false);
    auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
    PublicationGate gate;
    std::atomic<std::size_t> calls{0};
    auto future = std::async(std::launch::async, [&] {
        return scan_wait_history(
            fixture.session, worker,
            {.chat_id = -1001,
             .after = 1,
             .matcher = StreamMessageMatcher{},
             .start_history =
                 [&](const StreamHistoryRequest&) {
                     calls.fetch_add(1, std::memory_order_release);
                     return ready(tgcli::core::TdValue::from(
                         tgcli::core::TdMessages{.total_count = 0, .messages = {}}));
                 },
             .hooks = gate.hooks()});
    });
    gate.wait();
    CHECK(calls.load(std::memory_order_acquire) == 0);
    REQUIRE(fixture.hub->activate_armed(1001, 7, 20) == 1);
    gate.release();
    REQUIRE(future.get().state);
    CHECK(calls.load(std::memory_order_acquire) == 1);
}

TEST_CASE("wait history scanner gives history DTO precedence over overlapping live",
          "[stream][wait-scanner][dedup][filter]") {
    SessionFixture fixture;
    auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
    publish_live(*fixture.hub, 110, "live-does-not-match", 21);
    publish_live(*fixture.hub, 111, "target", 22);
    auto compiled = compile_stream_regex("^target$");
    REQUIRE(std::holds_alternative<StreamRegex>(compiled));
    auto regex = std::make_shared<StreamRegex>(std::move(std::get<StreamRegex>(compiled)));
    auto scan = scan_wait_history(
        fixture.session, worker,
        {.chat_id = -1001,
         .after = 105,
         .matcher = StreamMessageMatcher{.sender_user_id = 42, .regex = regex},
         .start_history =
             [](const StreamHistoryRequest&) {
                 return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                     .total_count = 2, .messages = {message(110, "target"), message(100)}}));
             },
         .hooks = nullptr});
    REQUIRE(scan.state);
    REQUIRE(scan.state->initial_match());
    CHECK(scan.state->initial_match()->at("id") == 110);

    const auto duplicate = materialize_message_summary(message(110, "target"));
    REQUIRE(duplicate);
    const auto line =
        nlohmann::json{{"event", "message"}, {"message", message_summary_json(*duplicate)}};
    const StreamCopiedItem copied{.descriptor = {.receive_sequence = 30,
                                                 .chat_id = -1001,
                                                 .sender_id = 42,
                                                 .event_class = StreamEventClass::Message,
                                                 .sender_kind = StreamSenderKind::User},
                                  .data = line,
                                  .wire_bytes = line.dump().size() + 1};
    CHECK_FALSE(scan.state->match_live(copied));
}

TEST_CASE("wait history scanner rejects malformed advancing pages and accepts local boundaries",
          "[stream][wait-scanner][pagination]") {
    SECTION("out of order") {
        SessionFixture fixture;
        auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
        auto scan = scan_wait_history(
            fixture.session, worker,
            {.chat_id = -1001,
             .after = 1,
             .matcher = StreamMessageMatcher{},
             .start_history =
                 [](const StreamHistoryRequest&) {
                     return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                         .total_count = 2, .messages = {message(120), message(121)}}));
                 },
             .hooks = nullptr});
        CHECK_FALSE(scan.state);
        REQUIRE(worker.terminal_snapshot());
        CHECK(worker.terminal_snapshot()->cause == StreamTerminalCause::PaginationInvalid);
    }
    SECTION("null-only boundary") {
        SessionFixture fixture;
        auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
        auto scan = scan_wait_history(
            fixture.session, worker,
            {.chat_id = -1001,
             .after = 1,
             .matcher = StreamMessageMatcher{},
             .start_history =
                 [](const StreamHistoryRequest&) {
                     return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                         .total_count = 2, .messages = {std::nullopt, std::nullopt}}));
                 },
             .hooks = nullptr});
        REQUIRE(scan.state);
        CHECK_FALSE(scan.state->initial_match());
        CHECK_FALSE(worker.terminal_snapshot());
    }
}

TEST_CASE("wait history scanner never starts a history call after a terminal claim",
          "[stream][wait-scanner][terminal]") {
    SessionFixture fixture;
    auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
    REQUIRE(worker.claim({.cause = StreamTerminalCause::AuthorizationLost,
                          .auth_state = 12,
                          .metadata_failure = {}}));
    std::size_t calls = 0;
    auto scan =
        scan_wait_history(fixture.session, worker,
                          {.chat_id = -1001,
                           .after = 1,
                           .matcher = StreamMessageMatcher{},
                           .start_history =
                               [&](const StreamHistoryRequest&) {
                                   ++calls;
                                   return ready(tgcli::core::TdValue::from(
                                       tgcli::core::TdMessages{.total_count = 0, .messages = {}}));
                               },
                           .hooks = nullptr});
    CHECK_FALSE(scan.state);
    CHECK(calls == 0);
    REQUIRE(worker.terminal_snapshot());
    CHECK(worker.terminal_snapshot()->cause == StreamTerminalCause::AuthorizationLost);
}

TEST_CASE("wait history threshold remains enforced for every post-scan live message",
          "[stream][wait-scanner][after][live]") {
    SessionFixture fixture;
    auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
    auto scan =
        scan_wait_history(fixture.session, worker,
                          {.chat_id = -1001,
                           .after = 105,
                           .matcher = StreamMessageMatcher{},
                           .start_history =
                               [](const StreamHistoryRequest&) {
                                   return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                                       .total_count = 1, .messages = {message(100, "boundary")}}));
                               },
                           .hooks = nullptr});
    REQUIRE(scan.state);
    CHECK_FALSE(scan.state->initial_match());
    CHECK_FALSE(scan.state->match_live(copied_message(104)));
    CHECK_FALSE(scan.state->match_live(copied_message(105)));
    REQUIRE(scan.state->match_live(copied_message(106)));
    CHECK(scan.state->match_live(copied_message(106))->at("id") == 106);
}

TEST_CASE("wait history excludes ineligible and nonmatching DTO bytes from overlap capacity",
          "[stream][wait-scanner][capacity][history]") {
    SECTION("oversized ineligible boundary") {
        SessionFixture fixture;
        auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
        auto scan = scan_wait_history(
            fixture.session, worker,
            {.chat_id = -1001,
             .after = 100,
             .matcher = StreamMessageMatcher{},
             .start_history =
                 [](const StreamHistoryRequest&) {
                     return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                         .total_count = 1,
                         .messages = {message(100, std::string(kStreamQueueItemBytes, 'x'))}}));
                 },
             .hooks = nullptr});
        REQUIRE(scan.state);
        CHECK_FALSE(scan.state->initial_match());
        CHECK_FALSE(worker.terminal_snapshot());
    }

    SECTION("large nonmatching history retains only bounded keys") {
        SessionFixture fixture;
        auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
        auto compiled = compile_stream_regex("^target$");
        REQUIRE(std::holds_alternative<StreamRegex>(compiled));
        auto regex = std::make_shared<StreamRegex>(std::move(std::get<StreamRegex>(compiled)));
        std::int64_t next = 2'000;
        auto scan = scan_wait_history(
            fixture.session, worker,
            {.chat_id = -1001,
             .after = 1,
             .matcher = StreamMessageMatcher{.sender_user_id = std::nullopt, .regex = regex},
             .start_history =
                 [&](const StreamHistoryRequest& call) {
                     std::vector<std::optional<tgcli::core::TdMessageSummary>> messages;
                     if (call.from_message_id != 0) {
                         messages.emplace_back(message(call.from_message_id, "anchor"));
                     }
                     while (messages.size() < 100 && next >= 1'001) {
                         messages.emplace_back(message(next--, std::string(9'000, 'x')));
                     }
                     if (next < 1'001 && messages.size() < 100) {
                         messages.emplace_back(message(1, std::string(kStreamQueueItemBytes, 'x')));
                     }
                     return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                         .total_count = static_cast<std::int32_t>(messages.size()),
                         .messages = std::move(messages)}));
                 },
             .hooks = nullptr});
        REQUIRE(scan.state);
        CHECK_FALSE(scan.state->initial_match());
        CHECK_FALSE(worker.terminal_snapshot());
    }
}

TEST_CASE("wait history bounds its real overlap index and retained candidate",
          "[stream][wait-scanner][capacity][history]") {
    SECTION("consumed-key index") {
        SessionFixture fixture;
        auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
        auto compiled = compile_stream_regex("^never$");
        REQUIRE(std::holds_alternative<StreamRegex>(compiled));
        auto regex = std::make_shared<StreamRegex>(std::move(std::get<StreamRegex>(compiled)));
        std::int64_t next = 3'000;
        std::size_t emitted = 0;
        auto scan = scan_wait_history(
            fixture.session, worker,
            {.chat_id = -1001,
             .after = 1,
             .matcher = StreamMessageMatcher{.sender_user_id = std::nullopt, .regex = regex},
             .start_history =
                 [&](const StreamHistoryRequest& call) {
                     std::vector<std::optional<tgcli::core::TdMessageSummary>> messages;
                     if (call.from_message_id != 0) {
                         messages.emplace_back(message(call.from_message_id, "no"));
                     }
                     while (messages.size() < 100 && emitted < kStreamQueueItems + 1) {
                         messages.emplace_back(message(next--, "no"));
                         ++emitted;
                     }
                     return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                         .total_count = static_cast<std::int32_t>(messages.size()),
                         .messages = std::move(messages)}));
                 },
             .hooks = nullptr});
        CHECK_FALSE(scan.state);
        const auto terminal = worker.terminal_snapshot();
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::HistoryOverlap);
        CHECK(terminal->queued_items == kStreamQueueItems);
        CHECK(terminal->queued_bytes == kStreamQueueItems * sizeof(std::int64_t) * 2);
        CHECK(terminal->incoming_bytes == sizeof(std::int64_t) * 2);
    }

    SECTION("matching candidate bytes") {
        SessionFixture fixture;
        auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
        auto scan = scan_wait_history(
            fixture.session, worker,
            {.chat_id = -1001,
             .after = 1,
             .matcher = StreamMessageMatcher{},
             .start_history =
                 [](const StreamHistoryRequest&) {
                     return ready(tgcli::core::TdValue::from(tgcli::core::TdMessages{
                         .total_count = 1,
                         .messages = {message(2, std::string(kStreamQueueItemBytes, 'x'))}}));
                 },
             .hooks = nullptr});
        CHECK_FALSE(scan.state);
        const auto terminal = worker.terminal_snapshot();
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::HistoryOverlap);
        CHECK(terminal->queued_items == 1);
        CHECK(terminal->queued_bytes == sizeof(std::int64_t) * 2);
        CHECK(terminal->incoming_bytes > kStreamQueueItemBytes);
    }
}

TEST_CASE("wait history maps structural page counts and null mixtures to internal",
          "[stream][wait-scanner][pagination][structure]") {
    const auto verify = [](tgcli::core::TdMessages page) {
        SessionFixture fixture;
        auto worker = testing::RequestSessionTestAccess::stream_worker(fixture.session);
        auto scan =
            scan_wait_history(fixture.session, worker,
                              {.chat_id = -1001,
                               .after = 1,
                               .matcher = StreamMessageMatcher{},
                               .start_history =
                                   [page = std::move(page)](const StreamHistoryRequest&) mutable {
                                       return ready(tgcli::core::TdValue::from(std::move(page)));
                                   },
                               .hooks = nullptr});
        CHECK_FALSE(scan.state);
        const auto terminal = worker.terminal_snapshot();
        REQUIRE(terminal);
        CHECK(terminal->cause == StreamTerminalCause::Internal);
        CHECK(terminal->operation == StreamOperation::WaitFor);
    };

    SECTION("negative total") {
        verify({.total_count = -1, .messages = {}});
    }
    SECTION("total smaller than page") {
        verify({.total_count = 0, .messages = {message(2)}});
    }
    SECTION("mixed null page") {
        verify({.total_count = 2, .messages = {message(2), std::nullopt}});
    }
}
