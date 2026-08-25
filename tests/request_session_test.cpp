#include "common/exit_codes.hpp"
#include "daemon/activity_tracker.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <latch>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

constexpr std::string_view kNonce = "00112233445566778899aabbccddeeff";

struct Captured {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<json> challenge;
    std::optional<json> result;
    std::optional<proto::Error> error;
};

class ThrowingTerminalSink final : public daemon::ResponseSink {
  private:
    daemon::DeliveryOutcome emit_item([[maybe_unused]] json data) override {
        return daemon::DeliveryOutcome::Complete;
    }
    void emit_progress([[maybe_unused]] json data) override {}
    daemon::DeliveryOutcome emit_result([[maybe_unused]] json data) override {
        throw std::runtime_error("terminal transport failure");
    }
    daemon::DeliveryOutcome emit_error([[maybe_unused]] std::string code,
                                       [[maybe_unused]] std::string message,
                                       [[maybe_unused]] json details,
                                       [[maybe_unused]] int exit_code) override {
        throw std::runtime_error("terminal transport failure");
    }
    daemon::ChallengeReply emit_challenge([[maybe_unused]] json data) override {
        return {};
    }
};

class FailingChallengeSink final : public daemon::ResponseSink {
  public:
    [[nodiscard]] bool emitted_error() const {
        return emitted_error_;
    }

  private:
    daemon::DeliveryOutcome emit_item([[maybe_unused]] json data) override {
        return daemon::DeliveryOutcome::Complete;
    }
    void emit_progress([[maybe_unused]] json data) override {}
    daemon::DeliveryOutcome emit_result([[maybe_unused]] json data) override {
        return daemon::DeliveryOutcome::Complete;
    }
    daemon::DeliveryOutcome emit_error([[maybe_unused]] std::string code,
                                       [[maybe_unused]] std::string message,
                                       [[maybe_unused]] json details,
                                       [[maybe_unused]] int exit_code) override {
        emitted_error_ = true;
        return daemon::DeliveryOutcome::Complete;
    }
    daemon::ChallengeReply emit_challenge([[maybe_unused]] json data) override {
        return {std::nullopt, daemon::ChallengeFailure{"INPUT_FAILED", "challenge transport failed",
                                                       json::object(), kGeneric}};
    }

    bool emitted_error_ = false;
};

std::shared_ptr<daemon::CallbackSink> make_sink(
    Captured& captured,
    daemon::CallbackSink::ChallengeFn on_challenge = [](const json&) -> std::optional<json> {
        return std::nullopt;
    }) {
    return std::make_shared<daemon::CallbackSink>(
        [](const json&) {}, [](const json&) {},
        [&captured](json data) {
            const std::lock_guard lock(captured.mutex);
            captured.result = std::move(data);
            captured.cv.notify_all();
        },
        [&captured](std::string code, std::string message, json details, int exit_code) {
            const std::lock_guard lock(captured.mutex);
            captured.error =
                proto::Error{1, std::move(code), std::move(message), std::move(details), exit_code};
            captured.cv.notify_all();
        },
        [&captured, on_challenge = std::move(on_challenge)](json data) mutable {
            {
                const std::lock_guard lock(captured.mutex);
                captured.challenge = data;
                captured.cv.notify_all();
            }
            return on_challenge(std::move(data));
        });
}

proto::Request request(bool tty = true, double timeout = 1.0) {
    proto::Request value("main");
    value.id = 1;
    value.command = {"challenge-test"};
    value.context.tty = tty;
    value.context.timeout_seconds = timeout;
    value.context.cwd = "/";
    return value;
}

daemon::ChallengeSpec challenge(std::uint64_t generation = 4, std::uint64_t auth_sequence = 9) {
    return {proto::ChallengeKind::AuthenticationCode,
            generation,
            auth_sequence,
            "Code: ",
            {{"delivery_type", "sms"}, {"expected_length", 5}, {"resend_timeout", 30}}};
}

proto::Answer answer(const json& emitted, json value = "12345", std::uint64_t id = 1,
                     secure::WipeObserver wipe_observer = {}) {
    return {id,
            {{"nonce", emitted["nonce"]},
             {"sequence", emitted["sequence"]},
             {"client_generation", emitted["client_generation"]},
             {"auth_sequence", emitted["auth_sequence"]},
             {"value", std::move(value)}},
            std::move(wipe_observer)};
}

json wait_challenge(Captured& captured) {
    std::unique_lock lock(captured.mutex);
    REQUIRE(captured.cv.wait_for(lock, std::chrono::seconds(2),
                                 [&captured] { return captured.challenge.has_value(); }));
    return *captured.challenge;
}

daemon::ActivityTracker tracked_activity() {
    daemon::ActivityTracker tracker([] {});
    if (!tracker.daemon_ready(std::nullopt)) {
        throw std::logic_error("activity tracker was already ready");
    }
    return tracker;
}

