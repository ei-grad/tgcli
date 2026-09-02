#include "daemon/request_session.hpp"

#include "common/exit_codes.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace tgcli::daemon {

namespace {

proto::Request freeze_request(proto::Request&& request) {
    const proto::Request source = std::move(request);
    std::string error;
    auto frozen = proto::admit_request_source(source, error);
    if (!frozen) {
        throw std::invalid_argument(error);
    }
    return std::move(*frozen);
}

constexpr std::size_t kConsumedChallengeLimit = 16;

std::optional<std::uint64_t> optional_uint(const nlohmann::json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    return static_cast<std::uint64_t>(value.get<std::int64_t>());
}

enum class IdentityOrder { Stale, Equal, Future };

IdentityOrder compare_identity(std::optional<std::uint64_t> current_generation,
                               std::optional<std::uint64_t> current_auth_sequence,
                               const proto::Answer& answer) {
    const auto generation = optional_uint(answer.answer["client_generation"]);
    const auto auth_sequence = optional_uint(answer.answer["auth_sequence"]);
    if (generation != current_generation) {
        if (generation && current_generation && *generation < *current_generation) {
            return IdentityOrder::Stale;
        }
        return IdentityOrder::Future;
    }
    if (auth_sequence == current_auth_sequence) {
        return IdentityOrder::Equal;
    }
    if (auth_sequence && current_auth_sequence && *auth_sequence < *current_auth_sequence) {
        return IdentityOrder::Stale;
    }
    return IdentityOrder::Future;
}

} // namespace

ChallengeOutcome::ChallengeOutcome(ChallengeStatus status_value, std::monostate value)
    : status_(status_value), value_(value) {}

ChallengeOutcome::ChallengeOutcome(ChallengeStatus status_value, bool value)
    : status_(status_value), value_(value) {}

ChallengeOutcome::ChallengeOutcome(ChallengeStatus status_value, std::string& value,
                                   secure::WipeObserver wipe_observer)
    : status_(status_value), wipe_observer_(std::move(wipe_observer)) {
    value_.emplace<std::string>(value);
    secure::wipe(value, wipe_observer_, "challenge_value_source");
}

