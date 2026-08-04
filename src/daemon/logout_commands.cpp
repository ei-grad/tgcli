#include "daemon/logout_commands.hpp"

#include "common/exit_codes.hpp"
#include "core/auth_bootstrap.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "proto/destructive_plan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <future>
#include <optional>
#include <string_view>
#include <tgcli/version.hpp>
#include <thread>
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

namespace {

using core::AuthState;
using core::AuthStateSnapshot;
using nlohmann::json;

class AuthQueue final {
  public:
    explicit AuthQueue(core::TdClient& client, RequestSession* session = nullptr)
        : client_(client), session_(session) {
        subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
                {
                    const std::lock_guard lock(mutex_);
                    snapshots_.push_back(snapshot);
                }
                if (snapshot && session_ != nullptr) {
                    session_->supersede(snapshot->client_generation, snapshot->auth_sequence);
                }
                condition_.notify_all();
            });
    }

    ~AuthQueue() {
        client_.unsubscribe_auth_states(subscription_);
    }

    AuthQueue(const AuthQueue&) = delete;
    AuthQueue& operator=(const AuthQueue&) = delete;
    AuthQueue(AuthQueue&&) = delete;
    AuthQueue& operator=(AuthQueue&&) = delete;

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot> current() const {
        return client_.auth_state();
    }

    std::shared_ptr<const AuthStateSnapshot>
    take_after(std::uint64_t receive_sequence, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        auto available = [&] {
            return !snapshots_.empty() &&
                   snapshots_.back()->receive_event_sequence > receive_sequence;
        };
        if (!available()) {
            condition_.wait_until(lock, deadline, available);
        }
        while (!snapshots_.empty() &&
               snapshots_.front()->receive_event_sequence <= receive_sequence) {
            snapshots_.pop_front();
        }
        if (snapshots_.empty()) {
            return {};
        }
        auto result = std::move(snapshots_.front());
        snapshots_.pop_front();
        return result;
    }

  private:
    core::TdClient& client_;
    RequestSession* session_;
    std::uint64_t subscription_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<const AuthStateSnapshot>> snapshots_;
};

std::string invocation_id() {
    std::array<unsigned char, 16> bytes{};
    const int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return {};
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            ::close(descriptor);
            return {};
        }
        offset += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::array<char, 21> rendered{};
    if (std::strftime(rendered.data(), rendered.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
    }
    return rendered.data();
}

std::string not_authed_reason(AuthState state) {
    return state == AuthState::WaitPhoneNumber ? "login_required" : "not_ready";
}

std::int32_t retry_after(std::string_view message) {
    std::int32_t value = 0;
    bool found = false;
    for (const char character : message) {
        if (character >= '0' && character <= '9') {
            found = true;
            const auto digit = static_cast<std::int32_t>(character - '0');
            constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
            value = value > (maximum - digit) / 10 ? maximum : value * 10 + digit;
        } else if (found) {
            break;
        }
    }
    return value;
}

std::optional<StructuredOutcomeError> structured_error(std::string code, json details) {
    std::string error;
    return parse_structured_outcome_error(
        {{"code", std::move(code)}, {"details", std::move(details)}}, error);
}

std::string terminal_message(std::string_view code) {
    if (code == "REMOTE_LOGOUT_UNCONFIRMED") {
        return "remote logout could not be confirmed";
    }
    if (code == "RATE_LIMITED") {
        return "Telegram rate limit";
    }
    if (code == "TDLIB_ERROR") {
        return "TDLib logout request failed";
    }
    if (code == "AUTH_FUNCTION_DENIED") {
        return "TDLib logout request was denied";
    }
    if (code == "DAEMON_SHUTDOWN") {
        return "daemon is shutting down";
    }
    if (code == "TIMEOUT") {
        return "logout timed out";
    }
    return "logout failed";
}

std::optional<std::string> environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::optional<std::string>{value} : std::nullopt;
}