void wait_until_deadline(const daemon::RequestSession& session) {
    REQUIRE(session.deadline().expires_at.has_value());
    while (daemon::RequestSession::Clock::now() < *session.deadline().expires_at) {
        std::this_thread::yield();
    }
}

} // namespace

// NOLINTBEGIN(misc-const-correctness): Catch2 and concurrency callbacks mutate test state.
TEST_CASE("response sink reports complete and suppressed deliveries", "[session][transport]") {
    daemon::CallbackSink sink([](const json&) {}, [](const json&) {}, [](const json&) {},
                              [](const std::string&, const std::string&, const json&, int) {});

    CHECK(sink.item({{"event", "message"}}) == daemon::DeliveryOutcome::Complete);
    CHECK(sink.result({{"ok", true}}) == daemon::DeliveryOutcome::Complete);
    CHECK(sink.item({{"event", "late"}}) == daemon::DeliveryOutcome::Suppressed);
    CHECK(sink.error("LATE", "late terminal", json::object(), kGeneric) ==
          daemon::DeliveryOutcome::Suppressed);
}

TEST_CASE("request session holds activity until terminal forwarding completes",
          "[session][activity]") {
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    std::atomic<std::size_t> requests_during_result = 0;
    auto sink = std::make_shared<daemon::CallbackSink>(
        [](const json&) {}, [](const json&) {},
        [&tracker, &requests_during_result](const json&) {
            requests_during_result.store(tracker.snapshot().requests, std::memory_order_relaxed);
        },
        [](const std::string&, const std::string&, const json&, int) {});
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));

    CHECK(tracker.snapshot().requests == 1);
    session.result({{"ok", true}});
    CHECK(requests_during_result.load(std::memory_order_relaxed) == 1);
    CHECK(tracker.snapshot().requests == 0);
    session.error("LATE", "suppressed", json::object(), kGeneric);
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("request session retains activity across challenge reservation and in-flight waits",
          "[challenge][session][activity]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));

    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    CHECK(tracker.snapshot().requests == 1);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    CHECK(future.get().status() == daemon::ChallengeStatus::Answered);
    CHECK(tracker.snapshot().requests == 1);
    REQUIRE(session.reserve_in_flight());
    CHECK(tracker.snapshot().requests == 1);
    session.result({{"ok", true}});
    CHECK(tracker.snapshot().requests == 0);
    CHECK(session.in_flight_state() == daemon::InFlightState::InFlight);
    session.settle_in_flight();
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("unlimited request challenges remain disconnect and shutdown aware",
          "[challenge][session][deadline][unlimited]") {
    for (const auto cause : {std::string_view("disconnect"), std::string_view("shutdown")}) {
        DYNAMIC_SECTION(cause) {
            Captured captured;
            auto sink = make_sink(captured);
            daemon::RequestSession session(
                request(), sink, 17, [] { return std::string(kNonce); },
                daemon::ActivityTracker::Token{}, nullptr, RequestDeadline{});
            CHECK_FALSE(session.deadline().expires_at);

            auto future = std::async(std::launch::async,
                                     [&session] { return session.challenge(challenge()); });
            static_cast<void>(wait_challenge(captured));
            CHECK(future.wait_for(std::chrono::seconds(0)) != std::future_status::ready);

            if (cause == "disconnect") {
                session.disconnect();
            } else {
                session.shutdown();
            }
            CHECK(future.get().status() == (cause == "disconnect"
                                                ? daemon::ChallengeStatus::Disconnected
                                                : daemon::ChallengeStatus::Shutdown));
        }
    }
}

TEST_CASE("request session disconnect releases activity before orphaned work settles",
          "[session][activity][disconnect]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
    REQUIRE(session.reserve_direct_in_flight());
    CHECK(tracker.snapshot().requests == 1);

    session.disconnect();
    CHECK(session.in_flight_state() == daemon::InFlightState::Orphaned);
    CHECK(tracker.snapshot().requests == 0);
    session.result({{"late", true}});
    CHECK_FALSE(captured.result.has_value());
    CHECK(tracker.snapshot().requests == 0);
    session.settle_in_flight();
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("request session disconnect releases activity from an orphaned auth query",
          "[challenge][session][activity][disconnect]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    CHECK(future.get().status() == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight());
    CHECK(tracker.snapshot().requests == 1);

    session.disconnect();
    CHECK(session.in_flight_state() == daemon::InFlightState::Orphaned);
    CHECK(tracker.snapshot().requests == 0);
    session.settle_in_flight();
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("request session promotes its sole activity owner to a subscription",
          "[session][activity][subscription]") {
    for (const auto termination : {std::string_view("planned_expiry"), std::string_view("error"),
                                   std::string_view("disconnect")}) {
        DYNAMIC_SECTION(termination) {
            Captured captured;
            auto sink = make_sink(captured);
            daemon::ActivityTracker tracker = tracked_activity();
            auto activity = tracker.try_request();
            REQUIRE(activity);
            daemon::RequestSession session(
                request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));

            REQUIRE(session.promote_to_subscription());
            auto state = tracker.snapshot();
            CHECK(state.requests == 0);
            CHECK(state.subscriptions == 1);
            CHECK_FALSE(state.zero_since);
            CHECK_FALSE(session.promote_to_subscription());
            if (termination == "planned_expiry") {
                session.result({{"planned_expiry", true}});
            } else if (termination == "error") {
                session.error("CANCELLED", "subscription cancelled", json::object(), kGeneric);
            } else {
                session.disconnect();
            }
            state = tracker.snapshot();
            CHECK(state.requests == 0);
            CHECK(state.subscriptions == 0);
        }
    }
}

TEST_CASE("stale and cancelled challenge answers retain request activity until terminal",
          "[challenge][session][activity]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    auto stale = answer(emitted);
    stale.answer["auth_sequence"] = 8;
    const auto stale_disposition = session.receive_answer(std::move(stale));
    CHECK(stale_disposition == daemon::AnswerDisposition::StaleIgnored);
    CHECK(tracker.snapshot().requests == 1);
    auto cancelled = answer(emitted);
    cancelled.answer.erase("value");
    cancelled.answer["cancelled"] = true;
    const auto cancelled_disposition = session.receive_answer(std::move(cancelled));
    CHECK(cancelled_disposition == daemon::AnswerDisposition::Cancelled);
    CHECK(future.get().status() == daemon::ChallengeStatus::Cancelled);
    CHECK(tracker.snapshot().requests == 1);
    session.error("CANCELLED", "request cancelled", json::object(), kGeneric);
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("rejected challenge answers release activity through their protocol terminal",
          "[challenge][session][activity]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    auto rejected = answer(emitted);
    rejected.answer["nonce"] = std::string(kNonce.size(), 'f');
    const auto disposition = session.receive_answer(std::move(rejected));

    CHECK(disposition == daemon::AnswerDisposition::Rejected);
    CHECK(future.get().status() == daemon::ChallengeStatus::ProtocolError);
    REQUIRE(captured.error.has_value());
    CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("disconnect cleans up a request that timed out before its first challenge",
          "[challenge][session][activity][timeout][disconnect]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(true, 0.01), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
    wait_until_deadline(session);

    CHECK(session.challenge(challenge()).status() == daemon::ChallengeStatus::TimedOut);
    CHECK_FALSE(session.cancellation_requested());
    CHECK(tracker.snapshot().requests == 1);
    session.disconnect();
    CHECK(session.cancellation_requested());
    CHECK(session.in_flight_state() == daemon::InFlightState::None);
    CHECK(tracker.snapshot().requests == 0);
    session.error("TIMEOUT", "late timeout terminal", json::object(), kTimeout);
    CHECK_FALSE(captured.result.has_value());
    CHECK_FALSE(captured.error.has_value());
}

TEST_CASE("disconnect cleans up a timed-out challenge before a late answer",
          "[challenge][session][activity][timeout][disconnect]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(true, 0.01), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);

    CHECK(future.get().status() == daemon::ChallengeStatus::TimedOut);
    CHECK_FALSE(session.cancellation_requested());
    CHECK(tracker.snapshot().requests == 1);
    session.disconnect();
    CHECK(session.cancellation_requested());
    CHECK(tracker.snapshot().requests == 0);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::RequestTerminated);
    session.error("TIMEOUT", "late timeout terminal", json::object(), kTimeout);
    CHECK_FALSE(captured.result.has_value());
    CHECK_FALSE(captured.error.has_value());
}

TEST_CASE("disconnect cleans up direct and auth work orphaned after timeout",
          "[challenge][session][activity][timeout][disconnect]") {
    for (const auto mode : {std::string_view("direct"), std::string_view("auth")}) {
        DYNAMIC_SECTION(mode) {
            Captured captured;
            auto sink = make_sink(captured);
            daemon::ActivityTracker tracker = tracked_activity();
            auto activity = tracker.try_request();
            REQUIRE(activity);
            daemon::RequestSession session(
                request(true, 0.05), sink, 17, [] { return std::string(kNonce); },
                std::move(*activity));
            std::vector<daemon::InFlightState> transitions;
            session.set_in_flight_hook(
                [&transitions](daemon::InFlightState state) { transitions.push_back(state); });
            if (mode == "direct") {
                REQUIRE(session.reserve_direct_in_flight());
            } else {
                auto future = std::async(std::launch::async,
                                         [&session] { return session.challenge(challenge()); });
                const auto emitted = wait_challenge(captured);
                CHECK(session.receive_answer(answer(emitted)) ==
                      daemon::AnswerDisposition::Accepted);
                CHECK(future.get().status() == daemon::ChallengeStatus::Answered);
                REQUIRE(session.reserve_in_flight());
            }
            wait_until_deadline(session);
            proto::Answer timeout_probe{session.request().id, json::object()};
            const auto timeout_disposition = session.receive_answer(std::move(timeout_probe));
            CHECK(timeout_disposition == daemon::AnswerDisposition::RequestTerminated);
            CHECK_FALSE(session.cancellation_requested());
            CHECK(session.in_flight_state() == daemon::InFlightState::InFlight);
            CHECK(tracker.snapshot().requests == 1);

            session.disconnect();
            CHECK(session.cancellation_requested());
            CHECK(session.in_flight_state() == daemon::InFlightState::Orphaned);
            CHECK(tracker.snapshot().requests == 0);
            session.result({{"late", true}});
            CHECK_FALSE(captured.result.has_value());
            CHECK_FALSE(captured.error.has_value());
            session.settle_in_flight();
            CHECK(session.in_flight_state() == daemon::InFlightState::None);
            REQUIRE(transitions.size() == 3);
            CHECK(transitions[0] == daemon::InFlightState::InFlight);
            CHECK(transitions[1] == daemon::InFlightState::Orphaned);
            CHECK(transitions[2] == daemon::InFlightState::None);
            CHECK(tracker.snapshot().requests == 0);
        }
    }
}

TEST_CASE("challenge transport terminal releases request activity", "[session][activity]") {
    FailingChallengeSink sink;
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));

    CHECK(session.challenge(challenge()).status() == daemon::ChallengeStatus::ProtocolError);
    CHECK(sink.emitted_error());
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("terminal transport exceptions cannot retain request activity",
          "[session][activity][exception]") {
    ThrowingTerminalSink sink;
    daemon::ActivityTracker tracker = tracked_activity();
    auto activity = tracker.try_request();
    REQUIRE(activity);
    daemon::RequestSession session(
        request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));

    CHECK_THROWS_AS(session.result({{"ok", true}}), std::runtime_error);
    CHECK(tracker.snapshot().requests == 0);
}

TEST_CASE("request session terminal races release activity exactly once",
          "[session][activity][race][stress]") {
    for (int iteration = 0; iteration < 250; ++iteration) {
        daemon::ActivityTracker tracker = tracked_activity();
        auto activity = tracker.try_request();
        REQUIRE(activity);
        std::atomic<int> terminals = 0;
        std::atomic<bool> held_during_terminal = true;
        auto record_terminal = [&] {
            held_during_terminal.store(held_during_terminal.load(std::memory_order_relaxed) &&
                                           tracker.snapshot().requests == 1,
                                       std::memory_order_relaxed);
            terminals.fetch_add(1, std::memory_order_relaxed);
        };
        auto sink = std::make_shared<daemon::CallbackSink>(
            [](const json&) {}, [](const json&) {},
            [&record_terminal](const json&) { record_terminal(); },
            [&record_terminal](const std::string&, const std::string&, const json&, int) {
                record_terminal();
            });
        daemon::RequestSession session(
            request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
        std::latch start(3);
        std::thread result([&] {
            start.count_down();
            start.wait();
            session.result({{"iteration", iteration}});
        });
        std::thread error([&] {
            start.count_down();
            start.wait();
            session.error("RACE", "terminal race", json::object(), kGeneric);
        });
        start.count_down();
        start.wait();
        result.join();
        error.join();

        CHECK(terminals.load(std::memory_order_relaxed) == 1);
        CHECK(held_during_terminal.load(std::memory_order_relaxed));
        CHECK(tracker.snapshot().requests == 0);
    }
}

TEST_CASE("request session releases activity on timeout shutdown destruction and construction",
          "[session][activity][lifecycle]") {
    SECTION("timeout remains active until its terminal error") {
        Captured captured;
        auto sink = make_sink(captured);
        daemon::ActivityTracker tracker = tracked_activity();
        auto activity = tracker.try_request();
        REQUIRE(activity);
        daemon::RequestSession session(
            request(true, 0.002), sink, 17, [] { return std::string(kNonce); },
            std::move(*activity));
        auto future =
            std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
        wait_challenge(captured);
        CHECK(future.get().status() == daemon::ChallengeStatus::TimedOut);
        CHECK(tracker.snapshot().requests == 1);
        session.error("TIMEOUT", "request timed out", json::object(), kTimeout);
        CHECK(tracker.snapshot().requests == 0);
    }

    SECTION("shutdown terminal releases activity") {
        Captured captured;
        auto sink = make_sink(captured);
        daemon::ActivityTracker tracker = tracked_activity();
        auto activity = tracker.try_request();
        REQUIRE(activity);
        daemon::RequestSession session(
            request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
        session.shutdown();
        REQUIRE(captured.error.has_value());
        CHECK(captured.error->code == "DAEMON_SHUTDOWN");
        CHECK(tracker.snapshot().requests == 0);
    }

    SECTION("destruction releases unterminated activity") {
        Captured captured;
        auto sink = make_sink(captured);
        daemon::ActivityTracker tracker = tracked_activity();
        auto activity = tracker.try_request();
        REQUIRE(activity);
        {
            daemon::RequestSession session(
                request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity));
            CHECK(tracker.snapshot().requests == 1);
        }
        CHECK(tracker.snapshot().requests == 0);
    }

    SECTION("throwing construction releases supplied activity") {
        daemon::ActivityTracker tracker = tracked_activity();
        auto activity = tracker.try_request();
        REQUIRE(activity);
        std::shared_ptr<daemon::ResponseSink> sink;
        CHECK_THROWS_AS(
            daemon::RequestSession(
                request(), sink, 17, [] { return std::string(kNonce); }, std::move(*activity)),
            std::invalid_argument);
        CHECK(tracker.snapshot().requests == 0);
    }
}
// NOLINTEND(misc-const-correctness)

TEST_CASE("request session accepts one exact answer", "[challenge][session]") {
    // NOLINTBEGIN(misc-const-correctness): Catch2 and async callbacks mutate test state.
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });

    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    auto outcome = future.get();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(outcome.status() == daemon::ChallengeStatus::Answered);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    std::string value;
    REQUIRE(outcome.take_string(value));
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(value == "12345");
    CHECK_FALSE(outcome.take_string(value));
    CHECK(session.reserve_in_flight()); // NOLINT(misc-const-correctness): Catch2 expansion.
    session.settle_in_flight();
    // NOLINTEND(misc-const-correctness)
}

