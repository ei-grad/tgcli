#include "daemon/account_removal_remote.hpp"

#include "common/exit_codes.hpp"
#include "daemon/logout_commands.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <regex>
#include <thread>
#include <tuple>
#include <utility>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;
using core::AuthState;
using core::AuthStateSnapshot;

class RemovalAuthTracker final {
  public:
    RemovalAuthTracker(core::TdClient& client, RequestSession& session)
        : client_(client), session_(session) {
        subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
                {
                    const std::lock_guard lock(mutex_);
                    latest_ = snapshot;
                    pending_.push_back(snapshot);
                }
                condition_.notify_all();
            });
        const auto current = client_.auth_state();
        const std::lock_guard lock(mutex_);
        if (current) {
            latest_ = current;
            pending_.push_back(current);
        }
    }

    ~RemovalAuthTracker() {
        client_.unsubscribe_auth_states(subscription_);
    }

    RemovalAuthTracker(const RemovalAuthTracker&) = delete;
    RemovalAuthTracker& operator=(const RemovalAuthTracker&) = delete;
    RemovalAuthTracker(RemovalAuthTracker&&) = delete;
    RemovalAuthTracker& operator=(RemovalAuthTracker&&) = delete;

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot> current() const {
        const std::lock_guard lock(mutex_);
        return latest_;
    }

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot>
    wait_current(RequestSession::Clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        while (!latest_ && !session_.cancellation_requested() &&
               RequestSession::Clock::now() < deadline) {
            condition_.wait_until(lock, std::min(deadline, RequestSession::Clock::now() + 10ms));
        }
        return latest_;
    }

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot>
    first_after(const AuthStateSnapshot& previous) const {
        const std::lock_guard lock(mutex_);
        return first_after_locked(previous);
    }

    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot>
    wait_after(const AuthStateSnapshot& previous, RequestSession::Clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        while (first_after_locked(previous) == nullptr && !session_.cancellation_requested() &&
               RequestSession::Clock::now() < deadline) {
            condition_.wait_until(lock, std::min(deadline, RequestSession::Clock::now() + 10ms));
        }
        return first_after_locked(previous);
    }

  private:
    [[nodiscard]] std::shared_ptr<const AuthStateSnapshot>
    first_after_locked(const AuthStateSnapshot& previous) const {
        const auto identity = std::pair{previous.client_generation, previous.auth_sequence};
        const auto found = std::ranges::find_if(pending_, [&](const auto& candidate) {
            return candidate &&
                   std::pair{candidate->client_generation, candidate->auth_sequence} > identity;
        });
        if (found != pending_.end()) {
            return *found;
        }
        if (latest_ && std::pair{latest_->client_generation, latest_->auth_sequence} > identity) {
            return latest_;
        }
        return nullptr;
    }

    core::TdClient& client_;
    RequestSession& session_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::shared_ptr<const AuthStateSnapshot> latest_;
    std::deque<std::shared_ptr<const AuthStateSnapshot>> pending_;
    std::uint64_t subscription_ = 0;
};

std::int64_t retry_after(std::string_view message) {
    static const std::regex seconds(R"((?:FLOOD_WAIT_|retry after )([0-9]+))", std::regex::icase);
    std::match_results<std::string_view::const_iterator> match;
    if (std::regex_search(message.begin(), message.end(), match, seconds) && match.size() == 2) {
        try {
            return std::stoll(std::string(match[1].first, match[1].second));
        } catch (const std::exception&) {
            return 0;
        }
    }
    return 0;
}

RemovalOperationError timeout_error(const std::shared_ptr<const AuthStateSnapshot>& state) {
    return {"TIMEOUT",
            "account removal timed out",
            {{"operation", "account_remove"},
             {"state", state ? nlohmann::json(core::auth_state_name(state->data.state))
                             : nlohmann::json(nullptr)}},
            kTimeout};
}

RemovalOperationError remote_error(std::string account,
                                   const std::shared_ptr<const AuthStateSnapshot>& state,
                                   std::string reason) {
    return {"REMOTE_LOGOUT_UNCONFIRMED",
            "remote logout could not be confirmed",
            {{"account", std::move(account)},
             {"state", state ? std::string(core::auth_state_name(state->data.state)) : "unknown"},
             {"reason", std::move(reason)}},
            kGeneric};
}