int terminal_exit(std::string_view code) {
    if (code == "RATE_LIMITED") {
        return kRateLimited;
    }
    if (code == "AUTH_FUNCTION_DENIED") {
        return kDenied;
    }
    if (code == "TIMEOUT") {
        return kTimeout;
    }
    return kGeneric;
}

} // namespace

LogoutCoordinator::LogoutCoordinator(core::TdClient& client, ConfigRuntime& config_runtime,
                                     paths::Environment environment, std::string account,
                                     std::string config_path,
                                     std::function<void()> audit_fatal_shutdown,
                                     std::shared_ptr<const testing::LogoutHooks> hooks)
    : client_(client), config_runtime_(config_runtime), environment_(std::move(environment)),
      account_(std::move(account)), config_path_(std::move(config_path)),
      config_store_(config_path_, {}, environment_.uid),
      audit_fatal_shutdown_(std::move(audit_fatal_shutdown)), hooks_(std::move(hooks)),
      audit_(paths::account_state_dir(account_, environment_), account_, environment_.uid,
             hooks_ ? hooks_->audit : nullptr) {}

bool LogoutCoordinator::request_active(RequestSession& session) {
    if (session.shutdown_requested()) {
        session.error("DAEMON_SHUTDOWN", "daemon is shutting down", {{"reason", "daemon_shutdown"}},
                      kGeneric);
        return false;
    }
    if (session.cancellation_requested()) {
        return false;
    }
    if (std::chrono::steady_clock::now() >= session.deadline()) {
        session.error("TIMEOUT", "logout audit preflight timed out",
                      {{"operation", "audit"}, {"state", nullptr}}, kTimeout);
        return false;
    }
    return true;
}

bool LogoutCoordinator::acquire_operation_lock(RequestSession& session,
                                               std::unique_lock<std::mutex>& operation_lock) {
    for (;;) {
        if (!request_active(session)) {
            return false;
        }
        if (operation_lock.try_lock()) {
            return request_active(session);
        }
        const auto now = std::chrono::steady_clock::now();
        const auto retry_at = std::min(session.deadline(), now + std::chrono::milliseconds(2));
        if (retry_at > now) {
            std::this_thread::sleep_until(retry_at);
        }
    }
}

ChallengeOutcome LogoutCoordinator::request_challenge(RequestSession& session,
                                                      ChallengeSpec spec) const {
    if (hooks_ && hooks_->challenge_provider) {
        return hooks_->challenge_provider(std::move(spec));
    }
    return session.challenge(std::move(spec));
}

void LogoutCoordinator::report_audit_incomplete(RequestSession& session,
                                                const IncompleteLogoutAudit& incomplete,
                                                std::string_view message) {
    nlohmann::json completed = nlohmann::json::array();
    for (const auto stage : incomplete.completed_stages) {
        completed.push_back(audit_stage_name(stage));
    }
    std::string error;
    const auto mutation =
        derive_mutation_state(DestructiveCommand::Logout, incomplete.completed_stages, error);
    if (!mutation) {
        report_audit_unavailable(session);
        return;
    }
    session.error("AUDIT_INCOMPLETE", std::string(message),
                  {{"account", account_},
                   {"path", audit_.path()},
                   {"mutation_state", mutation_state_name(*mutation)},
                   {"completed_stages", std::move(completed)}},
                  kGeneric);
}

void LogoutCoordinator::report_audit_unavailable(RequestSession& session, std::string reason) {
    session.error("AUDIT_UNAVAILABLE", "logout audit cannot be inspected",
                  {{"account", account_}, {"path", audit_.path()}, {"reason", std::move(reason)}},
                  kDenied);
}

