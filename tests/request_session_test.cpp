#include "common/exit_codes.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

proto::Answer answer(const json& emitted, json value = "12345", std::uint64_t id = 1) {
    return {id,
            {{"nonce", emitted["nonce"]},
             {"sequence", emitted["sequence"]},
             {"client_generation", emitted["client_generation"]},
             {"auth_sequence", emitted["auth_sequence"]},
             {"value", std::move(value)}}};
}

json wait_challenge(Captured& captured) {
    std::unique_lock lock(captured.mutex);
    REQUIRE(captured.cv.wait_for(lock, std::chrono::seconds(2),
                                 [&captured] { return captured.challenge.has_value(); }));
    return *captured.challenge;
}

} // namespace

TEST_CASE("request session accepts one exact answer", "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });

    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    const auto outcome = future.get();
    CHECK(outcome.status == daemon::ChallengeStatus::Answered);
    REQUIRE(std::holds_alternative<std::string>(outcome.value));
    CHECK(std::get<std::string>(outcome.value) == "12345");
    CHECK(session.reserve_in_flight());
    session.settle_in_flight();
}

TEST_CASE("explicit cancellation consumes the challenge without a value", "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    auto cancelled = answer(emitted);
    cancelled.answer.erase("value");
    cancelled.answer["cancelled"] = true;
    CHECK(session.receive_answer(cancelled) == daemon::AnswerDisposition::Cancelled);
    CHECK(future.get().status == daemon::ChallengeStatus::Cancelled);
    CHECK(session.receive_answer(cancelled) == daemon::AnswerDisposition::DuplicateIgnored);
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
            Captured captured;
            auto sink = make_sink(captured);
            daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
            auto future = std::async(std::launch::async,
                                     [&session] { return session.challenge(challenge()); });
            const auto emitted = wait_challenge(captured);
            auto invalid = answer(emitted);
            mutation.apply(invalid);
            CHECK(session.receive_answer(invalid) == daemon::AnswerDisposition::Rejected);
            CHECK(future.get().status == daemon::ChallengeStatus::ProtocolError);
            REQUIRE(captured.error.has_value());
            CHECK(captured.error->code == "PROTOCOL_ANSWER_INVALID");
            CHECK(captured.error->details ==
                  json{{"request_id", invalid.id}, {"reason", mutation.reason}});
        }
    }
}

TEST_CASE("old generation, stale sequence, and exact duplicates are ignored",
          "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });

    auto first =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto first_frame = wait_challenge(captured);
    const auto exact = answer(first_frame);
    CHECK(session.receive_answer(exact) == daemon::AnswerDisposition::Accepted);
    CHECK(first.get().status == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight());
    session.settle_in_flight();
    CHECK(session.receive_answer(exact) == daemon::AnswerDisposition::DuplicateIgnored);

    {
        const std::lock_guard lock(captured.mutex);
        captured.challenge.reset();
    }
    auto second =
        std::async(std::launch::async, [&session] { return session.challenge(challenge(4, 10)); });
    const auto second_frame = wait_challenge(captured);
    auto stale_sequence = answer(second_frame);
    stale_sequence.answer["sequence"] = 1;
    CHECK(session.receive_answer(stale_sequence) == daemon::AnswerDisposition::StaleIgnored);
    auto stale_generation = answer(second_frame);
    stale_generation.answer["auth_sequence"] = 9;
    CHECK(session.receive_answer(stale_generation) == daemon::AnswerDisposition::StaleIgnored);
    CHECK(session.receive_answer(answer(second_frame)) == daemon::AnswerDisposition::Accepted);
    CHECK(second.get().status == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight());
    session.settle_in_flight();
}

TEST_CASE("consumed answer identity includes the request id", "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    const auto exact = answer(emitted);
    CHECK(session.receive_answer(exact) == daemon::AnswerDisposition::Accepted);
    CHECK(future.get().status == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight());
    session.settle_in_flight();
    CHECK(session.receive_answer(exact) == daemon::AnswerDisposition::DuplicateIgnored);

    auto changed_request = exact;
    changed_request.id = 2;
    CHECK(session.receive_answer(changed_request) == daemon::AnswerDisposition::Rejected);
    REQUIRE(captured.error.has_value());
    CHECK(captured.error->details == json{{"request_id", 2}, {"reason", "unknown_request"}});
}