TEST_CASE("challenge answers are moved from the protocol frame and consumed once",
          "[challenge][session][secret]") {
    // NOLINTBEGIN(misc-const-correctness): Catch2 and observer callbacks mutate test state.
    STATIC_CHECK_FALSE(std::is_copy_constructible_v<daemon::ChallengeOutcome>);
    STATIC_CHECK_FALSE(std::is_copy_assignable_v<daemon::ChallengeOutcome>);
    STATIC_CHECK_FALSE(std::is_copy_constructible_v<proto::Answer>);
    STATIC_CHECK_FALSE(std::is_copy_assignable_v<proto::Answer>);
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    constexpr std::string_view sentinel = "123456";
    std::vector<std::tuple<std::string, std::size_t, bool>> wipe_observations;
    std::mutex wipe_mutex;
    auto observer = [&wipe_observations, &wipe_mutex](std::string_view stage, const char* bytes,
                                                      std::size_t size) {
        const std::lock_guard lock(wipe_mutex);
        wipe_observations.emplace_back(stage, size,
                                       std::all_of(bytes, bytes + static_cast<std::ptrdiff_t>(size),
                                                   [](char value) { return value == '\0'; }));
    };
    auto protocol_answer = answer(emitted, std::string(sentinel), 1, observer);
    const auto disposition = session.receive_answer(std::move(protocol_answer));
    CHECK(disposition == daemon::AnswerDisposition::Accepted);
    auto outcome = future.get();
    std::string consumed;
    REQUIRE(outcome.take_string(consumed));
    CHECK(consumed == sentinel);
    CHECK_FALSE(outcome.take_string(consumed));
    consumed.assign(consumed.size(), '\0');
    const std::lock_guard wipe_lock(wipe_mutex);
    for (const std::string_view required_stage :
         {"answer_source", "answer_move_source", "challenge_value_source",
          "challenge_outcome_move_source", "challenge_take_source"}) {
        CHECK(std::ranges::any_of(
            wipe_observations, [required_stage, sentinel](const auto& observation) {
                return std::get<0>(observation) == required_stage &&
                       std::get<1>(observation) == sentinel.size() && std::get<2>(observation);
            }));
    }
    CHECK(std::ranges::any_of(wipe_observations, [](const auto& observation) {
        return std::get<0>(observation) == "answer_payload" && std::get<2>(observation);
    }));
    // NOLINTEND(misc-const-correctness)
}