ChallengeOutcome::~ChallengeOutcome() {
    if (auto* retained = std::get_if<std::string>(&value_)) {
        secure::wipe(*retained, wipe_observer_, "challenge_outcome");
    }
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,cppcoreguidelines-prefer-member-initializer,performance-noexcept-move-constructor)
ChallengeOutcome::ChallengeOutcome(ChallengeOutcome&& other) : status_(other.status_) {
    if (auto* retained = std::get_if<std::string>(&other.value_)) {
        value_.emplace<std::string>(*retained);
        secure::wipe(*retained, other.wipe_observer_, "challenge_outcome_move_source");
        other.value_ = std::monostate{};
    } else {
        value_ = other.value_;
    }
    wipe_observer_ = std::move(other.wipe_observer_);
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
ChallengeOutcome& ChallengeOutcome::operator=(ChallengeOutcome&& other) {
    if (this != &other) {
        if (auto* retained = std::get_if<std::string>(&value_)) {
            secure::wipe(*retained, wipe_observer_, "challenge_outcome");
        }
        status_ = other.status_;
        if (auto* retained = std::get_if<std::string>(&other.value_)) {
            value_.emplace<std::string>(*retained);
            secure::wipe(*retained, other.wipe_observer_, "challenge_outcome_move_source");
            other.value_ = std::monostate{};
        } else {
            value_ = other.value_;
        }
        wipe_observer_ = std::move(other.wipe_observer_);
    }
    return *this;
}

bool ChallengeOutcome::take_string(std::string& output) {
    auto* retained = std::get_if<std::string>(&value_);
    if (retained == nullptr) {
        return false;
    }
    secure::transfer(*retained, output, wipe_observer_, "challenge_take_source");
    value_ = std::monostate{};
    return true;
}

std::optional<bool> ChallengeOutcome::take_boolean() {
    const auto* retained = std::get_if<bool>(&value_);
    if (retained == nullptr) {
        return std::nullopt;
    }
    const bool result = *retained;
    value_ = std::monostate{};
    return result;
}

ChallengeStatus ChallengeOutcome::status() const {
    return status_;
}

RequestSession::RequestSession(proto::Request request, ResponseSink& transport,
                               std::uint64_t connection_id, NonceGenerator nonce_generator,
                               ActivityTracker::Token request_activity,
                               std::shared_ptr<const AdmittedAccountConfig> admitted_config,
                               std::optional<RequestDeadline> admission_deadline,
                               ConfigAdmissionMode config_admission_mode,
                               std::optional<WallClock::time_point> admission_wall_time)
    : request_(freeze_request(std::move(request))), transport_(&transport),
      connection_id_(connection_id),
      deadline_(admission_deadline ? *admission_deadline : compute_deadline(request_)),
      admission_wall_time_(admission_wall_time ? admission_wall_time.value() : WallClock::now()),
      nonce_generator_(nonce_generator ? std::move(nonce_generator) : secure_nonce),
      admitted_config_(std::move(admitted_config)), config_admission_mode_(config_admission_mode),
      activity_(std::move(request_activity)) {}

RequestSession::RequestSession(proto::Request request, std::shared_ptr<ResponseSink> transport,
                               std::uint64_t connection_id, NonceGenerator nonce_generator,
                               ActivityTracker::Token request_activity,
                               std::shared_ptr<const AdmittedAccountConfig> admitted_config,
                               std::optional<RequestDeadline> admission_deadline,
                               ConfigAdmissionMode config_admission_mode,
                               std::optional<WallClock::time_point> admission_wall_time)
    : request_(freeze_request(std::move(request))), transport_owner_(std::move(transport)),
      transport_(transport_owner_.get()), connection_id_(connection_id),
      deadline_(admission_deadline ? *admission_deadline : compute_deadline(request_)),
      admission_wall_time_(admission_wall_time ? admission_wall_time.value() : WallClock::now()),
      nonce_generator_(nonce_generator ? std::move(nonce_generator) : secure_nonce),
      admitted_config_(std::move(admitted_config)), config_admission_mode_(config_admission_mode),
      activity_(std::move(request_activity)) {
    if (transport_ == nullptr) {
        throw std::invalid_argument("request session transport is null");
    }
}

RequestSession::~RequestSession() noexcept {
    try {
        disconnect();
        std::optional<StreamSubscriptionLease> subscription;
        {
            const std::lock_guard lock(activity_mutex_);
            subscription = std::move(stream_subscription_);
            if (activity_state_ != ActivityState::Released) {
                activity_.reset();
                activity_state_ = ActivityState::Released;
            }
        }
        subscription.reset();
    } catch (...) {
        // Mutex and observer callables expose non-enumerable exception types. Destruction cannot
        // safely recover after lifecycle teardown begins.
        std::terminate();
    }
}

const proto::Request& RequestSession::request() const {
    return request_;
}

std::uint64_t RequestSession::request_source_bytes() const {
    return request_.source_bytes();
}

std::uint64_t RequestSession::connection_id() const {
    return connection_id_;
}

const RequestDeadline& RequestSession::deadline() const {
    return deadline_;
}

RequestSession::WallClock::time_point RequestSession::admission_wall_time() const {
    return admission_wall_time_;
}

cancellation::Token RequestSession::cancellation_token() const {
    return cancellation_source_.get_token();
}

bool RequestSession::cancellation_requested() const {
    return cancellation_source_.stop_requested();
}

bool RequestSession::shutdown_requested() const {
    const std::lock_guard lock(session_mutex_);
    return shutdown_requested_;
}

const std::shared_ptr<const AdmittedAccountConfig>& RequestSession::admitted_config() const {
    return admitted_config_;
}

ConfigAdmissionMode RequestSession::config_admission_mode() const {
    return config_admission_mode_;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ChallengeOutcome RequestSession::challenge(ChallengeSpec spec) {
    if (!request_.context.tty) {
        return {ChallengeStatus::NoTty, std::monostate{}};
    }

    nlohmann::json payload;
    std::uint64_t sequence = 0;
    {
        const std::lock_guard lock(session_mutex_);
        if (state_ != State::Running) {
            switch (state_) {
            case State::Disconnected:
                return {ChallengeStatus::Disconnected, std::monostate{}};
            case State::Shutdown:
                return {ChallengeStatus::Shutdown, std::monostate{}};
            case State::TimedOut:
                return {ChallengeStatus::TimedOut, std::monostate{}};
            case State::ProtocolError:
            case State::AuditFatal:
                return {ChallengeStatus::ProtocolError, std::monostate{}};
            case State::Running:
                break;
            }
        }
        if (deadline_expired(deadline_)) {
            state_ = State::TimedOut;
            return {ChallengeStatus::TimedOut, std::monostate{}};
        }
        if (current_) {
            throw std::logic_error("request already has an active challenge");
        }
        if (spec.client_generation.has_value() != spec.auth_sequence.has_value()) {
            throw std::invalid_argument("challenge generation fields must have matching nullness");
        }
        if (spec.client_generation && latest_auth_identity_ &&
            *latest_auth_identity_ > std::pair{*spec.client_generation, *spec.auth_sequence}) {
            return {ChallengeStatus::Superseded, std::monostate{}};
        }
        if (answer_reserved_) {
            if (!reserved_identity_ ||
                reserved_identity_->client_generation != spec.client_generation ||
                reserved_identity_->auth_sequence != spec.auth_sequence) {
                throw std::logic_error("reserved answer identity does not match continuation");
            }
            answer_reserved_ = false;
            reserved_identity_.reset();
        }
        sequence = next_sequence_++;
        Identity identity{request_.id, nonce_generator_(), sequence, spec.client_generation,
                          spec.auth_sequence};
        payload = {{"kind", proto::challenge_kind_name(spec.kind)},
                   {"nonce", identity.nonce},
                   {"sequence", identity.sequence},
                   {"client_generation", identity.client_generation
                                             ? nlohmann::json(*identity.client_generation)
                                             : nlohmann::json(nullptr)},
                   {"auth_sequence", identity.auth_sequence
                                         ? nlohmann::json(*identity.auth_sequence)
                                         : nlohmann::json(nullptr)},
                   {"secret", proto::challenge_kind_is_secret(spec.kind)},
                   {"prompt", std::move(spec.prompt)},
                   {"details", std::move(spec.details)}};
        std::string error;
        if (!proto::validate_challenge_payload(payload, error)) {
            throw std::invalid_argument(error);
        }
        current_ = CurrentChallenge{spec.kind, std::move(identity), payload, spec.reserves_query};
        resolution_.reset();
    }

    if (auto immediate = transport_->challenge(payload)) {
        receive_answer(proto::Answer{request_.id, std::move(*immediate)});
    } else if (transport_->has_terminal()) {
        const std::lock_guard lock(session_mutex_);
        state_ = State::ProtocolError;
        release_activity();
        resolve_current({ChallengeStatus::ProtocolError, std::monostate{}});
    }

    std::unique_lock lock(session_mutex_);
    while (!resolution_ || resolution_->sequence != sequence) {
        if (deadline_.expires_at) {
            static_cast<void>(challenge_cv_.wait_until(lock, *deadline_.expires_at));
        } else {
            challenge_cv_.wait(lock);
        }
        if (deadline_expired(deadline_) && current_ && current_->identity.sequence == sequence) {
            state_ = State::TimedOut;
            resolve_current({ChallengeStatus::TimedOut, std::monostate{}});
        }
    }
    auto outcome = std::move(resolution_->outcome);
    resolution_.reset();
    const auto return_hook = challenge_return_hook_;
    lock.unlock();
    if (return_hook) {
        return_hook(outcome.status());
    }
    return outcome;
}

// Identity classification, deadline choice, challenge consumption and terminal
// claim must remain one mutex-serialized decision tree.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
AnswerDisposition RequestSession::receive_answer(proto::Answer answer) {
    std::optional<StreamProtocolAnswerInvalidReason> rejection;
    {
        const std::lock_guard lock(session_mutex_);
        if (state_ != State::Running) {
            return AnswerDisposition::RequestTerminated;
        }
        if (deadline_expired(deadline_)) {
            state_ = State::TimedOut;
            if (current_) {
                secure::wipe(answer.answer, answer.wipe_observer(), "answer_payload");
                resolve_current({ChallengeStatus::TimedOut, std::monostate{}});
            }
            return AnswerDisposition::RequestTerminated;
        }
        std::string validation_error;
        if (!proto::validate_answer_payload(answer.answer, validation_error)) {
            rejection = StreamProtocolAnswerInvalidReason::Malformed;
        } else if (answer.id != request_.id) {
            rejection = StreamProtocolAnswerInvalidReason::UnknownRequest;
        } else if (exact_consumed(answer)) {
            return AnswerDisposition::DuplicateIgnored;
        } else if (!current_) {
            const auto sequence = answer.answer["sequence"].get<std::uint64_t>();
            if (sequence < next_sequence_) {
                return AnswerDisposition::StaleIgnored;
            }
            rejection = StreamProtocolAnswerInvalidReason::FutureSequence;
        } else {
            const auto sequence = answer.answer["sequence"].get<std::uint64_t>();
            if (sequence < current_->identity.sequence) {
                return AnswerDisposition::StaleIgnored;
            }
            if (sequence > current_->identity.sequence) {
                rejection = StreamProtocolAnswerInvalidReason::FutureSequence;
            } else if (compare_identity(current_->identity.client_generation,
                                        current_->identity.auth_sequence,
                                        answer) == IdentityOrder::Stale) {
                return AnswerDisposition::StaleIgnored;
            } else if (compare_identity(current_->identity.client_generation,
                                        current_->identity.auth_sequence,
                                        answer) == IdentityOrder::Future) {
                rejection = StreamProtocolAnswerInvalidReason::GenerationMismatch;
            } else if (answer.answer["nonce"] != current_->identity.nonce) {
                rejection = StreamProtocolAnswerInvalidReason::NonceMismatch;
            } else if (answer.answer.contains("value") &&
                       proto::challenge_kind_expects_boolean(current_->kind) !=
                           answer.answer["value"].is_boolean()) {
                rejection = StreamProtocolAnswerInvalidReason::Malformed;
            } else {
                remember_consumed(current_->identity);
                if (answer.answer.contains("cancelled")) {
                    secure::wipe(answer.answer, answer.wipe_observer(), "answer_payload");
                    resolve_current({ChallengeStatus::Cancelled, std::monostate{}});
                    return AnswerDisposition::Cancelled;
                }
                if (current_->reserves_query) {
                    answer_reserved_ = true;
                    reserved_identity_ = current_->identity;
                }
                ChallengeOutcome outcome{ChallengeStatus::Answered, std::monostate{}};
                if (answer.answer["value"].is_boolean()) {
                    outcome = ChallengeOutcome{ChallengeStatus::Answered,
                                               answer.answer["value"].get<bool>()};
                } else {
                    auto& value = answer.answer["value"].get_ref<std::string&>();
                    outcome =
                        ChallengeOutcome{ChallengeStatus::Answered, value, answer.wipe_observer()};
                }
                secure::wipe(answer.answer, answer.wipe_observer(), "answer_payload");
                resolve_current(std::move(outcome));
                return AnswerDisposition::Accepted;
            }
        }
        state_ = State::ProtocolError;
        secure::wipe(answer.answer, answer.wipe_observer(), "answer_payload");
        const auto reason = rejection.value_or(StreamProtocolAnswerInvalidReason::Malformed);
        notify_probe(testing::RequestSessionProbePoint::BeforeProtocolTerminalRoute);
        const bool routed =
            route_protocol_terminal({.cause = StreamTerminalCause::ProtocolAnswerInvalid,
                                     .protocol_request_id = answer.id,
                                     .protocol_reason = reason,
                                     .metadata_failure = {}});
        notify_probe(testing::RequestSessionProbePoint::AfterProtocolTerminalRoute);
        if (!routed) {
            static_cast<void>(forward_protocol_fallback_error(
                "PROTOCOL_ANSWER_INVALID", "invalid challenge answer",
                {{"request_id", answer.id},
                 {"reason", std::string(stream_protocol_answer_invalid_reason_name(reason))}},
                kUsage));
        }
        if (current_) {
            resolve_current({ChallengeStatus::ProtocolError, std::monostate{}});
        }
    }
    return AnswerDisposition::Rejected;
}

bool RequestSession::supersede(std::uint64_t client_generation, std::uint64_t auth_sequence) {
    const std::lock_guard lock(session_mutex_);
    if (state_ != State::Running) {
        return false;
    }
    const auto observed = std::pair{client_generation, auth_sequence};
    if (!latest_auth_identity_ || observed > *latest_auth_identity_) {
        latest_auth_identity_ = observed;
    }
    const Identity* identity = nullptr;
    if (current_) {
        identity = &current_->identity;
    } else if (answer_reserved_ && reserved_identity_) {
        identity = &*reserved_identity_;
    }
    if (identity == nullptr || !identity->client_generation || !identity->auth_sequence ||
        client_generation < *identity->client_generation ||
        (client_generation == *identity->client_generation &&
         auth_sequence <= *identity->auth_sequence)) {
        return false;
    }
    if (current_) {
        resolve_current({ChallengeStatus::Superseded, std::monostate{}});
    } else {
        answer_reserved_ = false;
        reserved_identity_.reset();
    }
    return true;
}

void RequestSession::disconnect() {
    InFlightHook hook;
    InFlightState state = InFlightState::None;
    {
        const std::lock_guard lock(session_mutex_);
        if (state_ != State::Running && state_ != State::TimedOut) {
            return;
        }
        disconnected_at_ = Clock::now();
        if (terminal_batch_claimed_) {
            static_cast<void>(cancellation_source_.request_stop());
            return;
        }
        state_ = State::Disconnected;
        static_cast<void>(cancellation_source_.request_stop());
        answer_reserved_ = false;
        reserved_identity_.reset();
        if (current_) {
            resolve_current({ChallengeStatus::Disconnected, std::monostate{}});
        }
        if (in_flight_state_ == InFlightState::InFlight) {
            in_flight_state_ = InFlightState::Orphaned;
            state = in_flight_state_;
            hook = in_flight_hook_;
        }
    }
    if (!claim_stream_terminal(
            {.cause = StreamTerminalCause::Disconnected, .metadata_failure = {}})) {
        release_activity();
    }
    notify_in_flight(state, hook);
}

void RequestSession::shutdown() {
    bool audited = false;
    {
        const std::lock_guard lock(session_mutex_);
        if (state_ != State::Running) {
            return;
        }
        if (!shutdown_at_) {
            shutdown_at_ = Clock::now();
        }
        shutdown_requested_ = true;
        audited = audited_terminal_ || terminal_batch_claimed_;
        static_cast<void>(cancellation_source_.request_stop());
        answer_reserved_ = false;
        reserved_identity_.reset();
        if (!audited) {
            state_ = State::Shutdown;
        }
        if (current_) {
            resolve_current({ChallengeStatus::Shutdown, std::monostate{}});
        }
    }
    if (audited) {
        return;
    }
    if (claim_stream_terminal({.cause = StreamTerminalCause::Shutdown, .metadata_failure = {}})) {
        return;
    }
    static_cast<void>(error("DAEMON_SHUTDOWN", "daemon is shutting down",
                            {{"reason", "daemon_shutdown"}}, kGeneric));
}

TerminalBatchStatus RequestSession::begin_terminal_batch() {
    const std::lock_guard lock(session_mutex_);
    if (shutdown_requested_) {
        return TerminalBatchStatus::Shutdown;
    }
    switch (state_) {
    case State::Running:
        if (deadline_expired(deadline_)) {
            state_ = State::TimedOut;
            return TerminalBatchStatus::TimedOut;
        }
        if (!static_cast<ResponseSink&>(*this).reserve_terminal_batch()) {
            return TerminalBatchStatus::ProtocolError;
        }
        terminal_batch_claimed_ = true;
        return TerminalBatchStatus::Designated;
    case State::Disconnected:
        return TerminalBatchStatus::Disconnected;
    case State::Shutdown:
        return TerminalBatchStatus::Shutdown;
    case State::TimedOut:
        return TerminalBatchStatus::TimedOut;
    case State::ProtocolError:
    case State::AuditFatal:
        return TerminalBatchStatus::ProtocolError;
    }
    return TerminalBatchStatus::ProtocolError;
}

DeliveryOutcome RequestSession::complete_terminal_batch(nlohmann::json progress,
                                                        nlohmann::json result) {
    {
        const std::lock_guard lock(session_mutex_);
        if (!terminal_batch_claimed_ || terminal_batch_finished_) {
            return DeliveryOutcome::Suppressed;
        }
        terminal_batch_finished_ = true;
    }
    return static_cast<ResponseSink&>(*this).finish_terminal_batch(std::move(progress),
                                                                   std::move(result));
}

DeliveryOutcome RequestSession::fail_terminal_batch(std::string code, std::string message,
                                                    nlohmann::json details, int exit_code) {
    {
        const std::lock_guard lock(session_mutex_);
        if (!terminal_batch_claimed_ || terminal_batch_finished_) {
            return DeliveryOutcome::Suppressed;
        }
        terminal_batch_finished_ = true;
    }
    return static_cast<ResponseSink&>(*this).fail_terminal_batch(
        std::move(code), std::move(message), std::move(details), exit_code);
}

AuditedTerminalStatus RequestSession::begin_audited_terminal() {
    const std::lock_guard lock(session_mutex_);
    if (shutdown_requested_) {
        return AuditedTerminalStatus::Shutdown;
    }
    switch (state_) {
    case State::Running:
        if (deadline_expired(deadline_)) {
            state_ = State::TimedOut;
            return AuditedTerminalStatus::TimedOut;
        }
        audited_terminal_ = true;
        return AuditedTerminalStatus::Designated;
    case State::Disconnected:
        return AuditedTerminalStatus::Disconnected;
    case State::Shutdown:
        return AuditedTerminalStatus::Shutdown;
    case State::TimedOut:
        return AuditedTerminalStatus::TimedOut;
    case State::ProtocolError:
    case State::AuditFatal:
        return AuditedTerminalStatus::ProtocolError;
    }
    return AuditedTerminalStatus::ProtocolError;
}

AuditedTerminalStatus RequestSession::claim_audited_terminal_event(Clock::time_point committed_at) {
    const std::lock_guard lock(session_mutex_);
    if (audited_terminal_event_) {
        return *audited_terminal_event_;
    }
    AuditedTerminalStatus result = AuditedTerminalStatus::ProtocolError;
    if (!audited_terminal_) {
        result = AuditedTerminalStatus::ProtocolError;
    } else {
        auto terminal_at = deadline_.expires_at;
        result = terminal_at ? AuditedTerminalStatus::TimedOut : AuditedTerminalStatus::Designated;
        if (disconnected_at_ && (!terminal_at || *disconnected_at_ < *terminal_at)) {
            terminal_at = disconnected_at_;
            result = AuditedTerminalStatus::Disconnected;
        }
        if (shutdown_at_ && (!terminal_at || *shutdown_at_ < *terminal_at)) {
            terminal_at = shutdown_at_;
            result = AuditedTerminalStatus::Shutdown;
        }
        if (!terminal_at || committed_at < *terminal_at) {
            result = AuditedTerminalStatus::Designated;
        } else if (result == AuditedTerminalStatus::TimedOut) {
            state_ = State::TimedOut;
        }
    }
    if (result != AuditedTerminalStatus::Designated) {
        audited_terminal_event_ = result;
    }
    return result;
}

AuditedDispatchStatus RequestSession::dispatch_audited(const std::function<void()>& dispatch) {
    const std::lock_guard lock(session_mutex_);
    if (!audited_terminal_) {
        return AuditedDispatchStatus::ProtocolError;
    }
    if (shutdown_requested_) {
        return AuditedDispatchStatus::Shutdown;
    }
    switch (state_) {
    case State::Running:
        if (deadline_expired(deadline_)) {
            return AuditedDispatchStatus::TimedOut;
        }
        dispatch();
        return AuditedDispatchStatus::Dispatched;
    case State::Disconnected:
        return AuditedDispatchStatus::Disconnected;
    case State::Shutdown:
        return AuditedDispatchStatus::Shutdown;
    case State::TimedOut:
        return AuditedDispatchStatus::TimedOut;
    case State::ProtocolError:
    case State::AuditFatal:
        return AuditedDispatchStatus::ProtocolError;
    }
    return AuditedDispatchStatus::ProtocolError;
}

void RequestSession::audit_fatal() {
    const std::lock_guard lock(session_mutex_);
    if (state_ == State::Disconnected || state_ == State::AuditFatal) {
        return;
    }
    state_ = State::AuditFatal;
    static_cast<void>(cancellation_source_.request_stop());
    answer_reserved_ = false;
    reserved_identity_.reset();
    release_activity();
    if (current_) {
        resolve_current({ChallengeStatus::ProtocolError, std::monostate{}});
    }
}

bool RequestSession::promote_to_subscription() {
    const std::lock_guard lock(activity_mutex_);
    if (activity_state_ != ActivityState::OpenRequest || !activity_.promote_to_subscription()) {
        return false;
    }
    activity_state_ = ActivityState::OpenLegacySubscription;
    return true;
}

bool RequestSession::reserve_in_flight() {
    InFlightHook hook;
    {
        const std::lock_guard lock(session_mutex_);
        if (state_ != State::Running || !answer_reserved_ ||
            in_flight_state_ != InFlightState::None) {
            return false;
        }
        answer_reserved_ = false;
        reserved_identity_.reset();
        in_flight_state_ = InFlightState::InFlight;
        hook = in_flight_hook_;
    }
    notify_in_flight(InFlightState::InFlight, hook);
    return true;
}

bool RequestSession::reserve_direct_in_flight() {
    InFlightHook hook;
    {
        const std::lock_guard lock(session_mutex_);
        if (state_ != State::Running || current_ || answer_reserved_ ||
            in_flight_state_ != InFlightState::None) {
            return false;
        }
        in_flight_state_ = InFlightState::InFlight;
        hook = in_flight_hook_;
    }
    notify_in_flight(InFlightState::InFlight, hook);
    return true;
}

void RequestSession::settle_in_flight() {
    InFlightHook hook;
    {
        const std::lock_guard lock(session_mutex_);
        if (in_flight_state_ == InFlightState::None) {
            return;
        }
        in_flight_state_ = InFlightState::None;
        hook = in_flight_hook_;
    }
    notify_in_flight(InFlightState::None, hook);
}

InFlightState RequestSession::in_flight_state() const {
    const std::lock_guard lock(session_mutex_);
    return in_flight_state_;
}

void RequestSession::set_in_flight_hook(InFlightHook hook) {
    const std::lock_guard lock(session_mutex_);
    in_flight_hook_ = std::move(hook);
}

void RequestSession::set_challenge_return_hook(ChallengeReturnHook hook) {
    const std::lock_guard lock(session_mutex_);
    challenge_return_hook_ = std::move(hook);
}

std::string RequestSession::secure_nonce() {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("cannot open system random source");
    }
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(fd);
            throw std::runtime_error("cannot read system random source");
        }
        offset += static_cast<std::size_t>(count);
    }
    ::close(fd);
    constexpr std::string_view hex = "0123456789abcdef";
    std::string nonce(32, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        nonce.at(index * 2) = hex[bytes.at(index) >> 4U];
        nonce.at(index * 2 + 1) = hex[bytes.at(index) & 0x0fU];
    }
    return nonce;
}

