#include "common/exit_codes.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
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
    proto::Request value;
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

} // namespace

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
    for (int iteration = 0; // NOLINT(misc-const-correctness): loop counter.
         iteration < 64; ++iteration) {
        Captured captured; // NOLINT(misc-const-correctness): callback output.
        auto sink = make_sink(captured);
        daemon::RequestSession session( // NOLINT(misc-const-correctness): mutable state.
            request(true, 0.003), sink, 17, [] { return std::string(kNonce); });
        auto future =
            std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
        const auto emitted = wait_challenge(captured);
        std::this_thread::sleep_for(std::chrono::milliseconds(iteration % 5));
        const auto disposition = session.receive_answer(answer(emitted));
        const auto status = future.get().status();
        // NOLINTNEXTLINE(misc-const-correctness): Catch2 creates a mutable handler.
        CHECK(((disposition == daemon::AnswerDisposition::Accepted &&
                status == daemon::ChallengeStatus::Answered) ||
               (disposition == daemon::AnswerDisposition::RequestTerminated &&
                status == daemon::ChallengeStatus::TimedOut)));
    }
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