TEST_CASE("explicit cancellation consumes the challenge without a value", "[challenge][session]") {
    // NOLINTBEGIN(misc-const-correctness): Catch2 and observer callbacks mutate test state.
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    std::vector<std::pair<std::string, bool>> wipe_observations;
    auto observer = [&wipe_observations](std::string_view stage, const char* bytes,
                                         std::size_t size) {
        wipe_observations.emplace_back(stage,
                                       std::all_of(bytes, bytes + static_cast<std::ptrdiff_t>(size),
                                                   [](char value) { return value == '\0'; }));
    };
    auto cancelled = answer(emitted, "123456", 1, observer);
    cancelled.answer.erase("value");
    cancelled.answer["cancelled"] = true;
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    const auto cancellation_disposition = session.receive_answer(std::move(cancelled));
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(cancellation_disposition == daemon::AnswerDisposition::Cancelled);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(future.get().status() == daemon::ChallengeStatus::Cancelled);
    for (const std::string_view required_stage :
         {"answer_source", "answer_move_source", "answer_payload"}) {
        CHECK(std::ranges::any_of(wipe_observations, [required_stage](const auto& observation) {
            return observation.first == required_stage && observation.second;
        }));
    }
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    auto duplicate = answer(emitted);
    duplicate.answer.erase("value");
    duplicate.answer["cancelled"] = true;
    const auto duplicate_disposition = session.receive_answer(std::move(duplicate));
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(duplicate_disposition == daemon::AnswerDisposition::DuplicateIgnored);
    // NOLINTEND(misc-const-correctness)
}