RemovalOperationError td_error(const core::TdError& error) {
    if (error.code == 429) {
        return {"RATE_LIMITED",
                "Telegram rate limit",
                {{"operation", "account_remove"},
                 {"tdlib_code", 429},
                 {"retry_after", retry_after(error.message)}},
                kRateLimited};
    }
    return {"TDLIB_ERROR",
            "TDLib account removal request failed",
            {{"operation", "account_remove"}, {"tdlib_code", error.code}},
            kGeneric};
}

RemovalOperationError bootstrap_error(const core::BootstrapError& error,
                                      const proto::AccountRemovePlan& plan,
                                      const std::shared_ptr<const AuthStateSnapshot>& state) {
    switch (error.failure) {
    case core::BootstrapFailure::TimedOut:
        return timeout_error(state);
    case core::BootstrapFailure::PathInvalid:
    case core::BootstrapFailure::InvalidSnapshot:
        return {"CONFIG_INVALID",
                "cannot open account storage for removal",
                {{"path", plan.config_path()}, {"reason", "path_invalid"}},
                kGeneric};
    case core::BootstrapFailure::ConfigConflict:
        return {"CONFIG_CONFLICT",
                "config.toml changed before account removal",
                {{"path", plan.config_path()},
                 {"expected", plan.config_snapshot()},
                 {"current", plan.config_snapshot()}},
                kGeneric};
    case core::BootstrapFailure::Cancelled:
    case core::BootstrapFailure::AuthorizationChanged:
        return remote_error(plan.account(), state, "generation_lost");
    case core::BootstrapFailure::InvalidCredential:
    case core::BootstrapFailure::InputRequired:
    case core::BootstrapFailure::HookFailed:
    case core::BootstrapFailure::ConfigInvalid:
    case core::BootstrapFailure::Duplicate:
        return remote_error(plan.account(), state, "state_unproven");
    }
    return remote_error(plan.account(), state, "state_unproven");
}

struct TransitionResult {
    std::shared_ptr<const AuthStateSnapshot> state;
    std::optional<RemovalOperationError> error;
};

struct BootstrapResponsePoll {
    bool accepted = false;
    std::optional<TransitionResult> outcome;
};

RemovalOperationError unexpected_bootstrap_response() {
    return {"INTERNAL",
            "TDLib returned an unexpected bootstrap object",
            {{"operation", "account_remove"}, {"reason", "internal_error"}},
            kGeneric};
}

BootstrapResponsePoll
consume_bootstrap_response(std::future<core::TdValue>& response,
                           const std::shared_ptr<const AuthStateSnapshot>& sent,
                           RemovalAuthTracker& tracker, const proto::AccountRemovePlan& plan) {
    auto changed = tracker.first_after(*sent);
    try {
        auto value = response.get();
        const bool transition_preceded_response =
            changed && changed->receive_event_sequence != 0 &&
            (value.receive_event_sequence() == 0 ||
             changed->receive_event_sequence < value.receive_event_sequence());
        if (transition_preceded_response) {
            return {false, TransitionResult{std::move(changed), std::nullopt}};
        }
        if (const auto* error = value.get_if<core::TdError>()) {
            return {false, TransitionResult{{}, td_error(*error)}};
        }
        if (value.get_if<core::TdOk>() == nullptr) {
            return {false, TransitionResult{{}, unexpected_bootstrap_response()}};
        }
        return {true, std::nullopt};
    } catch (const std::exception&) {
        changed = tracker.first_after(*sent);
        if (changed) {
            return {false, TransitionResult{std::move(changed), std::nullopt}};
        }
        return {false, TransitionResult{
                           {}, remote_error(plan.account(), tracker.current(), "transport_lost")}};
    }
}

