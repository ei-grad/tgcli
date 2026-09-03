#pragma once

#include "common/cancellation.hpp"
#include "daemon/activity_tracker.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/stream_subscription.hpp"
#include "proto/frame.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
enum class TerminalBatchStatus { Designated, Disconnected, Shutdown, TimedOut, ProtocolError };
enum class ConfigAdmissionMode { DirectFallback, FrozenRuntime };

namespace testing {

enum class RequestSessionProbePoint : std::uint8_t {
    BeforeProtocolTerminalRoute,
    AfterProtocolTerminalRoute,
    BeforePublicTerminalBit,
    BetweenTerminalBatchFrames
};
using RequestSessionProbeHook = void (*)(void*, RequestSessionProbePoint) noexcept;
class RequestSessionTestAccess;

} // namespace testing

class RequestSession final : public ResponseSink {
  public:
    using Clock = std::chrono::steady_clock;
    using WallClock = std::chrono::system_clock;
    using NonceGenerator = std::function<std::string()>;
    using InFlightHook = std::function<void(InFlightState)>;
    using ChallengeReturnHook = std::function<void(ChallengeStatus)>;

    RequestSession(proto::Request request, ResponseSink& transport, std::uint64_t connection_id = 0,
                   NonceGenerator nonce_generator = {},
                   ActivityTracker::Token request_activity = {},
                   std::shared_ptr<const AdmittedAccountConfig> admitted_config = {},
                   std::optional<RequestDeadline> admission_deadline = {},
                   ConfigAdmissionMode config_admission_mode = ConfigAdmissionMode::DirectFallback,
                   std::optional<WallClock::time_point> admission_wall_time = {});
    RequestSession(proto::Request request, std::shared_ptr<ResponseSink> transport,
                   std::uint64_t connection_id = 0, NonceGenerator nonce_generator = {},
                   ActivityTracker::Token request_activity = {},
                   std::shared_ptr<const AdmittedAccountConfig> admitted_config = {},
                   std::optional<RequestDeadline> admission_deadline = {},
                   ConfigAdmissionMode config_admission_mode = ConfigAdmissionMode::DirectFallback,
                   std::optional<WallClock::time_point> admission_wall_time = {});
    ~RequestSession() noexcept override;
    RequestSession(const RequestSession&) = delete;
    RequestSession& operator=(const RequestSession&) = delete;
    RequestSession(RequestSession&&) = delete;
    RequestSession& operator=(RequestSession&&) = delete;

    [[nodiscard]] const proto::Request& request() const;
    [[nodiscard]] std::uint64_t request_source_bytes() const;
    [[nodiscard]] std::uint64_t connection_id() const;
    [[nodiscard]] const RequestDeadline& deadline() const;
    [[nodiscard]] WallClock::time_point admission_wall_time() const;
    [[nodiscard]] cancellation::Token cancellation_token() const;
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
    TerminalBatchStatus begin_terminal_batch();
    DeliveryOutcome complete_terminal_batch(nlohmann::json progress, nlohmann::json result);
    DeliveryOutcome fail_terminal_batch(std::string code, std::string message,
                                        nlohmann::json details, int exit_code);
    [[nodiscard]] StreamSubscriptionActivationResult activate_stream_subscription(
        const std::shared_ptr<StreamIngressHub>& hub, const StreamIngressRequest& request,
        StreamActivityMode activity_mode, testing::StreamActivationProbe probe = {});
    [[nodiscard]] std::optional<StreamActivationProjection> stream_activation_projection() const;

    bool reserve_in_flight();
    bool reserve_direct_in_flight();
    void settle_in_flight();
    [[nodiscard]] InFlightState in_flight_state() const;
    void set_in_flight_hook(InFlightHook hook);
    void set_challenge_return_hook(ChallengeReturnHook hook);