TEST_CASE("rejected answers wipe a short token without emitting it",
          "[challenge][session][secret][error]") {
    // NOLINTBEGIN(misc-const-correctness): Catch2 and observer callbacks mutate test state.
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    constexpr std::string_view sentinel = "1:token";
    std::vector<std::pair<std::string, bool>> wipe_observations;
    auto observer = [&wipe_observations, sentinel](std::string_view stage, const char* bytes,
                                                   std::size_t size) {
        if (size == sentinel.size()) {
            wipe_observations.emplace_back(
                stage, std::all_of(bytes, bytes + static_cast<std::ptrdiff_t>(size),
                                   [](char value) { return value == '\0'; }));
        }
    };
    auto rejected = answer(emitted, std::string(sentinel), 1, observer);
    rejected.answer["nonce"] = std::string(kNonce.size(), 'f');
    const auto disposition = session.receive_answer(std::move(rejected));
    CHECK(disposition == daemon::AnswerDisposition::Rejected);
    CHECK(future.get().status() == daemon::ChallengeStatus::ProtocolError);
    REQUIRE(captured.error.has_value());
    CHECK(captured.error->message.find(sentinel) == std::string::npos);
    CHECK(captured.error->details.dump().find(sentinel) == std::string::npos);
    for (const std::string_view required_stage :
         {"answer_source", "answer_move_source", "answer_payload"}) {
        CHECK(std::ranges::any_of(wipe_observations, [required_stage](const auto& observation) {
            return observation.first == required_stage && observation.second;
        }));
    }
    // NOLINTEND(misc-const-correctness)
}