TransitionResult wait_for_transition(std::future<core::TdValue>& response,
                                     const std::shared_ptr<const AuthStateSnapshot>& sent,
                                     RemovalAuthTracker& tracker, RequestSession& session,
                                     const proto::AccountRemovePlan& plan) {
    bool response_ok = false;
    for (;;) {
        auto changed = tracker.first_after(*sent);
        if (!response_ok && response.wait_for(0ms) == std::future_status::ready) {
            auto poll = consume_bootstrap_response(response, sent, tracker, plan);
            if (poll.outcome) {
                return std::move(*poll.outcome);
            }
            response_ok = poll.accepted;
        }
        if (changed) {
            return {std::move(changed), std::nullopt};
        }
        if (RequestSession::Clock::now() >= session.deadline()) {
            return {{}, timeout_error(tracker.current())};
        }
        if (session.cancellation_requested()) {
            return {{}, remote_error(plan.account(), tracker.current(), "transport_lost")};
        }
        std::this_thread::sleep_for(1ms);
    }
}

class DirectInFlightScope final {
  public:
    explicit DirectInFlightScope(RequestSession& session)
        : session_(session), acquired_(session_.reserve_direct_in_flight()) {}

    ~DirectInFlightScope() {
        if (acquired_) {
            session_.settle_in_flight();
        }
    }

    DirectInFlightScope(const DirectInFlightScope&) = delete;
    DirectInFlightScope& operator=(const DirectInFlightScope&) = delete;
    DirectInFlightScope(DirectInFlightScope&&) = delete;
    DirectInFlightScope& operator=(DirectInFlightScope&&) = delete;

    [[nodiscard]] bool acquired() const {
        return acquired_;
    }

  private:
    RequestSession& session_;
    bool acquired_;
};

std::optional<RemovalOperationError>
lifecycle_error(core::TdClosedDecisionStatus status, const proto::AccountRemovePlan& plan,
                const std::shared_ptr<const AuthStateSnapshot>& state) {
    switch (status) {
    case core::TdClosedDecisionStatus::Disconnected:
        return remote_error(plan.account(), state, "transport_lost");
    case core::TdClosedDecisionStatus::Shutdown:
        return RemovalOperationError{"DAEMON_SHUTDOWN",
                                     "daemon is shutting down",
                                     {{"reason", "daemon_shutdown"}},
                                     kGeneric};
    case core::TdClosedDecisionStatus::TimedOut:
        return remote_error(plan.account(), state, "timeout");
    case core::TdClosedDecisionStatus::Rejected:
        return remote_error(plan.account(), state, "generation_lost");
    case core::TdClosedDecisionStatus::Pending:
    case core::TdClosedDecisionStatus::Closed:
    case core::TdClosedDecisionStatus::Error:
        return std::nullopt;
    }
    return remote_error(plan.account(), state, "generation_lost");
}

RemovalOperationError unexpected_logout_response() {
    return {"INTERNAL",
            "TDLib returned an unexpected logout object",
            {{"operation", "account_remove"}, {"reason", "internal_error"}},
            kGeneric};
}

RemovalOperationError authorization_error(const core::TdAuthorizationError& error,
                                          const proto::AccountRemovePlan& plan,
                                          const std::shared_ptr<const AuthStateSnapshot>& state) {
    if (error.failure() == core::TdAuthorizationFailure::FunctionDenied) {
        return {
            "AUTH_FUNCTION_DENIED",
            "TDLib logout request was denied",
            {{"account", plan.account()},
             {"state", state ? std::string(core::auth_state_name(state->data.state)) : "unknown"},
             {"function", "logOut"}},
            kDenied};
    }
    return remote_error(plan.account(), state, "generation_lost");
}

std::optional<RemovalOperationError> observe_logout_progress(
    const proto::AccountRemovePlan& plan, const std::shared_ptr<const AuthStateSnapshot>& starting,
    RemovalAuthTracker& tracker, std::shared_ptr<const AuthStateSnapshot>& observed) {
    auto changed = tracker.first_after(*observed);
    if (!changed) {
        return std::nullopt;
    }
    observed = std::move(changed);
    if (observed->client_generation != starting->client_generation) {
        return remote_error(plan.account(), observed, "generation_lost");
    }
    return std::nullopt;
}