RequestDeadline RequestSession::compute_deadline(const proto::Request& request) {
    if (const auto deadline =
            request_deadline(request.context.timeout_seconds, DeadlineDefault::Default60)) {
        return *deadline;
    }
    throw std::invalid_argument("request timeout must be finite, positive, and representable");
}

void RequestSession::remember_consumed(const Identity& identity) {
    consumed_.push_back(identity);
    while (consumed_.size() > kConsumedChallengeLimit) {
        consumed_.pop_front();
    }
}

bool RequestSession::exact_consumed(const proto::Answer& answer) const {
    if (!answer.answer.is_object() || !answer.answer.contains("nonce") ||
        !answer.answer.contains("sequence") || !answer.answer.contains("client_generation") ||
        !answer.answer.contains("auth_sequence")) {
        return false;
    }
    return std::any_of(consumed_.begin(), consumed_.end(), [&answer](const Identity& identity) {
        return answer.id == identity.request_id && answer.answer["nonce"] == identity.nonce &&
               answer.answer["sequence"] == identity.sequence &&
               optional_uint(answer.answer["client_generation"]) == identity.client_generation &&
               optional_uint(answer.answer["auth_sequence"]) == identity.auth_sequence;
    });
}

void RequestSession::resolve_current(ChallengeOutcome outcome) {
    if (!current_) {
        return;
    }
    resolution_ = Resolution{current_->identity.sequence, std::move(outcome)};
    current_.reset();
    challenge_cv_.notify_all();
}