bool LogoutCoordinator::append_unconfirmed_recovery(const IncompleteLogoutAudit& incomplete,
                                                    AuthState observed) {
    const auto audit_timestamp = [this] {
        return hooks_ && hooks_->timestamp ? hooks_->timestamp() : timestamp();
    };
    std::string error_text;
    auto error =
        structured_error("REMOTE_LOGOUT_UNCONFIRMED", {{"account", account_},
                                                       {"state", core::auth_state_name(observed)},
                                                       {"reason", "generation_lost"}});
    if (!error) {
        return false;
    }
    auto outcome = make_failure_audit_outcome({incomplete.invocation_id, audit_timestamp()},
                                              proto::DestructivePlan{incomplete.plan},
                                              incomplete.completed_stages, *error, error_text);
    LogoutAuditFailure failure;
    return outcome && audit_.append(serialize(*outcome), failure);
}

std::optional<AuthState> LogoutCoordinator::wait_for_bootstrap_observation(
    RequestSession& session, const std::shared_ptr<const AuthStateSnapshot>& starting,
    std::future<core::TdValue>& response) {
    bool response_consumed = false;
    for (;;) {
        const auto current = client_.auth_state();
        if (current && current->client_generation == starting->client_generation &&
            current->auth_sequence > starting->auth_sequence &&
            current->data.state != AuthState::WaitTdlibParameters &&
            current->data.state != AuthState::Unknown) {
            return current->data.state;
        }
        if (!response_consumed &&
            response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            response_consumed = true;
            try {
                if (response.get().get_if<core::TdError>() != nullptr) {
                    return std::nullopt;
                }
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
        if (std::chrono::steady_clock::now() >= session.deadline() ||
            session.cancellation_requested()) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::optional<AuthState> LogoutCoordinator::observe_recovery_state(RequestSession& session) {
    auto observed = client_.auth_state();
    while ((!observed || observed->data.state == AuthState::Unknown) &&
           std::chrono::steady_clock::now() < session.deadline() &&
           !session.cancellation_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        observed = client_.auth_state();
    }
    if (!observed || observed->data.state == AuthState::Unknown) {
        return std::nullopt;
    }
    if (observed->data.state != AuthState::WaitTdlibParameters) {
        return observed->data.state;
    }

    const auto loaded = config_store_.load({session.deadline(), session.cancellation_token()});
    if (!loaded || !loaded.snapshot) {
        return std::nullopt;
    }
    auto captured = core::capture_bootstrap_snapshot(
        account_, loaded.snapshot, environment_, environment_.test_dc, kVersion,
        environment_value("TGCLI_API_ID"), environment_value("TGCLI_API_HASH"));
    if (!captured.snapshot) {
        return std::nullopt;
    }
    core::AuthBootstrap bootstrap(client_, config_store_, std::move(*captured.snapshot));
    auto result = bootstrap.run(
        observed, {{session.deadline(), session.cancellation_token()}, false, {}, {}});
    if (!result || !result.response) {
        return std::nullopt;
    }
    return wait_for_bootstrap_observation(session, observed, *result.response);
}

LogoutCoordinator::PreflightStep LogoutCoordinator::reconcile_preflight(RequestSession& session) {
    const auto audit_timestamp = [this] {
        return hooks_ && hooks_->timestamp ? hooks_->timestamp() : timestamp();
    };
    auto reconciliation = reconcile_definite_logout_audit(audit_, audit_timestamp);
    if (!request_active(session)) {
        return PreflightStep::Failed;
    }
    if (reconciliation.status == LogoutAuditReconcileStatus::Clean) {
        return PreflightStep::Complete;
    }
    if (reconciliation.status == LogoutAuditReconcileStatus::Invalid) {
        if (reconciliation.incomplete) {
            report_audit_incomplete(session, *reconciliation.incomplete,
                                    "logout audit contains a partial record");
        } else {
            report_audit_unavailable(session, reconciliation.failure.reason.empty()
                                                  ? "path_invalid"
                                                  : reconciliation.failure.reason);
        }
        return PreflightStep::Failed;
    }
    if (!reconciliation.incomplete) {
        report_audit_unavailable(session);
        return PreflightStep::Failed;
    }
    auto incomplete = std::move(*reconciliation.incomplete);
    if (reconciliation.status == LogoutAuditReconcileStatus::AppendFailed) {
        report_audit_incomplete(session, incomplete, "logout audit cannot be updated");
        return PreflightStep::Failed;
    }
    const auto observed = observe_recovery_state(session);
    if (!observed || !append_unconfirmed_recovery(incomplete, *observed)) {
        report_audit_incomplete(session, incomplete, "logout audit reconciliation is incomplete");
        return PreflightStep::Failed;
    }
    return PreflightStep::Retry;
}

bool LogoutCoordinator::preflight(RequestSession& session) {
    std::unique_lock<std::mutex> operation_lock(operation_mutex_, std::defer_lock);
    if (!acquire_operation_lock(session, operation_lock)) {
        return false;
    }
    for (;;) {
        const auto step = reconcile_preflight(session);
        if (step == PreflightStep::Complete) {
            return true;
        }
        if (step == PreflightStep::Failed) {
            return false;
        }
    }
}

// The operation mutex is account-local. It keeps two independently confirmed logout
// invocations from creating competing lifecycle waiters for one TDLib generation.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void LogoutCoordinator::logout(const proto::Request& request, RequestSession& session) {
    std::unique_lock<std::mutex> operation_lock(operation_mutex_, std::defer_lock);
    if (!acquire_operation_lock(session, operation_lock)) {
        return;
    }
    if (!request.args.empty()) {
        session.error("USAGE", "logout takes no command arguments",
                      {{"argument", nullptr}, {"reason", "invalid_argument"}}, kUsage);
        return;
    }

    std::string plan_error;
    auto plan = proto::make_logout_plan(account_, plan_error);
    if (!plan) {
        session.error("INTERNAL", "cannot build logout plan",
                      {{"operation", "logout"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }

    const auto admission =
        config_runtime_.admit(account_, session.deadline(), session.cancellation_token());
    if (admission.refresh_status == ConfigRefreshStatus::TimedOut) {
        session.error("TIMEOUT", "logout config admission timed out",
                      {{"operation", "logout"}, {"state", nullptr}}, kTimeout);
        return;
    }
    if (admission.refresh_status != ConfigRefreshStatus::Completed || !admission.decision) {
        return;
    }

    std::shared_ptr<const AdmittedAccountConfig> admitted;
    std::optional<ConfigAdmissionDenied> denied;
    if (const auto* value =
            std::get_if<std::shared_ptr<const AdmittedAccountConfig>>(&*admission.decision)) {
        admitted = *value;
    } else {
        denied = std::get<ConfigAdmissionDenied>(*admission.decision);
    }

    if (denied && denied->state == ConfigAdmissionState::AccountMissing) {
        session.error("ACCOUNT_NOT_FOUND", "account is not configured", {{"account", account_}},
                      kNotFound);
        return;
    }
    if (admitted && admitted->state == ConfigAdmissionState::ImplicitMain) {
        session.error("ACCOUNT_NOT_FOUND", "account is not configured", {{"account", account_}},
                      kNotFound);
        return;
    }
    if (denied && denied->state == ConfigAdmissionState::ConfigInvalidWithoutLastGood) {
        session.error("CONFIG_INVALID", "cannot validate logout config",
                      {{"path", config_path_},
                       {"reason", denied->reload_diagnostic
                                      ? config::reason_name(denied->reload_diagnostic->reason)
                                      : "io_error"}},
                      kGeneric);
        return;
    }
    const bool invalid_last_good_account_missing =
        (admitted && admitted->state == ConfigAdmissionState::ConfigInvalidWithLastGood &&
         !admitted->last_good_account_present) ||
        (denied && denied->state == ConfigAdmissionState::ConfigInvalidWithLastGood &&
         !denied->last_good_account_present);
    if (invalid_last_good_account_missing) {
        const auto diagnostic = admitted ? admitted->reload_diagnostic : denied->reload_diagnostic;
        session.error(
            "CONFIG_INVALID", "cannot validate logout account",
            {{"path", config_path_},
             {"reason", diagnostic ? config::reason_name(diagnostic->reason) : "io_error"}},
            kGeneric);
        return;
    }

    if (request.context.dry_run) {
        session.result({{"dry_run", true}, {"plan", proto::serialize(*plan)}});
        return;
    }

    ConfigWriteAuthority config_authority;
    std::string config_snapshot;
    if (admitted) {
        config_authority = {.grant_valid = admitted->standing_write_grants_valid,
                            .allow_write = admitted->settings.allow_write};
        config_snapshot = admitted->snapshot_identity;
    } else {
        config_authority = {.grant_valid = false,
                            .allow_write = denied->last_good_settings &&
                                           denied->last_good_settings->allow_write};
        config_snapshot = denied->snapshot_identity.value_or("missing");
    }
    const auto authority = evaluate_destructive_authority(request.context, config_authority);
    if (const auto* rejected = std::get_if<DeniedAuthority>(&authority)) {
        session.error(
            "WRITE_DENIED", "logout requires write authority",
            {{"account", account_}, {"reason", write_denial_reason_name(rejected->reason)}},
            kDenied);
        return;
    }
    const auto* granted = std::get_if<GrantedAuthority>(&authority);
    if (granted == nullptr) {
        session.error("INTERNAL", "logout authority decision is invalid",
                      {{"operation", "logout"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }

    AuthQueue auth(client_, &session);
    auto starting = auth.current();
    const auto require_ready = [&] {
        const auto current = auth.current();
        if (current && current->data.state == AuthState::Ready) {
            starting = current;
            return true;
        }
        const auto state = current ? current->data.state : AuthState::Unknown;
        session.error("NOT_AUTHED", "logout requires an authenticated account",
                      {{"account", account_},
                       {"state", core::auth_state_name(state)},
                       {"reason", not_authed_reason(state)}},
                      kNotAuthed);
        return false;
    };
    if (!starting || starting->data.state != AuthState::Ready) {
        if (!require_ready()) {
            return;
        }
    }

    ConfirmationSource confirmation_source = ConfirmationSource::Yes;
    if (!request.context.yes) {
        for (;;) {
            auto confirmation = request_challenge(
                session, {proto::ChallengeKind::DestructiveConfirmation,
                          starting->client_generation,
                          starting->auth_sequence,
                          "Log out account \"" + account_ + "\"? [y/N] ",
                          {{"action", "logout"}, {"target", proto::serialize(*plan)}},
                          false});
            const auto answer = confirmation.take_boolean();
            if (confirmation.status() == ChallengeStatus::Superseded) {
                if (!require_ready()) {
                    return;
                }
                continue;
            }
            if (confirmation.status() == ChallengeStatus::Answered && answer.value_or(false)) {
                const auto confirmed = auth.current();
                if (confirmed && confirmed->client_generation == starting->client_generation &&
                    confirmed->auth_sequence == starting->auth_sequence) {
                    break;
                }
                if (!require_ready()) {
                    return;
                }
                continue;
            }
            if (confirmation.status() == ChallengeStatus::TimedOut) {
                session.error("TIMEOUT", "logout confirmation timed out",
                              {{"operation", "logout"}, {"state", "ready"}}, kTimeout);
                return;
            }
            if (!session.has_terminal() && confirmation.status() != ChallengeStatus::Disconnected &&
                confirmation.status() != ChallengeStatus::Shutdown &&
                confirmation.status() != ChallengeStatus::ProtocolError) {
                session.error("CONFIRMATION_REQUIRED", "logout was not confirmed",
                              {{"account", account_},
                               {"action", "logout"},
                               {"target", proto::serialize(*plan)}},
                              kDenied);
            }
            return;
        }
        confirmation_source = ConfirmationSource::Tty;
    } else {
        if (!require_ready()) {
            return;
        }
    }

    const auto identity_value = [this] {
        return AuditRecordIdentity{hooks_ && hooks_->invocation_id ? hooks_->invocation_id()
                                                                   : invocation_id(),
                                   hooks_ && hooks_->timestamp ? hooks_->timestamp() : timestamp()};
    };
    const auto invocation = identity_value().invocation_id;
    if (invocation.empty()) {
        session.error("AUDIT_UNAVAILABLE", "cannot create logout audit identity",
                      {{"account", account_}, {"path", audit_.path()}, {"reason", "open_failed"}},
                      kDenied);
        return;
    }
    const auto record_identity = [&] {
        auto value = identity_value();
        value.invocation_id = invocation;
        return value;
    };

    std::string factory_error;
    auto intent = make_logout_audit_intent(record_identity(), *plan, config_snapshot,
                                           granted->source, confirmation_source, factory_error);
    if (!intent) {
        session.error("INTERNAL", "cannot create logout audit intent",
                      {{"operation", "audit"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    if (hooks_ && hooks_->before_intent) {
        hooks_->before_intent();
    }
    const auto terminal_status = session.begin_audited_terminal();
    if (terminal_status != AuditedTerminalStatus::Designated) {
        if (terminal_status == AuditedTerminalStatus::Shutdown) {
            session.error("DAEMON_SHUTDOWN", "daemon is shutting down",
                          {{"reason", "daemon_shutdown"}}, kGeneric);
        } else if (terminal_status == AuditedTerminalStatus::TimedOut) {
            session.error(
                "TIMEOUT", "logout timed out",
                {{"operation", "logout"}, {"state", core::auth_state_name(starting->data.state)}},
                kTimeout);
        }
        return;
    }
    LogoutAuditFailure audit_failure;
    if (!audit_.append(serialize(*intent), audit_failure, true)) {
        session.error(
            "AUDIT_UNAVAILABLE", "cannot durably append logout intent",
            {{"account", account_}, {"path", audit_.path()}, {"reason", audit_failure.reason}},
            kDenied);
        return;
    }

    std::vector<AuditStage> completed{AuditStage::IntentSynced};
    const auto audit_fatal = [&] {
        session.audit_fatal();
        if (audit_fatal_shutdown_) {
            audit_fatal_shutdown_();
        }
    };
    const auto fail = [&](const std::string& code, json details) {
        auto error = structured_error(code, details);
        if (!error) {
            audit_fatal();
            return;
        }
        std::string error_text;
        auto outcome = make_failure_audit_outcome(record_identity(), proto::DestructivePlan{*plan},
                                                  completed, *error, error_text);
        if (!outcome || !audit_.append(serialize(*outcome), audit_failure)) {
            audit_fatal();
            return;
        }
        session.error(code, terminal_message(code), std::move(details), terminal_exit(code));
    };

    if (!session.reserve_direct_in_flight()) {
        if (session.shutdown_requested()) {
            fail("DAEMON_SHUTDOWN", {{"reason", "daemon_shutdown"}});
        } else {
            fail("REMOTE_LOGOUT_UNCONFIRMED",
                 {{"account", account_},
                  {"state", core::auth_state_name(starting->data.state)},
                  {"reason", "transport_lost"}});
        }
        return;
    }

    auto send_checkpoint = make_logout_audit_checkpoint(
        record_identity(), *plan, AuditStage::LogoutSendStarted, factory_error);
    if (!send_checkpoint || !audit_.append(serialize(*send_checkpoint), audit_failure)) {
        session.settle_in_flight();
        audit_fatal();
        return;
    }
    completed.push_back(AuditStage::LogoutSendStarted);
    std::optional<core::TdClosedDecision> closed_decision;
    std::optional<std::future<core::TdValue>> dispatched_response;
    AuditedDispatchStatus dispatch_status = AuditedDispatchStatus::ProtocolError;
    try {
        dispatch_status = session.dispatch_audited([&] {
            auto decision = client_.begin_logout_decision(
                starting, [this, &session](std::chrono::steady_clock::time_point committed_at) {
                    if (hooks_ && hooks_->during_terminal_claim) {
                        hooks_->during_terminal_claim();
                    }
                    switch (session.claim_audited_terminal_event(committed_at)) {
                    case AuditedTerminalStatus::Designated:
                        return core::TdLifecycleClaimStatus::Active;
                    case AuditedTerminalStatus::Disconnected:
                        return core::TdLifecycleClaimStatus::Disconnected;
                    case AuditedTerminalStatus::Shutdown:
                        return core::TdLifecycleClaimStatus::Shutdown;
                    case AuditedTerminalStatus::TimedOut:
                        return core::TdLifecycleClaimStatus::TimedOut;
                    case AuditedTerminalStatus::ProtocolError:
                        return core::TdLifecycleClaimStatus::Rejected;
                    }
                    return core::TdLifecycleClaimStatus::Rejected;
                });
            if (decision) {
                closed_decision.emplace(std::move(decision));
                if (hooks_ && hooks_->before_send) {
                    hooks_->before_send();
                }
                dispatched_response.emplace(client_.send_logout(starting, *closed_decision));
            }
        });
    } catch (const std::exception&) {
        session.settle_in_flight();
        fail("REMOTE_LOGOUT_UNCONFIRMED", {{"account", account_},
                                           {"state", core::auth_state_name(starting->data.state)},
                                           {"reason", "generation_lost"}});
        return;
    }
    if (dispatch_status == AuditedDispatchStatus::Dispatched && !dispatched_response) {
        session.settle_in_flight();
        fail("REMOTE_LOGOUT_UNCONFIRMED", {{"account", account_},
                                           {"state", core::auth_state_name(starting->data.state)},
                                           {"reason", "generation_lost"}});
        return;
    }
    if (dispatch_status != AuditedDispatchStatus::Dispatched || !dispatched_response ||
        !closed_decision) {
        session.settle_in_flight();
        if (dispatch_status == AuditedDispatchStatus::Shutdown) {
            fail("DAEMON_SHUTDOWN", {{"reason", "daemon_shutdown"}});
        } else if (dispatch_status == AuditedDispatchStatus::TimedOut) {
            fail("REMOTE_LOGOUT_UNCONFIRMED",
                 {{"account", account_},
                  {"state", core::auth_state_name(starting->data.state)},
                  {"reason", "timeout"}});
        } else {
            fail("REMOTE_LOGOUT_UNCONFIRMED",
                 {{"account", account_},
                  {"state", core::auth_state_name(starting->data.state)},
                  {"reason", "transport_lost"}});
        }
        return;
    }
    auto response = std::move(dispatched_response).value();
    if (hooks_ && hooks_->after_send) {
        hooks_->after_send();
    }

    auto observed = starting;
    bool response_consumed = false;
    const auto fail_decision = [&](core::TdClosedDecisionStatus status) {
        if (status == core::TdClosedDecisionStatus::Shutdown) {
            session.settle_in_flight();
            fail("DAEMON_SHUTDOWN", {{"reason", "daemon_shutdown"}});
            return true;
        }
        if (status == core::TdClosedDecisionStatus::TimedOut) {
            session.settle_in_flight();
            fail("REMOTE_LOGOUT_UNCONFIRMED",
                 {{"account", account_},
                  {"state", core::auth_state_name(observed->data.state)},
                  {"reason", "timeout"}});
            return true;
        }
        if (status == core::TdClosedDecisionStatus::Disconnected ||
            status == core::TdClosedDecisionStatus::Rejected) {
            session.settle_in_flight();
            fail("REMOTE_LOGOUT_UNCONFIRMED",
                 {{"account", account_},
                  {"state", core::auth_state_name(observed->data.state)},
                  {"reason", status == core::TdClosedDecisionStatus::Disconnected
                                 ? "transport_lost"
                                 : "generation_lost"}});
            return true;
        }
        return false;
    };
    for (;;) {
        if (fail_decision(closed_decision->status())) {
            return;
        }
        if (const auto update =
                auth.take_after(observed->receive_event_sequence,
                                std::min(session.deadline(), std::chrono::steady_clock::now() +
                                                                 std::chrono::milliseconds(2)))) {
            observed = update;
            if (observed->client_generation != starting->client_generation) {
                session.settle_in_flight();
                fail("REMOTE_LOGOUT_UNCONFIRMED",
                     {{"account", account_},
                      {"state", core::auth_state_name(observed->data.state)},
                      {"reason", "generation_lost"}});
                return;
            }
        }

        if (!response_consumed &&
            response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            response_consumed = true;
            try {
                auto value = response.get();
                if (const auto* error = value.get_if<core::TdError>()) {
                    const auto status = closed_decision->status();
                    if (status == core::TdClosedDecisionStatus::Error) {
                        session.settle_in_flight();
                        if (error->code == 429) {
                            fail("RATE_LIMITED", {{"operation", "logout"},
                                                  {"tdlib_code", 429},
                                                  {"retry_after", retry_after(error->message)}});
                        } else {
                            fail("TDLIB_ERROR",
                                 {{"operation", "logout"}, {"tdlib_code", error->code}});
                        }
                        return;
                    }
                    if (fail_decision(status)) {
                        return;
                    }
                }
            } catch (const core::TdAuthorizationError& error) {
                session.settle_in_flight();
                if (error.failure() == core::TdAuthorizationFailure::GenerationClosed ||
                    error.failure() == core::TdAuthorizationFailure::GenerationMismatch ||
                    error.failure() == core::TdAuthorizationFailure::AuthSequenceMismatch ||
                    error.failure() == core::TdAuthorizationFailure::AuthStateMismatch) {
                    fail("REMOTE_LOGOUT_UNCONFIRMED",
                         {{"account", account_},
                          {"state", core::auth_state_name(observed->data.state)},
                          {"reason", "generation_lost"}});
                } else {
                    fail("AUTH_FUNCTION_DENIED",
                         {{"account", account_},
                          {"state", core::auth_state_name(observed->data.state)},
                          {"function", "logOut"}});
                }
                return;
            } catch (const std::exception&) {
                if (closed_decision->status() != core::TdClosedDecisionStatus::Closed) {
                    session.settle_in_flight();
                    fail("REMOTE_LOGOUT_UNCONFIRMED",
                         {{"account", account_},
                          {"state", core::auth_state_name(observed->data.state)},
                          {"reason", "generation_lost"}});
                    return;
                }
            }
        }

        if (observed->data.state == AuthState::Closed) {
            const auto status = closed_decision->status();
            if (status == core::TdClosedDecisionStatus::Closed) {
                break;
            }
            if (fail_decision(status)) {
                return;
            }
        }

        if (session.shutdown_requested() || session.cancellation_requested() ||
            std::chrono::steady_clock::now() >= session.deadline()) {
            if (fail_decision(closed_decision->settle_terminal())) {
                return;
            }
        }
    }

    session.settle_in_flight();
    auto closed_checkpoint = make_logout_audit_checkpoint(
        record_identity(), *plan, AuditStage::LogoutClosedConfirmed, factory_error);
    if (!closed_checkpoint || !audit_.append(serialize(*closed_checkpoint), audit_failure)) {
        audit_fatal();
        return;
    }
    completed.push_back(AuditStage::LogoutClosedConfirmed);
    auto outcome =
        make_logout_success_audit_outcome(record_identity(), *plan, completed, factory_error);
    if (!outcome || !audit_.append(serialize(*outcome), audit_failure)) {
        audit_fatal();
        return;
    }
    session.result({{"account", account_}, {"logged_out", true}});
}

void register_logout_command(Dispatcher& dispatcher, LogoutCoordinator& coordinator) {
    dispatcher.register_command(
        "logout", {Tier::Destructive,
                   [&coordinator](const proto::Request& request, RequestSession& session) {
                       coordinator.logout(request, session);
                   },
                   true});
}

} // namespace tgcli::daemon
