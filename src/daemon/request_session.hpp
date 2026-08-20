#pragma once

#include "daemon/activity_tracker.hpp"
#include "daemon/config_runtime.hpp"
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
#include <utility>
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
    ChallengeOutcome(ChallengeStatus status_value, std::monostate value);
    ChallengeOutcome(ChallengeStatus status_value, bool value);
    ChallengeOutcome(ChallengeStatus status_value, std::string& value,
                     secure::WipeObserver wipe_observer = {});
    ~ChallengeOutcome();
    ChallengeOutcome(const ChallengeOutcome&) = delete;
    ChallengeOutcome& operator=(const ChallengeOutcome&) = delete;
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    ChallengeOutcome(ChallengeOutcome&& other);
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    ChallengeOutcome& operator=(ChallengeOutcome&& other);

    [[nodiscard]] bool take_string(std::string& output);
    [[nodiscard]] std::optional<bool> take_boolean();
    [[nodiscard]] ChallengeStatus status() const;

  private:
    ChallengeStatus status_;
    std::variant<std::monostate, std::string, bool> value_;
    secure::WipeObserver wipe_observer_;
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
enum class AuditedDispatchStatus { Dispatched, Disconnected, Shutdown, TimedOut, ProtocolError };
enum class AuditedTerminalStatus { Designated, Disconnected, Shutdown, TimedOut, ProtocolError };
enum class ConfigAdmissionMode { DirectFallback, FrozenRuntime };

class RequestSession final : public ResponseSink {
  public:
    using Clock = std::chrono::steady_clock;
    using NonceGenerator = std::function<std::string()>;
    using InFlightHook = std::function<void(InFlightState)>;
    using ChallengeReturnHook = std::function<void(ChallengeStatus)>;

    RequestSession(proto::Request request, ResponseSink& transport, std::uint64_t connection_id = 0,
                   NonceGenerator nonce_generator = {},
                   ActivityTracker::Token request_activity = {},
                   std::shared_ptr<const AdmittedAccountConfig> admitted_config = {},
                   std::optional<RequestDeadline> admission_deadline = {},
                   ConfigAdmissionMode config_admission_mode = ConfigAdmissionMode::DirectFallback);
    RequestSession(proto::Request request, std::shared_ptr<ResponseSink> transport,
                   std::uint64_t connection_id = 0, NonceGenerator nonce_generator = {},
                   ActivityTracker::Token request_activity = {},
                   std::shared_ptr<const AdmittedAccountConfig> admitted_config = {},
                   std::optional<RequestDeadline> admission_deadline = {},
                   ConfigAdmissionMode config_admission_mode = ConfigAdmissionMode::DirectFallback);

    [[nodiscard]] const proto::Request& request() const;
    [[nodiscard]] std::uint64_t connection_id() const;
    [[nodiscard]] const RequestDeadline& deadline() const;
    [[nodiscard]] std::stop_token cancellation_token() const;
    [[nodiscard]] bool cancellation_requested() const;
    [[nodiscard]] bool shutdown_requested() const;
    [[nodiscard]] const std::shared_ptr<const AdmittedAccountConfig>& admitted_config() const;
    [[nodiscard]] ConfigAdmissionMode config_admission_mode() const;

    ChallengeOutcome challenge(ChallengeSpec spec);
    AnswerDisposition receive_answer(proto::Answer answer);
    bool supersede(std::uint64_t client_generation, std::uint64_t auth_sequence);

    void disconnect();
    void shutdown();
    AuditedTerminalStatus begin_audited_terminal();
    AuditedTerminalStatus claim_audited_terminal_event(Clock::time_point committed_at);
    void audit_fatal();
    AuditedDispatchStatus dispatch_audited(const std::function<void()>& dispatch);
    [[nodiscard]] bool promote_to_subscription();

    bool reserve_in_flight();
    bool reserve_direct_in_flight();
    void settle_in_flight();
    [[nodiscard]] InFlightState in_flight_state() const;
    void set_in_flight_hook(InFlightHook hook);
    void set_challenge_return_hook(ChallengeReturnHook hook);

  private:
    enum class State { Running, Disconnected, Shutdown, TimedOut, ProtocolError, AuditFatal };
    enum class ActivityState { Active, TerminalForwarding, Released };

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

    struct Resolution { // NOLINT(bugprone-exception-escape)
        std::uint64_t sequence = 0;
        ChallengeOutcome outcome{ChallengeStatus::ProtocolError, std::monostate{}};
    };

    static std::string secure_nonce();
    static RequestDeadline compute_deadline(const proto::Request& request);
    void remember_consumed(const Identity& identity);
    [[nodiscard]] bool exact_consumed(const proto::Answer& answer) const;
    void resolve_current(ChallengeOutcome outcome);
    static void notify_in_flight(InFlightState state, const InFlightHook& hook);
    [[nodiscard]] bool begin_terminal_forwarding();
    void finish_terminal_forwarding();
    void release_activity();

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
    RequestDeadline deadline_;
    NonceGenerator nonce_generator_;
    std::shared_ptr<const AdmittedAccountConfig> admitted_config_;
    ConfigAdmissionMode config_admission_mode_;

    mutable std::mutex session_mutex_;
    std::condition_variable challenge_cv_;
    State state_ = State::Running;
    std::uint64_t next_sequence_ = 1;
    std::optional<CurrentChallenge> current_;
    std::optional<Resolution> resolution_;
    std::deque<Identity> consumed_;
    bool answer_reserved_ = false;
    std::optional<Identity> reserved_identity_;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> latest_auth_identity_;
    InFlightState in_flight_state_ = InFlightState::None;
    InFlightHook in_flight_hook_;
    ChallengeReturnHook challenge_return_hook_;
    std::stop_source cancellation_source_;
    bool audited_terminal_ = false;
    std::optional<AuditedTerminalStatus> audited_terminal_event_;
    std::optional<Clock::time_point> disconnected_at_;
    std::optional<Clock::time_point> shutdown_at_;
    bool shutdown_requested_ = false;

    std::mutex activity_mutex_;
    ActivityTracker::Token activity_;
    ActivityState activity_state_ = ActivityState::Active;
};

} // namespace tgcli::daemon