std::optional<RemovalRemoteProof>
decision_outcome(core::TdClosedDecisionStatus status, const proto::AccountRemovePlan& plan,
                 const std::shared_ptr<const AuthStateSnapshot>& observed,
                 const RemovalCheckpoint& checkpoint) {
    if (status == core::TdClosedDecisionStatus::Closed) {
        if (!checkpoint(AuditStage::RemoteConfirmed)) {
            return RemovalRemoteProof{remote_error(plan.account(), observed, "transport_lost")};
        }
        return RemovalRemoteProof{AccountRemoveRemoteResult::Confirmed};
    }
    if (auto error = lifecycle_error(status, plan, observed)) {
        return RemovalRemoteProof{std::move(*error)};
    }
    return std::nullopt;
}

std::optional<RemovalRemoteProof>
consume_logout_response(std::future<core::TdValue>& response, core::TdClosedDecision& decision,
                        const proto::AccountRemovePlan& plan,
                        const std::shared_ptr<const AuthStateSnapshot>& observed) {
    try {
        auto value = response.get();
        if (const auto* error = value.get_if<core::TdError>()) {
            const auto status = decision.status();
            if (status == core::TdClosedDecisionStatus::Closed) {
                return std::nullopt;
            }
            if (status == core::TdClosedDecisionStatus::Error) {
                return RemovalRemoteProof{td_error(*error)};
            }
            if (auto terminal = lifecycle_error(status, plan, observed)) {
                return RemovalRemoteProof{std::move(*terminal)};
            }
        } else if (value.get_if<core::TdOk>() == nullptr) {
            return RemovalRemoteProof{unexpected_logout_response()};
        }
    } catch (const core::TdAuthorizationError& error) {
        if (decision.status() != core::TdClosedDecisionStatus::Closed) {
            return RemovalRemoteProof{authorization_error(error, plan, observed)};
        }
    } catch (const std::exception&) {
        if (decision.status() != core::TdClosedDecisionStatus::Closed) {
            return RemovalRemoteProof{remote_error(plan.account(), observed, "generation_lost")};
        }
    }
    return std::nullopt;
}

bool terminal_requested(const RequestSession& session) {
    return session.shutdown_requested() || session.cancellation_requested() ||
           RequestSession::Clock::now() >= session.deadline();
}

RemovalRemoteProof wait_for_logout_decision(
    const proto::AccountRemovePlan& plan, const std::shared_ptr<const AuthStateSnapshot>& starting,
    RemovalAuthTracker& tracker, RequestSession& session, const RemovalCheckpoint& checkpoint,
    core::TdClosedDecision& decision, std::future<core::TdValue>& response) {
    auto observed = starting;
    bool response_consumed = false;
    for (;;) {
        if (auto error = observe_logout_progress(plan, starting, tracker, observed)) {
            return std::move(*error);
        }

        if (auto outcome = decision_outcome(decision.status(), plan, observed, checkpoint)) {
            return std::move(*outcome);
        }

        if (!response_consumed && response.wait_for(0ms) == std::future_status::ready) {
            response_consumed = true;
            if (auto outcome = consume_logout_response(response, decision, plan, observed)) {
                return std::move(*outcome);
            }
        }

        if (terminal_requested(session)) {
            const auto terminal = decision.settle_terminal();
            if (auto outcome = decision_outcome(terminal, plan, observed, checkpoint)) {
                return std::move(*outcome);
            }
        }
        std::this_thread::sleep_for(1ms);
    }
}