  private:
    enum class State { Running, Disconnected, Shutdown, TimedOut, ProtocolError, AuditFatal };
    enum class ActivityState {
        OpenRequest,
        ProtocolTerminating,
        OpenLegacySubscription,
        OpenStreamSubscription,
        TerminalForwarding,
        Released
    };

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
    [[nodiscard]] bool begin_protocol_terminal_forwarding();
    void finish_terminal_forwarding();
    void release_activity();
    void cancel_stream_transport() noexcept;
    [[nodiscard]] bool claim_stream_terminal(StreamTerminalPayload payload) noexcept;
    [[nodiscard]] StreamSubscriptionWorker stream_worker() const;
    [[nodiscard]] bool route_protocol_terminal(StreamTerminalPayload payload) noexcept;
    DeliveryOutcome forward_protocol_fallback_error(std::string code, std::string message,
                                                    nlohmann::json details, int exit_code);
    void notify_probe(testing::RequestSessionProbePoint point) const noexcept;

    DeliveryOutcome emit_item(nlohmann::json data) override;
    void emit_progress(nlohmann::json data) override;
    DeliveryOutcome emit_result(nlohmann::json data) override;
    DeliveryOutcome emit_raw_result(secure::SensitiveString canonical) override;
    DeliveryOutcome emit_error(std::string code, std::string message, nlohmann::json details,
                               int exit_code) override;
    ChallengeReply emit_challenge(nlohmann::json data) override;
    void emit_abort() noexcept override;
    void before_direct_terminal_bit() noexcept override;
    void between_terminal_batch_frames() noexcept override;
    [[nodiscard]] bool claim_public_terminal() override;
    [[nodiscard]] bool claim_stream_forward_terminal() override;

    proto::Request request_;
    std::shared_ptr<ResponseSink> transport_owner_;
    ResponseSink* transport_;
    std::uint64_t connection_id_;
    RequestDeadline deadline_;
    WallClock::time_point admission_wall_time_;
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
    cancellation::Source cancellation_source_;
    bool audited_terminal_ = false;
    bool terminal_batch_claimed_ = false;
    bool terminal_batch_finished_ = false;
    std::optional<AuditedTerminalStatus> audited_terminal_event_;
    std::optional<Clock::time_point> disconnected_at_;
    std::optional<Clock::time_point> shutdown_at_;
    bool shutdown_requested_ = false;

    mutable std::mutex activity_mutex_;
    ActivityTracker::Token activity_;
    ActivityState activity_state_ = ActivityState::OpenRequest;
    std::optional<StreamSubscriptionLease> stream_subscription_;
    void* probe_context_ = nullptr;
    testing::RequestSessionProbeHook probe_hook_ = nullptr;

    friend class detail::StreamDeliveryRunner;
    friend class StreamCoordinator;
    friend class testing::RequestSessionTestAccess;
    friend StreamDeliveryStatus run_stream_delivery(RequestSession& session,
                                                    const StreamDeliveryOptions& options);
    friend StreamDeliveryStatus
    run_stream_match_delivery(RequestSession& session, const StreamMatchDeliveryOptions& options);
};

namespace testing {

class RequestSessionTestAccess {
  public:
    static void install_probe(RequestSession& session, void* context,
                              RequestSessionProbeHook hook) noexcept {
        session.probe_context_ = context;
        session.probe_hook_ = hook;
    }

    static void expire_deadline(RequestSession& session) noexcept {
        const std::lock_guard lock(session.session_mutex_);
        session.deadline_ = RequestDeadline{RequestSession::Clock::time_point::min()};
    }

    static StreamSubscriptionWorker stream_worker(const RequestSession& session) {
        return session.stream_worker();
    }

    static bool has_stream_subscription(const RequestSession& session) {
        const std::lock_guard lock(session.activity_mutex_);
        return session.activity_state_ == RequestSession::ActivityState::OpenStreamSubscription &&
               session.stream_subscription_.has_value();
    }
};

} // namespace testing

} // namespace tgcli::daemon
