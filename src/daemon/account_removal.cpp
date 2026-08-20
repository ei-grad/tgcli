#include "daemon/account_removal.hpp"

#include "common/exit_codes.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/removal_recovery.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

std::string random_invocation_id() {
    std::array<unsigned char, 16> bytes{};
    const int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return {};
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(descriptor);
            return {};
        }
        offset += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::string current_timestamp() {
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

json stages_json(const std::vector<AuditStage>& stages) {
    json result = json::array();
    for (const auto stage : stages) {
        result.push_back(audit_stage_name(stage));
    }
    return result;
}

int exit_for(std::string_view code) {
    if (code == "USAGE" || code == "DEFAULT_REASSIGNMENT_REQUIRED") {
        return kUsage;
    }
    if (code == "ACCOUNT_NOT_FOUND") {
        return kNotFound;
    }
    if (code == "WRITE_DENIED" || code == "CONFIRMATION_REQUIRED" ||
        code == "AUTH_FUNCTION_DENIED" || code == "AUDIT_UNAVAILABLE") {
        return kDenied;
    }
    if (code == "RATE_LIMITED") {
        return kRateLimited;
    }
    if (code == "TIMEOUT") {
        return kTimeout;
    }
    return kGeneric;
}

std::string message_for(std::string_view code) {
    if (code == "REMOTE_LOGOUT_UNCONFIRMED") {
        return "remote logout could not be confirmed";
    }
    if (code == "LOCAL_CLEANUP_FAILED") {
        return "local account cleanup failed";
    }
    if (code == "CONFIG_CONFLICT") {
        return "config.toml changed before account removal";
    }
    if (code == "DAEMON_SHUTDOWN") {
        return "daemon is shutting down";
    }
    if (code == "TIMEOUT") {
        return "account removal timed out";
    }
    if (code == "RATE_LIMITED") {
        return "Telegram rate limit";
    }
    if (code == "TDLIB_ERROR") {
        return "TDLib account removal request failed";
    }
    return "account removal failed";
}

std::vector<std::string> sorted_paths(const std::array<std::string, 2>& paths) {
    std::vector<std::string> result(paths.begin(), paths.end());
    std::ranges::sort(result, [](const std::string& left, const std::string& right) {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](unsigned char lhs, unsigned char rhs) { return lhs < rhs; });
    });
    return result;
}

struct ParsedArguments {
    std::string account;
    bool keep_session = false;
    std::optional<std::string> reassign_default;
};