TEST_CASE("request session rejects identity violations without waking with a value",
          "[challenge][session]") {
    struct Mutation {
        const char* reason;
        std::function<void(proto::Answer&)> apply;
    };
    const std::vector<Mutation> mutations{
        {"unknown_request", [](proto::Answer& value) { value.id = 99; }},
        {"nonce_mismatch",
         [](proto::Answer& value) { value.answer["nonce"] = std::string(32, 'f'); }},
        {"future_sequence", [](proto::Answer& value) { value.answer["sequence"] = 2; }},
        {"generation_mismatch",
         [](proto::Answer& value) { value.answer["client_generation"] = 5; }},
        {"generation_mismatch", [](proto::Answer& value) { value.answer["auth_sequence"] = 10; }},
    };

    for (const auto& mutation : mutations) {
        DYNAMIC_SECTION(mutation.reason) {
            Captured captured; // NOLINT(misc-const-correctness): callback output.
            auto sink = make_sink(captured);
            daemon::RequestSession session( // NOLINT(misc-const-correctness): mutable state.
                request(), sink, 17, [] { return std::string(kNonce); });
            auto future = std::async(std::launch::async,
                                     [&session] { return session.challenge(challenge()); });
            const auto emitted = wait_challenge(captured);
            auto invalid = answer(emitted);
            mutation.apply(invalid);
            const auto invalid_id = invalid.id;
            // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
            const auto disposition = session.receive_answer(std::move(invalid));
            // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
            CHECK(disposition == daemon::AnswerDisposition::Rejected);
            // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
            CHECK(future.get().status() == daemon::ChallengeStatus::ProtocolError);
            // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
            REQUIRE(captured.error.has_value());
            // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
            CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
            // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
            CHECK(captured.error->details ==
                  json{{"request_id", invalid_id}, {"reason", mutation.reason}});
        }
    }
}