RemovalRemoteProof
dispatch_logout(core::TdClient& client, const proto::AccountRemovePlan& plan,
                const std::shared_ptr<const AuthStateSnapshot>& current, bool send_checkpointed,
                RemovalAuthTracker& tracker, RequestSession& session,
                const RemovalCheckpoint& checkpoint,
                const std::shared_ptr<const testing::AccountRemovalRemoteHooks>& hooks) {
    const DirectInFlightScope in_flight(session);
    if (!in_flight.acquired()) {
        if (session.shutdown_requested()) {
            return RemovalOperationError{"DAEMON_SHUTDOWN",
                                         "daemon is shutting down",
                                         {{"reason", "daemon_shutdown"}},
                                         kGeneric};
        }
        return remote_error(plan.account(), current, "transport_lost");
    }
    if (!send_checkpointed && !checkpoint(AuditStage::RemoteLogoutSendStarted)) {
        return remote_error(plan.account(), current, "transport_lost");
    }

    std::optional<core::TdClosedDecision> decision;
    std::optional<std::future<core::TdValue>> response;
    AuditedDispatchStatus dispatch_status = AuditedDispatchStatus::ProtocolError;
    try {
        dispatch_status = session.dispatch_audited([&] {
            auto started = LogoutLifecycle::begin(client, current, session,
                                                  hooks ? hooks->during_terminal_claim
                                                        : std::function<void()>{});
            if (started) {
                decision.emplace(std::move(started));
                if (hooks && hooks->before_send) {
                    hooks->before_send();
                }
                response.emplace(LogoutLifecycle::send(client, current, *decision));
            }
        });
    } catch (const std::exception&) {
        return remote_error(plan.account(), current, "generation_lost");
    }
    if (dispatch_status == AuditedDispatchStatus::Shutdown) {
        return RemovalOperationError{"DAEMON_SHUTDOWN",
                                     "daemon is shutting down",
                                     {{"reason", "daemon_shutdown"}},
                                     kGeneric};
    }
    if (dispatch_status == AuditedDispatchStatus::TimedOut) {
        return remote_error(plan.account(), current, "timeout");
    }
    if (dispatch_status == AuditedDispatchStatus::Disconnected) {
        return remote_error(plan.account(), current, "transport_lost");
    }
    if (dispatch_status != AuditedDispatchStatus::Dispatched || !decision || !response) {
        return remote_error(plan.account(), current, "generation_lost");
    }
    return wait_for_logout_decision(plan, current, tracker, session, checkpoint, *decision,
                                    *response);
}

std::shared_ptr<const AuthStateSnapshot> wait_for_known_state(RemovalAuthTracker& tracker,
                                                              RequestSession& session) {
    auto current = tracker.wait_current(session.deadline());
    while (current && current->data.state == AuthState::Unknown &&
           RequestSession::Clock::now() < session.deadline() && !session.cancellation_requested()) {
        auto changed = tracker.wait_after(*current, session.deadline());
        if (!changed) {
            break;
        }
        current = std::move(changed);
    }
    return current;
}

RemovalRemoteProof checkpoint_not_present(std::string_view account,
                                          const std::shared_ptr<const AuthStateSnapshot>& current,
                                          const RemovalCheckpoint& checkpoint) {
    if (!checkpoint(AuditStage::RemoteNotPresent)) {
        return remote_error(std::string(account), current, "transport_lost");
    }
    return AccountRemoveRemoteResult::NotPresent;
}

RemovalOperationError
config_conflict_error(const proto::AccountRemovePlan& plan,
                      const std::shared_ptr<const config::ConfigSnapshot>& config_snapshot) {
    const std::string current = config_snapshot ? config_snapshot->identity : "missing";
    return {
        "CONFIG_CONFLICT",
        "config.toml changed before account removal",
        {{"path", plan.config_path()}, {"expected", plan.config_snapshot()}, {"current", current}},
        kGeneric};
}

struct RemoteBootstrapContext {
    std::reference_wrapper<core::TdClient> client;
    std::reference_wrapper<const config::Store> store;
    std::reference_wrapper<const paths::Environment> environment;
    std::reference_wrapper<const std::string> account;
    std::reference_wrapper<const std::string> application_version;
    std::reference_wrapper<const std::optional<std::string>> environment_api_id;
    std::reference_wrapper<const std::optional<std::string>> environment_api_hash;
    std::reference_wrapper<const core::AuthBootstrap::HookRunner> hook_runner;
};

