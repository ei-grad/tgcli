#include "daemon/request_session.hpp"

#include "common/exit_codes.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace tgcli::daemon {

namespace {

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

RequestSession::RequestSession(proto::Request request, ResponseSink& transport,
                               std::uint64_t connection_id, NonceGenerator nonce_generator)
    : request_(std::move(request)), transport_(&transport), connection_id_(connection_id),
      deadline_(compute_deadline(request_)),
      nonce_generator_(nonce_generator ? std::move(nonce_generator) : secure_nonce) {}

RequestSession::RequestSession(proto::Request request, std::shared_ptr<ResponseSink> transport,
                               std::uint64_t connection_id, NonceGenerator nonce_generator)
    : request_(std::move(request)), transport_owner_(std::move(transport)),
      transport_(transport_owner_.get()), connection_id_(connection_id),
      deadline_(compute_deadline(request_)),
      nonce_generator_(nonce_generator ? std::move(nonce_generator) : secure_nonce) {
    if (transport_ == nullptr) {
        throw std::invalid_argument("request session transport is null");
    }
}

const proto::Request& RequestSession::request() const {
    return request_;
}

std::uint64_t RequestSession::connection_id() const {
    return connection_id_;
}

RequestSession::Clock::time_point RequestSession::deadline() const {
    return deadline_;
}

std::stop_token RequestSession::cancellation_token() const {
    return cancellation_source_.get_token();
}

bool RequestSession::cancellation_requested() const {
    return cancellation_source_.stop_requested();
}

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
                return {ChallengeStatus::ProtocolError, std::monostate{}};
            case State::Running:
                break;
            }
        }
        if (Clock::now() >= deadline_) {
            state_ = State::TimedOut;
            return {ChallengeStatus::TimedOut, std::monostate{}};
        }
        if (current_) {
            throw std::logic_error("request already has an active challenge");
        }
        if (answer_reserved_) {
            throw std::logic_error("accepted challenge answer has not been claimed");
        }
        if (spec.client_generation.has_value() != spec.auth_sequence.has_value()) {
            throw std::invalid_argument("challenge generation fields must have matching nullness");
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
        resolve_current({ChallengeStatus::ProtocolError, std::monostate{}});
    }

    std::unique_lock lock(session_mutex_);
    while (!resolution_ || resolution_->sequence != sequence) {
        if (challenge_cv_.wait_until(lock, deadline_) == std::cv_status::timeout) {
            if (Clock::now() >= deadline_ && current_ && current_->identity.sequence == sequence) {
                state_ = State::TimedOut;
                resolve_current({ChallengeStatus::TimedOut, std::monostate{}});
            }
        }
    }
    return resolution_->outcome;
}

// Identity classification, deadline choice, challenge consumption and terminal
// claim must remain one mutex-serialized decision tree.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
AnswerDisposition RequestSession::receive_answer(const proto::Answer& answer) {
    std::string rejection;
    {
        const std::lock_guard lock(session_mutex_);
        if (state_ != State::Running) {
            return AnswerDisposition::RequestTerminated;
        }
        if (Clock::now() >= deadline_) {
            state_ = State::TimedOut;
            if (current_) {
                resolve_current({ChallengeStatus::TimedOut, std::monostate{}});
            }
            return AnswerDisposition::RequestTerminated;
        }
        std::string validation_error;
        if (!proto::validate_answer_payload(answer.answer, validation_error)) {
            rejection = "malformed";
        } else if (answer.id != request_.id) {
            rejection = "unknown_request";
        } else if (exact_consumed(answer)) {
            return AnswerDisposition::DuplicateIgnored;
        } else if (!current_) {
            const auto sequence = answer.answer["sequence"].get<std::uint64_t>();
            if (sequence < next_sequence_) {
                return AnswerDisposition::StaleIgnored;
            }
            rejection = "future_sequence";
        } else {
            const auto sequence = answer.answer["sequence"].get<std::uint64_t>();
            if (sequence < current_->identity.sequence) {
                return AnswerDisposition::StaleIgnored;
            }
            if (sequence > current_->identity.sequence) {
                rejection = "future_sequence";
            } else if (compare_identity(current_->identity.client_generation,
                                        current_->identity.auth_sequence,
                                        answer) == IdentityOrder::Stale) {
                return AnswerDisposition::StaleIgnored;
            } else if (compare_identity(current_->identity.client_generation,
                                        current_->identity.auth_sequence,
                                        answer) == IdentityOrder::Future) {
                rejection = "generation_mismatch";
            } else if (answer.answer["nonce"] != current_->identity.nonce) {
                rejection = "nonce_mismatch";
            } else if (answer.answer.contains("value") &&
                       proto::challenge_kind_expects_boolean(current_->kind) !=
                           answer.answer["value"].is_boolean()) {
                rejection = "malformed";
            } else {
                remember_consumed(current_->identity);
                if (answer.answer.contains("cancelled")) {
                    resolve_current({ChallengeStatus::Cancelled, std::monostate{}});
                    return AnswerDisposition::Cancelled;
                }
                if (current_->reserves_query) {
                    answer_reserved_ = true;
                    reserved_identity_ = current_->identity;
                }
                ChallengeOutcome outcome{ChallengeStatus::Answered, std::monostate{}};
                if (answer.answer["value"].is_boolean()) {
                    outcome.value = answer.answer["value"].get<bool>();
                } else {
                    outcome.value = answer.answer["value"].get<std::string>();
                }
                resolve_current(std::move(outcome));
                return AnswerDisposition::Accepted;
            }
        }
        state_ = State::ProtocolError;
        error("PROTOCOL_ANSWER_INVALID", "invalid challenge answer",
              {{"request_id", answer.id}, {"reason", rejection}}, kUsage);
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
        if (state_ != State::Running) {
            return;
        }
        state_ = State::Disconnected;
        cancellation_source_.request_stop();
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
    notify_in_flight(state, hook);
}

void RequestSession::shutdown() {
    const std::lock_guard lock(session_mutex_);
    if (state_ != State::Running) {
        return;
    }
    state_ = State::Shutdown;
    cancellation_source_.request_stop();
    answer_reserved_ = false;
    reserved_identity_.reset();
    error("DAEMON_SHUTDOWN", "daemon is shutting down", {{"reason", "daemon_shutdown"}}, kGeneric);
    if (current_) {
        resolve_current({ChallengeStatus::Shutdown, std::monostate{}});
    }
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

RequestSession::Clock::time_point RequestSession::compute_deadline(const proto::Request& request) {
    if (const auto deadline = proto::request_deadline(request.context.timeout_seconds)) {
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

void RequestSession::emit_item(nlohmann::json data) {
    transport_->item(std::move(data));
}

void RequestSession::emit_progress(nlohmann::json data) {
    transport_->progress(std::move(data));
}

void RequestSession::emit_result(nlohmann::json data) {
    transport_->result(std::move(data));
}

void RequestSession::emit_error(std::string code, std::string message, nlohmann::json details,
                                int exit_code) {
    transport_->error(std::move(code), std::move(message), std::move(details), exit_code);
}

ChallengeReply RequestSession::emit_challenge(nlohmann::json data) {
    return {transport_->challenge(std::move(data)), std::nullopt};
}

} // namespace tgcli::daemon