TEST_CASE("old generation, stale sequence, and exact duplicates are ignored",
          "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });

    auto first =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto first_frame = wait_challenge(captured);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(first_frame)) == daemon::AnswerDisposition::Accepted);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(first.get().status() == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight()); // NOLINT(misc-const-correctness): Catch2 expansion.
    session.settle_in_flight();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(first_frame)) ==
          daemon::AnswerDisposition::DuplicateIgnored);

    {
        const std::lock_guard lock(captured.mutex);
        captured.challenge.reset();
    }
    auto second =
        std::async(std::launch::async, [&session] { return session.challenge(challenge(4, 10)); });
    const auto second_frame = wait_challenge(captured);
    auto stale_sequence = answer(second_frame);
    stale_sequence.answer["sequence"] = 1;
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    const auto stale_sequence_disposition = session.receive_answer(std::move(stale_sequence));
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(stale_sequence_disposition == daemon::AnswerDisposition::StaleIgnored);
    auto stale_generation = answer(second_frame);
    stale_generation.answer["auth_sequence"] = 9;
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    const auto stale_generation_disposition = session.receive_answer(std::move(stale_generation));
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(stale_generation_disposition == daemon::AnswerDisposition::StaleIgnored);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(second_frame)) == daemon::AnswerDisposition::Accepted);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(second.get().status() == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight()); // NOLINT(misc-const-correctness): Catch2 expansion.
    session.settle_in_flight();
}

TEST_CASE("consumed answer identity includes the request id", "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(future.get().status() == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight()); // NOLINT(misc-const-correctness): Catch2 expansion.
    session.settle_in_flight();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::DuplicateIgnored);

    auto changed_request = answer(emitted);
    changed_request.id = 2;
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    const auto changed_request_disposition = session.receive_answer(std::move(changed_request));
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(changed_request_disposition == daemon::AnswerDisposition::Rejected);
    REQUIRE(captured.error.has_value()); // NOLINT(misc-const-correctness): Catch2 expansion.
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(captured.error->details == json{{"request_id", 2}, {"reason", "unknown_request"}});
}

TEST_CASE("same-state auth update supersedes the waiter", "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    CHECK(session.supersede(4, 10)); // NOLINT(misc-const-correctness): Catch2 expansion.
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(future.get().status() == daemon::ChallengeStatus::Superseded);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::StaleIgnored);
}

TEST_CASE("disconnect releases a prompt waiter and exposes orphan state", "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });
    std::atomic<daemon::InFlightState> observed{// NOLINT(misc-const-correctness): callback output.
                                                daemon::InFlightState::None};
    session.set_in_flight_hook([&observed](daemon::InFlightState state) { observed.store(state); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    wait_challenge(captured);
    session.disconnect();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(future.get().status() == daemon::ChallengeStatus::Disconnected);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.in_flight_state() == daemon::InFlightState::None);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(observed.load() == daemon::InFlightState::None);
}

TEST_CASE("disconnect after answer preserves an orphaned in-flight query", "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(future.get().status() == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight()); // NOLINT(misc-const-correctness): Catch2 expansion.
    session.disconnect();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.in_flight_state() == daemon::InFlightState::Orphaned);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::RequestTerminated);
    session.settle_in_flight();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.in_flight_state() == daemon::InFlightState::None);
}

TEST_CASE("disconnect after acceptance but before query claim prevents the send",
          "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    session.disconnect();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(future.get().status() == daemon::ChallengeStatus::Answered);
    CHECK_FALSE(session.reserve_in_flight()); // NOLINT(misc-const-correctness): Catch2 expansion.
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.in_flight_state() == daemon::InFlightState::None);
}

TEST_CASE("disconnect between challenges releases only the current waiter",
          "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(), sink, 17, [] { return std::string(kNonce); });
    auto first =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto first_frame = wait_challenge(captured);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.receive_answer(answer(first_frame)) == daemon::AnswerDisposition::Accepted);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(first.get().status() == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight()); // NOLINT(misc-const-correctness): Catch2 expansion.
    session.settle_in_flight();

    {
        const std::lock_guard lock(captured.mutex);
        captured.challenge.reset();
    }
    auto second =
        std::async(std::launch::async, [&session] { return session.challenge(challenge(4, 10)); });
    wait_challenge(captured);
    session.disconnect();
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(second.get().status() == daemon::ChallengeStatus::Disconnected);
}