std::optional<RemovalRemoteProof>
prepare_parameter_state(const RemoteBootstrapContext& context, const proto::AccountRemovePlan& plan,
                        const std::shared_ptr<const config::ConfigSnapshot>& config_snapshot,
                        RemovalAuthTracker& tracker, RequestSession& session,
                        const RemovalCheckpoint& checkpoint,
                        std::shared_ptr<const AuthStateSnapshot>& current) {
    if (current->data.state != AuthState::WaitTdlibParameters) {
        return std::nullopt;
    }
    if (!plan.data_root()) {
        return checkpoint_not_present(context.account.get(), current, checkpoint);
    }
    if (!config_snapshot || config_snapshot->identity != plan.config_snapshot()) {
        return RemovalRemoteProof{config_conflict_error(plan, config_snapshot)};
    }
    const auto& environment = context.environment.get();
    auto captured = core::capture_bootstrap_snapshot(
        context.account.get(), config_snapshot, environment, environment.test_dc,
        context.application_version.get(), context.environment_api_id.get(),
        context.environment_api_hash.get());
    if (!captured.snapshot) {
        const auto error = captured.error.value_or(core::BootstrapError{
            .failure = core::BootstrapFailure::InvalidSnapshot, .fields = {}, .hook = {}});
        return RemovalRemoteProof{bootstrap_error(error, plan, current)};
    }
    core::AuthBootstrap bootstrap(context.client.get(), context.store.get(),
                                  std::move(captured.snapshot.value()), context.hook_runner.get());
    core::BootstrapAttempt attempt;
    attempt.control = {session.deadline(), session.cancellation_token()};
    auto started = bootstrap.run(current, attempt);
    if (!started.response) {
        const auto error = started.error.value_or(core::BootstrapError{
            .failure = core::BootstrapFailure::Duplicate, .fields = {}, .hook = {}});
        return RemovalRemoteProof{bootstrap_error(error, plan, current)};
    }
    auto response = std::move(started.response.value());
    auto transition = wait_for_transition(response, current, tracker, session, plan);
    if (transition.error) {
        return RemovalRemoteProof{std::move(*transition.error)};
    }
    current = std::move(transition.state);
    return std::nullopt;
}

} // namespace

TdAccountRemovalRemote::TdAccountRemovalRemote(
    core::TdClient& client, const config::Store& store, paths::Environment environment,
    std::string account, std::string application_version,
    std::optional<std::string> environment_api_id, std::optional<std::string> environment_api_hash,
    core::AuthBootstrap::HookRunner hook_runner,
    std::shared_ptr<const testing::AccountRemovalRemoteHooks> hooks)
    : client_(client), store_(store), environment_(std::move(environment)),
      account_(std::move(account)), application_version_(std::move(application_version)),
      environment_api_id_(std::move(environment_api_id)),
      environment_api_hash_(std::move(environment_api_hash)), hook_runner_(std::move(hook_runner)),
      hooks_(std::move(hooks)) {}

RemovalRemoteProof TdAccountRemovalRemote::prove_remote_logout(
    const proto::AccountRemovePlan& plan,
    const std::shared_ptr<const config::ConfigSnapshot>& config_snapshot, bool send_checkpointed,
    RequestSession& session, const RemovalCheckpoint& checkpoint) {
    RemovalAuthTracker tracker(client_, session);
    auto current = wait_for_known_state(tracker, session);
    if (!current || current->data.state == AuthState::Unknown) {
        return remote_error(account_, current,
                            session.cancellation_requested() ? "transport_lost" : "timeout");
    }

    const RemoteBootstrapContext bootstrap_context{client_,
                                                   store_,
                                                   environment_,
                                                   account_,
                                                   application_version_,
                                                   environment_api_id_,
                                                   environment_api_hash_,
                                                   hook_runner_};
    if (auto outcome = prepare_parameter_state(bootstrap_context, plan, config_snapshot, tracker,
                                               session, checkpoint, current)) {
        return std::move(*outcome);
    }

    if (current && current->data.state == AuthState::WaitPhoneNumber) {
        return checkpoint_not_present(account_, current, checkpoint);
    }
    if (current && current->data.state == AuthState::Ready) {
        return dispatch_logout(client_, plan, current, send_checkpointed, tracker, session,
                               checkpoint, hooks_);
    }
    return remote_error(account_, current, "state_unproven");
}

std::optional<RemovalOperationError> TdAccountRemovalRemote::quiesce(RequestSession& session) {
    if (!client_.close_until(session.deadline())) {
        return timeout_error(client_.auth_state());
    }
    return std::nullopt;
}

} // namespace tgcli::daemon