void RequestSession::notify_in_flight(InFlightState state, const InFlightHook& hook) {
    if (hook) {
        hook(state);
    }
}

bool RequestSession::claim_public_terminal() {
    const std::lock_guard lock(activity_mutex_);
    if (activity_state_ != ActivityState::OpenRequest &&
        activity_state_ != ActivityState::OpenLegacySubscription) {
        return false;
    }
    activity_state_ = ActivityState::TerminalForwarding;
    return true;
}

bool RequestSession::claim_stream_forward_terminal() {
    const std::lock_guard lock(activity_mutex_);
    if (activity_state_ != ActivityState::OpenStreamSubscription) {
        return false;
    }
    activity_state_ = ActivityState::TerminalForwarding;
    return true;
}

bool RequestSession::begin_protocol_terminal_forwarding() {
    const std::lock_guard lock(activity_mutex_);
    if (activity_state_ != ActivityState::ProtocolTerminating) {
        return false;
    }
    activity_state_ = ActivityState::TerminalForwarding;
    return true;
}

void RequestSession::finish_terminal_forwarding() {
    std::optional<StreamSubscriptionLease> subscription;
    {
        const std::lock_guard lock(activity_mutex_);
        if (activity_state_ == ActivityState::TerminalForwarding) {
            activity_.reset();
            subscription = std::move(stream_subscription_);
            activity_state_ = ActivityState::Released;
        }
    }
    subscription.reset();
}