TEST_CASE("deadline and answer acceptance have one serialized winner", "[challenge][session]") {
    // NOLINTBEGIN(misc-const-correctness): Catch2 and concurrency callbacks mutate test state.
    constexpr int target_iterations = 64;
    constexpr int max_attempts = target_iterations * 4;
    int completed_iterations = 0; // NOLINT(misc-const-correctness): loop counter.
    for (int attempt = 0;         // NOLINT(misc-const-correctness): loop counter.
         attempt < max_attempts && completed_iterations < target_iterations; ++attempt) {
        CAPTURE(attempt, completed_iterations);
        Captured captured; // NOLINT(misc-const-correctness): callback output.
        auto sink = make_sink(captured);
        std::optional<daemon::ChallengeStatus> worker_status;
        daemon::RequestSession session( // NOLINT(misc-const-correctness): mutable state.
            request(true, 0.003), sink, 17, [] { return std::string(kNonce); });
        auto future = std::async(std::launch::async, [&] {
            auto outcome = session.challenge(challenge());
            const std::lock_guard lock(captured.mutex);
            worker_status = outcome.status();
            captured.cv.notify_all();
            return outcome;
        });
        json emitted;
        bool observed = false;
        bool challenge_emitted = false;
        {
            std::unique_lock lock(captured.mutex);
            observed = captured.cv.wait_for(lock, std::chrono::seconds(2), [&] {
                return captured.challenge.has_value() || worker_status.has_value();
            });
            if (captured.challenge) {
                emitted = *captured.challenge;
                challenge_emitted = true;
            }
        }
        REQUIRE(observed);
        if (!challenge_emitted) {
            REQUIRE(worker_status);
            CHECK(*worker_status == daemon::ChallengeStatus::TimedOut);
            CHECK(future.get().status() == daemon::ChallengeStatus::TimedOut);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(completed_iterations % 5));
        const auto disposition = session.receive_answer(answer(emitted));
        const auto status = future.get().status();
        // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
        CHECK(((disposition == daemon::AnswerDisposition::Accepted &&
                status == daemon::ChallengeStatus::Answered) ||
               (disposition == daemon::AnswerDisposition::RequestTerminated &&
                status == daemon::ChallengeStatus::TimedOut)));
        ++completed_iterations;
    }
    CHECK(completed_iterations == target_iterations);
    // NOLINTEND(misc-const-correctness)
}

TEST_CASE("an answer received after the deadline is wiped before the timed-out worker resumes",
          "[challenge][session][secret][ordering][timeout]") {
    // NOLINTBEGIN(misc-const-correctness): Catch2 and async callbacks mutate test state.
    Captured captured;
    std::atomic<bool> release_transport{false};
    auto sink = make_sink(captured, [&release_transport](const json&) -> std::optional<json> {
        while (!release_transport.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return std::nullopt;
    });
    daemon::RequestSession session(request(true, 0.01), sink, 17,
                                   [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    constexpr std::string_view sentinel = "123456";
    std::atomic<bool> answer_source_wiped{false};
    std::atomic<bool> answer_move_source_wiped{false};
    std::atomic<bool> answer_payload_wiped{false};
    auto observer = [&](std::string_view stage, const char* bytes, std::size_t size) {
        if (size != sentinel.size() ||
            !std::all_of(bytes, bytes + static_cast<std::ptrdiff_t>(size),
                         [](char value) { return value == '\0'; })) {
            return;
        }
        if (stage == "answer_source") {
            answer_source_wiped.store(true, std::memory_order_release);
        } else if (stage == "answer_move_source") {
            answer_move_source_wiped.store(true, std::memory_order_release);
        } else if (stage == "answer_payload") {
            answer_payload_wiped.store(true, std::memory_order_release);
        }
    };
    auto expired = answer(emitted, std::string(sentinel), 1, observer);
    const auto disposition = session.receive_answer(std::move(expired));
    const bool sources_wiped_before_resume =
        answer_source_wiped.load(std::memory_order_acquire) &&
        answer_move_source_wiped.load(std::memory_order_acquire) &&
        answer_payload_wiped.load(std::memory_order_acquire);
    release_transport.store(true, std::memory_order_release);

    CHECK(disposition == daemon::AnswerDisposition::RequestTerminated);
    CHECK(sources_wiped_before_resume);
    CHECK(future.get().status() == daemon::ChallengeStatus::TimedOut);
    // NOLINTEND(misc-const-correctness)
}

TEST_CASE("non-TTY sessions never emit a challenge", "[challenge][session]") {
    Captured captured; // NOLINT(misc-const-correctness): mutated through sink callbacks.
    auto sink = make_sink(captured);
    daemon::RequestSession session( // NOLINT(misc-const-correctness): owns mutable state.
        request(false), sink, 17, [] { return std::string(kNonce); });
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK(session.challenge(challenge()).status() == daemon::ChallengeStatus::NoTty);
    // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
    CHECK_FALSE(captured.challenge.has_value());
}

TEST_CASE("in-process dispatch uses the same challenge session", "[challenge][dispatch]") {
    daemon::Dispatcher dispatcher;
    dispatcher.register_command(
        "challenge-test",
        {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& session) {
             auto outcome = session.challenge(challenge());
             if (outcome.status() == daemon::ChallengeStatus::Answered) {
                 if (!session.reserve_in_flight()) {
                     return;
                 }
                 session.settle_in_flight();
                 std::string value;
                 static_cast<void>(outcome.take_string(value));
                 session.result({{"value", value}});
             }
         }});
    Captured captured;
    auto sink = make_sink(captured, [](const json& emitted) -> std::optional<json> {
        return answer(emitted).answer;
    });
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    dispatcher.dispatch(session);
    REQUIRE(captured.result.has_value());
    CHECK(*captured.result == json{{"value", "12345"}});
}