std::optional<ParsedArguments> parse_arguments(const proto::Request& request,
                                               RequestSession& session) {
    if (!request.args.is_object() || request.args.size() != 4 ||
        !request.args.contains("account") || !request.args["account"].is_string() ||
        !request.args.contains("global_account_supplied") ||
        !request.args["global_account_supplied"].is_boolean() ||
        !request.args.contains("keep_session") || !request.args["keep_session"].is_boolean() ||
        !request.args.contains("reassign_default") ||
        (!request.args["reassign_default"].is_null() &&
         !request.args["reassign_default"].is_string())) {
        session.error("USAGE", "invalid account removal arguments",
                      {{"argument", nullptr}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    if (request.args["global_account_supplied"].get<bool>()) {
        session.error("USAGE", "--account cannot target an account subcommand",
                      {{"argument", "--account"}, {"reason", "mutually_exclusive"}}, kUsage);
        return std::nullopt;
    }
    ParsedArguments result{request.args["account"].get<std::string>(),
                           request.args["keep_session"].get<bool>(), std::nullopt};
    if (!request.args["reassign_default"].is_null()) {
        result.reassign_default = request.args["reassign_default"].get<std::string>();
    }
    return result;
}

std::optional<std::string_view> config_mutation_reassignment(const PlannedAccountRemoval& planned) {
    const auto& reassignment = planned.plan.reassign_default();
    if (planned.config->default_account == std::optional{planned.plan.account()} &&
        planned.config->accounts.size() > 1 && reassignment) {
        return reassignment.value();
    }
    return std::nullopt;
}

AccountRemoveRemoteResult remote_result_from(const RemovalTombstone& tombstone) {
    if (std::ranges::find(tombstone.completed_stages, AuditStage::RemoteConfirmed) !=
        tombstone.completed_stages.end()) {
        return AccountRemoveRemoteResult::Confirmed;
    }
    if (std::ranges::find(tombstone.completed_stages, AuditStage::RemoteKept) !=
        tombstone.completed_stages.end()) {
        return AccountRemoveRemoteResult::Kept;
    }
    return AccountRemoveRemoteResult::NotPresent;
}

} // namespace

AccountRemovalCoordinator::AccountRemovalCoordinator(
    const config::Store& store, RemovalJournal& journal, paths::Environment environment,
    std::string account, AccountRemovalRemote& remote, std::function<void()> audit_fatal_shutdown,
    std::shared_ptr<const testing::AccountRemovalHooks> hooks,
    std::function<void()> shutdown_after_terminal)
    : store_(store), journal_(journal), environment_(std::move(environment)),
      account_(std::move(account)), remote_(remote),
      audit_fatal_shutdown_(std::move(audit_fatal_shutdown)), hooks_(std::move(hooks)),
      shutdown_after_terminal_(std::move(shutdown_after_terminal)) {}

bool preflight_account_removal_journal(const RemovalJournal& journal, std::string_view account,
                                       RequestSession& session) {
    const auto inspection = journal.inspect_account(account);
    if (inspection.status == RemovalInspectionStatus::Clean) {
        return true;
    }
    if (inspection.status == RemovalInspectionStatus::Incomplete && inspection.tombstone) {
        const auto& tombstone = *inspection.tombstone;
        session.error("REMOVAL_INCOMPLETE", "account removal requires an explicit retry",
                      {{"account", tombstone.account},
                       {"path", inspection.path},
                       {"invocation_id", tombstone.invocation_id},
                       {"stage", audit_stage_name(tombstone.stage)},
                       {"completed_stages", stages_json(tombstone.completed_stages)},
                       {"reason", "prior_crash"}},
                      kGeneric);
        return false;
    }
    session.error("AUDIT_UNAVAILABLE", "removal journal cannot be inspected",
                  {{"account", account},
                   {"path", inspection.path.empty() ? journal.directory() : inspection.path},
                   {"reason", inspection.failure.reason.empty() ? "path_invalid"
                                                                : inspection.failure.reason}},
                  kDenied);
    return false;
}

bool AccountRemovalCoordinator::preflight(std::string_view account, RequestSession& session) const {
    return preflight_account_removal_journal(journal_, account, session);
}

// The transaction is linear because each callback executes while Store holds config.lock.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void AccountRemovalCoordinator::remove(const proto::Request& request, RequestSession& session) {
    const std::unique_lock operation_lock(operation_mutex_);
    const auto arguments = parse_arguments(request, session);
    if (!arguments) {
        return;
    }
    if (arguments->account != account_ || request.account != account_) {
        session.error("ACCOUNT_MISMATCH", "removal target does not match the routed daemon",
                      {{"requested_account", arguments->account}, {"daemon_account", account_}},
                      kNotFound);
        return;
    }

    auto planned = plan_account_removal(
        store_, journal_, environment_, arguments->account, arguments->keep_session,
        arguments->reassign_default, {session.deadline().expires_at, session.cancellation_token()},
        hooks_ ? hooks_->filesystem : nullptr);
    if (planned.error) {
        const auto& error = planned.error.value();
        session.error(error.code, error.message, error.details, error.exit_code);
        return;
    }
    if (!planned.planned) {
        session.error("INTERNAL", "account removal planner returned no decision",
                      {{"operation", "account_remove"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    auto& removal = planned.planned.value();
    if (request.context.dry_run) {
        session.result({{"dry_run", true}, {"plan", proto::serialize(removal.plan)}});
        return;
    }

    const auto authority = evaluate_destructive_authority(
        request.context,
        {.grant_valid = removal.account_config.has_value(),
         .allow_write = removal.account_config && removal.account_config->allow_write});
    const auto* granted = std::get_if<GrantedAuthority>(&authority);
    if (const auto* denied = std::get_if<DeniedAuthority>(&authority)) {
        session.error("WRITE_DENIED", "account removal requires write authority",
                      {{"account", account_}, {"reason", write_denial_reason_name(denied->reason)}},
                      kDenied);
        return;
    }
    if (granted == nullptr) {
        session.error("INTERNAL", "account removal authority decision is invalid",
                      {{"operation", "account_remove"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }

    ConfirmationSource confirmation_source = ConfirmationSource::Yes;
    if (!request.context.yes) {
        const std::string prompt =
            removal.plan.keep_session()
                ? "Remove local account \"" + account_ +
                      "\" while keeping its Telegram session? [y/N] "
                : "Remove account \"" + account_ + "\" and log out remotely? [y/N] ";
        auto confirmation = session.challenge(
            {proto::ChallengeKind::DestructiveConfirmation,
             std::nullopt,
             std::nullopt,
             prompt,
             {{"action", "account_remove"}, {"target", proto::serialize(removal.plan)}},
             false});
        const auto answer = confirmation.take_boolean();
        if (confirmation.status() != ChallengeStatus::Answered || !answer.value_or(false)) {
            if (!session.has_terminal() && confirmation.status() != ChallengeStatus::Disconnected &&
                confirmation.status() != ChallengeStatus::Shutdown &&
                confirmation.status() != ChallengeStatus::ProtocolError) {
                session.error("CONFIRMATION_REQUIRED", "account removal was not confirmed",
                              {{"account", account_},
                               {"action", "account_remove"},
                               {"target", proto::serialize(removal.plan)}},
                              kDenied);
            }
            return;
        }
        confirmation_source = ConfirmationSource::Tty;
    }

    std::string invocation;
    if (removal.recovery) {
        invocation = removal.recovery->invocation_id;
    } else if (hooks_ && hooks_->invocation_id) {
        invocation = hooks_->invocation_id();
    } else {
        invocation = random_invocation_id();
    }
    const auto record_identity = [&] {
        return AuditRecordIdentity{invocation, hooks_ && hooks_->timestamp ? hooks_->timestamp()
                                                                           : current_timestamp()};
    };
    if (invocation.empty()) {
        session.error(
            "AUDIT_UNAVAILABLE", "cannot create removal audit identity",
            {{"account", account_}, {"path", journal_.audit_path()}, {"reason", "open_failed"}},
            kDenied);
        return;
    }

    struct TransactionState {
        bool fatal = false;
        bool client_quiesced = false;
        bool terminal_ready = false;
        std::optional<RemovalOperationError> terminal_error;
        std::optional<json> terminal_result;
        std::shared_ptr<const config::ConfigSnapshot> current_config;
    } state;
    state.current_config = removal.config;
    RemovalJournalFailure journal_failure;
    const auto fatal = [&] {
        state.fatal = true;
        session.audit_fatal();
        if (audit_fatal_shutdown_) {
            audit_fatal_shutdown_();
        }
    };
    const auto checkpoint = [&](AuditStage stage) {
        if (!journal_.advance(invocation, stage, journal_failure)) {
            fatal();
            return false;
        }
        return true;
    };
    const auto load_tombstone = [&]() -> std::optional<RemovalTombstone> {
        auto tombstone = journal_.load(invocation, journal_failure);
        if (!tombstone) {
            fatal();
        }
        return tombstone;
    };
    const auto restore_terminal = [&](const json& outcome) {
        if (outcome.value("success", false)) {
            state.terminal_result = outcome["result"];
        } else if (outcome.contains("error") && outcome["error"].is_object() &&
                   outcome["error"].contains("code") && outcome["error"]["code"].is_string() &&
                   outcome["error"].contains("details")) {
            const auto code = outcome["error"]["code"].get<std::string>();
            state.terminal_error = RemovalOperationError{
                code, message_for(code), outcome["error"]["details"], exit_for(code)};
        } else {
            fatal();
            return;
        }
        state.terminal_ready = true;
    };
    const auto sync_existing_outcome = [&] {
        auto outcome = journal_.audit_outcome(invocation, journal_failure);
        if (!outcome || !checkpoint(AuditStage::OutcomeSynced)) {
            fatal();
            return false;
        }
        restore_terminal(*outcome);
        return !state.fatal;
    };
    const auto record_failure = [&](RemovalOperationError error) {
        auto tombstone = load_tombstone();
        if (!tombstone) {
            return false;
        }
        auto structured = parse_structured_outcome_error(
            {{"code", error.code}, {"details", error.details}}, journal_failure.reason);
        auto outcome = structured ? make_failure_audit_outcome(record_identity(),
                                                               proto::DestructivePlan{removal.plan},
                                                               tombstone->completed_stages,
                                                               *structured, journal_failure.reason)
                                  : std::nullopt;
        if (!outcome || !journal_.append_outcome(*outcome, journal_failure) ||
            !checkpoint(AuditStage::OutcomeSynced)) {
            fatal();
            return false;
        }
        state.terminal_error = std::move(error);
        state.terminal_ready = true;
        return true;
    };
    const auto audit_unavailable = [&](std::string_view reason) {
        state.terminal_error = RemovalOperationError{
            "AUDIT_UNAVAILABLE",
            "removal audit is unavailable",
            {{"account", account_}, {"path", journal_.audit_path()}, {"reason", reason}},
            kDenied};
        state.terminal_ready = true;
    };
    const auto ensure_intent = [&] {
        const auto presence = journal_.audit_presence(invocation, journal_failure);
        if (!presence) {
            fatal();
            return false;
        }
        if (!presence->intent) {
            auto intent =
                make_account_remove_audit_intent(record_identity(), removal.plan, granted->source,
                                                 confirmation_source, journal_failure.reason);
            if (!intent || !journal_.append_intent(*intent, journal_failure)) {
                audit_unavailable(journal_failure.reason.empty() ? "write_failed"
                                                                 : journal_failure.reason);
                return false;
            }
        }
        return checkpoint(AuditStage::IntentSynced);
    };
    const auto observe_facts =
        [&](const RemovalTombstone& tombstone) -> std::optional<RemovalRecoveryFacts> {
        RemovalFilesystemFailure filesystem_failure;
        const auto data = observe_removal_root(removal.data_root, invocation, "data",
                                               environment_.uid, filesystem_failure);
        const auto state_root = observe_removal_root(removal.state_root, invocation, "state",
                                                     environment_.uid, filesystem_failure);
        const auto audit = journal_.audit_presence(invocation, journal_failure);
        if (!data || !state_root || !audit || !state.current_config) {
            return std::nullopt;
        }
        static_cast<void>(tombstone);
        return RemovalRecoveryFacts{*state.current_config, *data, *state_root, *audit};
    };
    // This loop is the pre-config half of the durable recovery state machine. Keeping the action
    // switch closed here prevents a new recovery action from silently skipping its checkpoint.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    const auto execute_until_config = [&]() {
        for (;;) {
            auto tombstone = load_tombstone();
            if (!tombstone) {
                return false;
            }
            auto facts = observe_facts(*tombstone);
            if (!facts) {
                fatal();
                return false;
            }
            const auto decision = decide_removal_recovery(*tombstone, *facts);
            if (!decision) {
                state.terminal_error = RemovalOperationError{
                    "REMOVAL_INCOMPLETE",
                    "removal state no longer matches its durable plan",
                    {{"account", account_},
                     {"path", journal_.tombstone_path(invocation)},
                     {"invocation_id", invocation},
                     {"stage", audit_stage_name(tombstone->stage)},
                     {"completed_stages", stages_json(tombstone->completed_stages)},
                     {"reason", decision.reason}},
                    kGeneric};
                state.terminal_ready = true;
                return false;
            }
            switch (*decision.action) {
            case RemovalRecoveryAction::EnsureIntent:
                if (!ensure_intent()) {
                    return false;
                }
                continue;
            case RemovalRecoveryAction::ReevaluateRemote:
                if (removal.plan.keep_session()) {
                    if (!checkpoint(AuditStage::RemoteKept)) {
                        return false;
                    }
                    continue;
                }
                {
                    const bool send_checkpointed =
                        tombstone->stage == AuditStage::RemoteLogoutSendStarted;
                    auto proof = remote_.prove_remote_logout(
                        removal.plan, removal.config, send_checkpointed, session, checkpoint);
                    if (const auto* error = std::get_if<RemovalOperationError>(&proof)) {
                        if (state.fatal) {
                            return false;
                        }
                        static_cast<void>(record_failure(*error));
                        return false;
                    }
                    auto proven = load_tombstone();
                    const auto remote_result = std::get<AccountRemoveRemoteResult>(proof);
                    const bool checkpoint_matches =
                        proven && ((remote_result == AccountRemoveRemoteResult::Confirmed &&
                                    proven->stage == AuditStage::RemoteConfirmed) ||
                                   (remote_result == AccountRemoveRemoteResult::NotPresent &&
                                    proven->stage == AuditStage::RemoteNotPresent));
                    if (!checkpoint_matches) {
                        if (state.fatal) {
                            return false;
                        }
                        static_cast<void>(record_failure(
                            {"INTERNAL",
                             "remote proof lacks a durable checkpoint",
                             {{"operation", "account_remove"}, {"reason", "internal_error"}},
                             kGeneric}));
                        return false;
                    }
                }
                continue;
            case RemovalRecoveryAction::QuiesceClient:
                if (tombstone->stage != AuditStage::ClientCloseStarted &&
                    !checkpoint(AuditStage::ClientCloseStarted)) {
                    return false;
                }
                state.client_quiesced = true;
                if (auto error = remote_.quiesce(session)) {
                    static_cast<void>(record_failure(std::move(*error)));
                    return false;
                }
                if (!checkpoint(AuditStage::ClientClosed)) {
                    return false;
                }
                continue;
            case RemovalRecoveryAction::BeginConfigRemoval:
            case RemovalRecoveryAction::RetryConfigRemoval:
                return checkpoint(AuditStage::ConfigRemoveStarted);
            case RemovalRecoveryAction::RecordOutcomeSynced:
                static_cast<void>(sync_existing_outcome());
                return false;
            case RemovalRecoveryAction::RecordConfigRemoved:
            case RemovalRecoveryAction::FinishDataRemoval:
            case RemovalRecoveryAction::FinishStateRemoval:
            case RemovalRecoveryAction::EnsureOutcome:
            case RemovalRecoveryAction::Complete:
                return false;
            }
        }
    };

    const auto terminal_status = session.begin_audited_terminal();
    if (terminal_status != AuditedTerminalStatus::Designated) {
        if (terminal_status == AuditedTerminalStatus::Shutdown) {
            session.error("DAEMON_SHUTDOWN", "daemon is shutting down",
                          {{"reason", "daemon_shutdown"}}, kGeneric);
        } else if (terminal_status == AuditedTerminalStatus::TimedOut) {
            session.error("TIMEOUT", "account removal timed out",
                          {{"operation", "account_remove"}, {"state", nullptr}}, kTimeout);
        }
        return;
    }
    bool created = removal.recovery.has_value();
    config::MutationControl control{session.deadline().expires_at, session.cancellation_token()};
    control.pre_commit = [&] {
        if (!created) {
            RemovalFilesystemFailure filesystem_failure;
            if (!revalidate_removal_root(removal.data_root, environment_.uid, filesystem_failure,
                                         hooks_ ? hooks_->filesystem : nullptr) ||
                !revalidate_removal_root(removal.state_root, environment_.uid, filesystem_failure,
                                         hooks_ ? hooks_->filesystem : nullptr)) {
                state.terminal_error = RemovalOperationError{
                    "LOCAL_CLEANUP_FAILED",
                    "account roots changed after confirmation",
                    {{"account", account_},
                     {"reason", filesystem_failure.reason},
                     {"removed", json::array()},
                     {"retained", json(sorted_paths(removal.plan.delete_paths()))}},
                    kGeneric};
                state.terminal_ready = true;
                return false;
            }
            if (!journal_.create(invocation, removal.plan, journal_failure)) {
                audit_unavailable(journal_failure.reason);
                return false;
            }
            created = true;
        }
        return execute_until_config();
    };
    control.already_committed_admission = [&](const config::ConfigSnapshot& current) {
        if (!created) {
            return false;
        }
        state.current_config = std::make_shared<const config::ConfigSnapshot>(current);
        auto tombstone = load_tombstone();
        auto facts = tombstone ? observe_facts(*tombstone) : std::nullopt;
        if (!tombstone || !facts) {
            return false;
        }
        const auto decision = decide_removal_recovery(*tombstone, *facts);
        return decision && (*decision.action == RemovalRecoveryAction::RecordConfigRemoved ||
                            *decision.action == RemovalRecoveryAction::FinishDataRemoval ||
                            *decision.action == RemovalRecoveryAction::FinishStateRemoval ||
                            *decision.action == RemovalRecoveryAction::EnsureOutcome ||
                            *decision.action == RemovalRecoveryAction::RecordOutcomeSynced ||
                            *decision.action == RemovalRecoveryAction::Complete);
    };
    // This is the post-config half of the transaction. Its ordered durable stages deliberately
    // remain together so data removal cannot move ahead of the committed config mutation.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    control.post_commit = [&] {
        auto tombstone = load_tombstone();
        if (!tombstone) {
            return false;
        }
        if (tombstone->stage == AuditStage::OutcomeSynced) {
            auto outcome = journal_.audit_outcome(invocation, journal_failure);
            if (!outcome) {
                fatal();
                return false;
            }
            restore_terminal(*outcome);
            return !state.fatal;
        }
        const auto presence = journal_.audit_presence(invocation, journal_failure);
        if (!presence) {
            fatal();
            return false;
        }
        if (presence->outcome) {
            return sync_existing_outcome();
        }
        if (tombstone->stage == AuditStage::ConfigRemoveStarted &&
            !checkpoint(AuditStage::ConfigRemoved)) {
            return false;
        }
        tombstone = load_tombstone();
        if (!tombstone) {
            return false;
        }
        if (tombstone->stage == AuditStage::ConfigRemoved &&
            !checkpoint(AuditStage::DataRemoveStarted)) {
            return false;
        }
        tombstone = load_tombstone();
        if (!tombstone) {
            return false;
        }
        if (tombstone->stage == AuditStage::DataRemoveStarted) {
            RemovalFilesystemFailure filesystem_failure;
            if (!delete_removal_root(removal.data_root, invocation, "data", environment_.uid,
                                     filesystem_failure, hooks_ ? hooks_->filesystem : nullptr)) {
                const auto retained = sorted_paths(removal.plan.delete_paths());
                return record_failure({"LOCAL_CLEANUP_FAILED",
                                       "data root cleanup failed",
                                       {{"account", account_},
                                        {"reason", filesystem_failure.reason},
                                        {"removed", json::array()},
                                        {"retained", json(retained)}},
                                       kGeneric});
            }
            if (!checkpoint(AuditStage::DataRemoved)) {
                return false;
            }
        }
        tombstone = load_tombstone();
        if (!tombstone) {
            return false;
        }
        if (tombstone->stage == AuditStage::DataRemoved &&
            !checkpoint(AuditStage::StateRemoveStarted)) {
            return false;
        }
        tombstone = load_tombstone();
        if (!tombstone) {
            return false;
        }
        if (tombstone->stage == AuditStage::StateRemoveStarted) {
            RemovalFilesystemFailure filesystem_failure;
            if (!delete_removal_root(removal.state_root, invocation, "state", environment_.uid,
                                     filesystem_failure, hooks_ ? hooks_->filesystem : nullptr)) {
                const std::array<std::string, 1> removed{removal.plan.delete_paths()[0]};
                const std::array<std::string, 1> retained{removal.plan.delete_paths()[1]};
                return record_failure({"LOCAL_CLEANUP_FAILED",
                                       "state root cleanup failed",
                                       {{"account", account_},
                                        {"reason", filesystem_failure.reason},
                                        {"removed", json(removed)},
                                        {"retained", json(retained)}},
                                       kGeneric});
            }
            if (!checkpoint(AuditStage::StateRemoved)) {
                return false;
            }
        }
        tombstone = load_tombstone();
        if (!tombstone) {
            return false;
        }
        if (tombstone->stage != AuditStage::StateRemoved) {
            return false;
        }
        auto outcome = make_account_remove_success_audit_outcome(
            record_identity(), removal.plan, remote_result_from(*tombstone),
            tombstone->completed_stages, journal_failure.reason);
        if (!outcome || !journal_.append_outcome(*outcome, journal_failure) ||
            !checkpoint(AuditStage::OutcomeSynced)) {
            fatal();
            return false;
        }
        state.terminal_result = serialize(*outcome)["result"];
        state.terminal_ready = true;
        return true;
    };

    const auto mutation = store_.remove_account(removal.plan.config_snapshot(), account_,
                                                config_mutation_reassignment(removal), control);
    if (state.fatal) {
        return;
    }
    if (state.terminal_ready) {
        if (state.terminal_error) {
            session.error(state.terminal_error->code, state.terminal_error->message,
                          state.terminal_error->details, state.terminal_error->exit_code);
        } else if (state.terminal_result) {
            session.result(std::move(*state.terminal_result));
        }
        if (state.client_quiesced && shutdown_after_terminal_) {
            shutdown_after_terminal_();
        }
        return;
    }
    if (mutation.status == config::MutationStatus::Conflict) {
        session.error("CONFIG_CONFLICT", "config.toml changed before account removal",
                      {{"path", store_.path()},
                       {"expected", removal.plan.config_snapshot()},
                       {"current", mutation.snapshot ? mutation.snapshot->identity : "missing"}},
                      kGeneric);
        return;
    }
    if (mutation.status == config::MutationStatus::TimedOut) {
        session.error("TIMEOUT", "account removal timed out",
                      {{"operation", "account_remove"}, {"state", nullptr}}, kTimeout);
        return;
    }
    if (mutation.status == config::MutationStatus::Cancelled) {
        return;
    }
    if (mutation.status != config::MutationStatus::Applied) {
        session.error(
            "CONFIG_INVALID", "account config removal failed",
            {{"path", store_.path()},
             {"reason", mutation.error ? config::reason_name(mutation.error->reason) : "io_error"}},
            kGeneric);
    }
}

void register_account_removal_command(Dispatcher& dispatcher,
                                      AccountRemovalCoordinator& coordinator) {
    dispatcher.register_command(
        "account remove", {Tier::Destructive,
                           [&coordinator](const proto::Request& request, RequestSession& session) {
                               coordinator.remove(request, session);
                           },
                           true});
}

} // namespace tgcli::daemon