void RequestSession::release_activity() {
    const std::lock_guard lock(activity_mutex_);
    if (activity_state_ == ActivityState::OpenRequest ||
        activity_state_ == ActivityState::OpenLegacySubscription) {
        activity_.reset();
        activity_state_ = ActivityState::Released;
    }
}

void RequestSession::cancel_stream_transport() noexcept {
    static_cast<void>(cancellation_source_.request_stop());
}

bool RequestSession::claim_stream_terminal(StreamTerminalPayload payload) noexcept {
    std::shared_ptr<detail::StreamSubscriptionState> subscription;
    {
        const std::lock_guard lock(activity_mutex_);
        if ((activity_state_ != ActivityState::OpenStreamSubscription &&
             activity_state_ != ActivityState::TerminalForwarding) ||
            !stream_subscription_ || !stream_subscription_->state_) {
            return false;
        }
        subscription = stream_subscription_->state_;
    }
    if (!subscription) {
        return false;
    }
    static_cast<void>(detail::stream_subscription_claim(subscription, payload));
    return true;
}

bool RequestSession::route_protocol_terminal(StreamTerminalPayload payload) noexcept {
    std::shared_ptr<detail::StreamSubscriptionState> subscription;
    {
        const std::lock_guard lock(activity_mutex_);
        if (activity_state_ == ActivityState::OpenRequest) {
            activity_state_ = ActivityState::ProtocolTerminating;
            return false;
        }
        if ((activity_state_ != ActivityState::OpenStreamSubscription &&
             activity_state_ != ActivityState::TerminalForwarding) ||
            !stream_subscription_ || !stream_subscription_->state_) {
            return false;
        }
        subscription = stream_subscription_->state_;
    }
    static_cast<void>(detail::stream_subscription_claim(subscription, payload));
    return true;
}