TEST_CASE("same-state auth update supersedes the waiter", "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    CHECK(session.supersede(4, 10));
    CHECK(future.get().status == daemon::ChallengeStatus::Superseded);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::StaleIgnored);
}

TEST_CASE("disconnect releases a prompt waiter and exposes orphan state", "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    std::atomic<daemon::InFlightState> observed{daemon::InFlightState::None};
    session.set_in_flight_hook([&observed](daemon::InFlightState state) { observed.store(state); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    wait_challenge(captured);
    session.disconnect();
    CHECK(future.get().status == daemon::ChallengeStatus::Disconnected);
    CHECK(session.in_flight_state() == daemon::InFlightState::None);
    CHECK(observed.load() == daemon::InFlightState::None);
}

TEST_CASE("disconnect after answer preserves an orphaned in-flight query", "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    CHECK(future.get().status == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight());
    session.disconnect();
    CHECK(session.in_flight_state() == daemon::InFlightState::Orphaned);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::RequestTerminated);
    session.settle_in_flight();
    CHECK(session.in_flight_state() == daemon::InFlightState::None);
}

TEST_CASE("disconnect after acceptance but before query claim prevents the send",
          "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto future =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto emitted = wait_challenge(captured);
    CHECK(session.receive_answer(answer(emitted)) == daemon::AnswerDisposition::Accepted);
    session.disconnect();
    CHECK(future.get().status == daemon::ChallengeStatus::Answered);
    CHECK_FALSE(session.reserve_in_flight());
    CHECK(session.in_flight_state() == daemon::InFlightState::None);
}

TEST_CASE("disconnect between challenges releases only the current waiter",
          "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(), sink, 17, [] { return std::string(kNonce); });
    auto first =
        std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
    const auto first_frame = wait_challenge(captured);
    CHECK(session.receive_answer(answer(first_frame)) == daemon::AnswerDisposition::Accepted);
    CHECK(first.get().status == daemon::ChallengeStatus::Answered);
    REQUIRE(session.reserve_in_flight());
    session.settle_in_flight();

    {
        const std::lock_guard lock(captured.mutex);
        captured.challenge.reset();
    }
    auto second =
        std::async(std::launch::async, [&session] { return session.challenge(challenge(4, 10)); });
    wait_challenge(captured);
    session.disconnect();
    CHECK(second.get().status == daemon::ChallengeStatus::Disconnected);
}

TEST_CASE("deadline and answer acceptance have one serialized winner", "[challenge][session]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        Captured captured;
        auto sink = make_sink(captured);
        daemon::RequestSession session(request(true, 0.003), sink, 17,
                                       [] { return std::string(kNonce); });
        auto future =
            std::async(std::launch::async, [&session] { return session.challenge(challenge()); });
        const auto emitted = wait_challenge(captured);
        std::this_thread::sleep_for(std::chrono::milliseconds(iteration % 5));
        const auto disposition = session.receive_answer(answer(emitted));
        const auto status = future.get().status;
        CHECK(((disposition == daemon::AnswerDisposition::Accepted &&
                status == daemon::ChallengeStatus::Answered) ||
               (disposition == daemon::AnswerDisposition::RequestTerminated &&
                status == daemon::ChallengeStatus::TimedOut)));
    }
}

TEST_CASE("non-TTY sessions never emit a challenge", "[challenge][session]") {
    Captured captured;
    auto sink = make_sink(captured);
    daemon::RequestSession session(request(false), sink, 17, [] { return std::string(kNonce); });
    CHECK(session.challenge(challenge()).status == daemon::ChallengeStatus::NoTty);
    CHECK_FALSE(captured.challenge.has_value());
}

TEST_CASE("in-process dispatch uses the same challenge session", "[challenge][dispatch]") {
    daemon::Dispatcher dispatcher;
    dispatcher.register_command(
        "challenge-test",
        {daemon::Tier::Read, [](const proto::Request&, daemon::RequestSession& session) {
             const auto outcome = session.challenge(challenge());
             if (outcome.status == daemon::ChallengeStatus::Answered) {
                 if (!session.reserve_in_flight()) {
                     return;
                 }
                 session.settle_in_flight();
                 session.result({{"value", std::get<std::string>(outcome.value)}});
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
