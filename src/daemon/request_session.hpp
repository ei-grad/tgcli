#pragma once

#include "daemon/dispatch.hpp"
#include "proto/frame.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>

namespace tgcli::daemon {

struct ChallengeSpec {
    proto::ChallengeKind kind;
    std::optional<std::uint64_t> client_generation;
    std::optional<std::uint64_t> auth_sequence;
    std::string prompt;
    nlohmann::json details = nlohmann::json::object();
    bool reserves_query = true;
};

enum class ChallengeStatus {
    Answered,
    Cancelled,
    Superseded,
    Disconnected,
    Shutdown,
    TimedOut,
    NoTty,
    ProtocolError,
};

struct ChallengeOutcome {
    ChallengeStatus status;
    std::variant<std::monostate, std::string, bool> value;
};

enum class AnswerDisposition {
    Accepted,
    Cancelled,
    DuplicateIgnored,
    StaleIgnored,
    Rejected,
    RequestTerminated,
};

enum class InFlightState { None, InFlight, Orphaned };

class RequestSession final : public ResponseSink {
  public:
    using Clock = std::chrono::steady_clock;
    using NonceGenerator = std::function<std::string()>;
    using InFlightHook = std::function<void(InFlightState)>;

    RequestSession(proto::Request request, ResponseSink& transport, std::uint64_t connection_id = 0,
                   NonceGenerator nonce_generator = {});
    RequestSession(proto::Request request, std::shared_ptr<ResponseSink> transport,
                   std::uint64_t connection_id = 0, NonceGenerator nonce_generator = {});

    [[nodiscard]] const proto::Request& request() const;
    [[nodiscard]] std::uint64_t connection_id() const;
    [[nodiscard]] Clock::time_point deadline() const;
    [[nodiscard]] std::stop_token cancellation_token() const;
    [[nodiscard]] bool cancellation_requested() const;

    ChallengeOutcome challenge(ChallengeSpec spec);
    AnswerDisposition receive_answer(const proto::Answer& answer);
    bool supersede(std::uint64_t client_generation, std::uint64_t auth_sequence);

    void disconnect();
    void shutdown();

    bool reserve_in_flight();
    void settle_in_flight();
    [[nodiscard]] InFlightState in_flight_state() const;
    void set_in_flight_hook(InFlightHook hook);

  private:
    enum class State { Running, Disconnected, Shutdown, TimedOut, ProtocolError };

    struct Identity {
        std::uint64_t request_id = 0;
        std::string nonce;
        std::uint64_t sequence = 0;
        std::optional<std::uint64_t> client_generation;
        std::optional<std::uint64_t> auth_sequence;
    };

    struct CurrentChallenge {
        proto::ChallengeKind kind;
        Identity identity;
        nlohmann::json payload;
        bool reserves_query = true;
    };

    struct Resolution {
        std::uint64_t sequence = 0;
        ChallengeOutcome outcome{ChallengeStatus::ProtocolError, std::monostate{}};
    };

    static std::string secure_nonce();
    static Clock::time_point compute_deadline(const proto::Request& request);
    void remember_consumed(const Identity& identity);
    [[nodiscard]] bool exact_consumed(const proto::Answer& answer) const;
    void resolve_current(ChallengeOutcome outcome);
    static void notify_in_flight(InFlightState state, const InFlightHook& hook);

    void emit_item(nlohmann::json data) override;
    void emit_progress(nlohmann::json data) override;
    void emit_result(nlohmann::json data) override;
    void emit_error(std::string code, std::string message, nlohmann::json details,
                    int exit_code) override;
    ChallengeReply emit_challenge(nlohmann::json data) override;

    proto::Request request_;
    std::shared_ptr<ResponseSink> transport_owner_;
    ResponseSink* transport_;
    std::uint64_t connection_id_;
    Clock::time_point deadline_;
    NonceGenerator nonce_generator_;

    mutable std::mutex session_mutex_;
    std::condition_variable challenge_cv_;
    State state_ = State::Running;
    std::uint64_t next_sequence_ = 1;
    std::optional<CurrentChallenge> current_;
    std::optional<Resolution> resolution_;
    std::deque<Identity> consumed_;
    bool answer_reserved_ = false;
    std::optional<Identity> reserved_identity_;
    InFlightState in_flight_state_ = InFlightState::None;
    InFlightHook in_flight_hook_;
    std::stop_source cancellation_source_;
};

} // namespace tgcli::daemon