DeliveryOutcome RequestSession::forward_protocol_fallback_error(std::string code,
                                                                std::string message,
                                                                nlohmann::json details,
                                                                int exit_code) {
    if (!begin_protocol_terminal_forwarding()) {
        return DeliveryOutcome::Suppressed;
    }
    if (!static_cast<ResponseSink&>(*this).claim_protocol_fallback_terminal()) {
        finish_terminal_forwarding();
        return DeliveryOutcome::Suppressed;
    }
    try {
        const auto outcome =
            transport_->error(std::move(code), std::move(message), std::move(details), exit_code);
        finish_terminal_forwarding();
        return outcome;
    } catch (...) {
        finish_terminal_forwarding();
        throw;
    }
}

void RequestSession::notify_probe(testing::RequestSessionProbePoint point) const noexcept {
    if (probe_hook_ != nullptr) {
        probe_hook_(probe_context_, point);
    }
}

DeliveryOutcome RequestSession::emit_item(nlohmann::json data) {
    return transport_->item(std::move(data));
}

void RequestSession::emit_progress(nlohmann::json data) {
    transport_->progress(std::move(data));
}

DeliveryOutcome RequestSession::emit_result(nlohmann::json data) {
    try {
        const auto outcome = transport_->result(std::move(data));
        finish_terminal_forwarding();
        return outcome;
    } catch (...) {
        // A terminal claim cannot be retried regardless of the transport's exception type.
        finish_terminal_forwarding();
        throw;
    }
}

DeliveryOutcome RequestSession::emit_raw_result(secure::SensitiveString canonical) {
    return transport_->raw_result(std::move(canonical));
}

DeliveryOutcome RequestSession::emit_error(std::string code, std::string message,
                                           nlohmann::json details, int exit_code) {
    try {
        const auto outcome =
            transport_->error(std::move(code), std::move(message), std::move(details), exit_code);
        finish_terminal_forwarding();
        return outcome;
    } catch (...) {
        // A terminal claim cannot be retried regardless of the transport's exception type.
        finish_terminal_forwarding();
        throw;
    }
}

ChallengeReply RequestSession::emit_challenge(nlohmann::json data) {
    return {transport_->challenge(std::move(data)), std::nullopt};
}

void RequestSession::emit_abort() noexcept {
    transport_->abort_transport();
}

void RequestSession::before_direct_terminal_bit() noexcept {
    notify_probe(testing::RequestSessionProbePoint::BeforePublicTerminalBit);
}

void RequestSession::between_terminal_batch_frames() noexcept {
    notify_probe(testing::RequestSessionProbePoint::BetweenTerminalBatchFrames);
}

} // namespace tgcli::daemon
