#include "daemon/write_kernel.hpp"

#include "common/exit_codes.hpp"
#include "proto/destructive_plan.hpp"

#include <limits>
#include <utility>
#include <vector>

namespace tgcli::daemon {

bool WriteDurableObservationSink::temporary_message_ids(nlohmann::json ids) noexcept {
    return observe(AccountAuditStage::TemporaryIdsObserved, std::move(ids));
}

bool WriteDurableObservationSink::forward_progress(nlohmann::json items) noexcept {
    return observe(AccountAuditStage::ForwardProgress, std::move(items));
}

bool WriteDurableObservationSink::durable() const noexcept {
    return durable_;
}

bool WriteDurableObservationSink::observe(AccountAuditStage stage, nlohmann::json value) noexcept {
    if (!durable_ || !observer_) {
        return false;
    }
    try {
        durable_ = observer_(stage, std::move(value));
    } catch (...) {
        durable_ = false;
    }
    return durable_;
}

namespace {

using nlohmann::json;

std::optional<AccountAuditOperation> audit_operation(proto::M3Operation operation) {
    const auto* identity = proto::m3_operation_identity(operation);
    return identity == nullptr ? std::nullopt
                               : parse_account_audit_operation(identity->canonical_name);
}

std::string_view operation_name(proto::M3Operation operation) {
    const auto* identity = proto::m3_operation_identity(operation);
    return identity == nullptr ? std::string_view{} : identity->canonical_name;
}

bool destructive(proto::M3Operation operation) {
    return operation == proto::M3Operation::MsgDelete || operation == proto::M3Operation::ChatLeave;
}

json error_terminal(std::string code, std::string message, json details, int exit_code) {
    return {{"kind", "error"},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"details", std::move(details)},
            {"exit_code", exit_code}};
}

WriteKernelResult rejected(json terminal) {
    return {WriteKernelStatus::Rejected, std::move(terminal), std::nullopt};
}

WriteKernelResult plain_audit_fatal() {
    return {WriteKernelStatus::AuditFatal, std::nullopt, std::nullopt};
}

json timeout_terminal(proto::M3Operation operation, std::string_view phase,
                      std::string_view idempotency, std::string_view outcome = "not_started") {
    return error_terminal("TIMEOUT", "request timed out",
                          {{"operation", operation_name(operation)},
                           {"phase", phase},
                           {"state", "ready"},
                           {"outcome", outcome},
                           {"idempotency", idempotency}},
                          kTimeout);
}

std::string_view pre_intent_idempotency(const WriteKernelRequest& request) {
    return request.idempotency_key_hash ? "not_created" : "not_requested";
}

std::string_view post_intent_idempotency(const WriteKernelRequest& request) {
    return request.idempotency_key_hash ? "removed" : "not_requested";
}

std::optional<write_contract::StoredTerminal> cancellation_terminal(proto::M3Operation operation) {
    std::string error;
    return write_contract::make_error_terminal(operation, "DAEMON_SHUTDOWN",
                                               "daemon is shutting down",
                                               {{"reason", "daemon_shutdown"}}, kGeneric, error);
}

bool cancelled(const WriteKernelRequest& request) {
    return request.cancellation_token.stop_requested() ||
           (request.cancelled && request.cancelled());
}

std::optional<WriteKernelResult> post_hook_interruption(const WriteKernelRequest& request,
                                                        std::string_view idempotency,
                                                        std::string_view phase = "preflight") {
    if (deadline_expired(request.deadline)) {
        return rejected(timeout_terminal(request.operation, phase, idempotency));
    }
    if (cancelled(request)) {
        return WriteKernelResult{WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
    }
    return std::nullopt;
}

json audit_unavailable_terminal(std::string_view account, std::string_view path,
                                AccountAuditDurabilityReason reason) {
    return error_terminal("AUDIT_UNAVAILABLE", "account audit log is unavailable",
                          {{"account", account},
                           {"path", path},
                           {"reason", account_audit_durability_reason_name(reason)}},
                          kDenied);
}

json spool_unavailable_terminal(proto::M3Operation operation, const FileSpoolError& failure) {
    const auto durability_name = [](DurabilityReason reason) -> std::string_view {
        switch (reason) {
        case DurabilityReason::PathInvalid:
            return "path_invalid";
        case DurabilityReason::WrongOwner:
            return "wrong_owner";
        case DurabilityReason::WrongType:
            return "wrong_type";
        case DurabilityReason::WrongMode:
            return "wrong_mode";
        case DurabilityReason::WrongLinkCount:
            return "wrong_link_count";
        case DurabilityReason::TooLarge:
            return "too_large";
        case DurabilityReason::CapacityExhausted:
            return "capacity_exhausted";
        case DurabilityReason::OpenFailed:
            return "open_failed";
        case DurabilityReason::LockFailed:
            return "lock_failed";
        case DurabilityReason::ReadFailed:
            return "read_failed";
        case DurabilityReason::WriteFailed:
            return "write_failed";
        case DurabilityReason::SyncFailed:
            return "sync_failed";
        case DurabilityReason::RenameFailed:
            return "rename_failed";
        case DurabilityReason::DirectorySyncFailed:
            return "directory_sync_failed";
        case DurabilityReason::ParseError:
            return "parse_error";
        case DurabilityReason::SchemaError:
            return "schema_error";
        case DurabilityReason::Contradiction:
            return "contradiction";
        }
        return "contradiction";
    };
    const auto reason = failure.durability_reason ? durability_name(*failure.durability_reason)
                                                  : std::string_view{"contradiction"};
    return error_terminal(
        "SPOOL_UNAVAILABLE", "attachment spool is unavailable",
        {{"operation", operation_name(operation)}, {"path", "spool/"}, {"reason", reason}},
        kGeneric);
}

WriteKernelResult gate_failure(const WriteKernelRequest& request,
                               const IdempotencyCoreGateResult& gate,
                               IdempotencyFoundation& foundation) {
    if (gate.status == IdempotencyCoreGateStatus::Interrupted) {
        const auto interruption = gate.store_failure.interruption ? gate.store_failure.interruption
                                                                  : gate.audit_failure.interruption;
        return interruption == AccountAuditFailure::Interruption::Deadline
                   ? rejected(timeout_terminal(request.operation, "preflight",
                                               pre_intent_idempotency(request)))
                   : WriteKernelResult{WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
    }
    if (gate.terminal) {
        return rejected(*gate.terminal);
    }
    if (gate.status == IdempotencyCoreGateStatus::StoreUnavailable) {
        return rejected(idempotency_unavailable_terminal(gate.store_failure));
    }
    if (gate.status == IdempotencyCoreGateStatus::SpoolUnavailable && gate.spool_failure) {
        return rejected(spool_unavailable_terminal(request.operation, *gate.spool_failure));
    }
    return rejected(audit_unavailable_terminal(request.account, foundation.audit().path(),
                                               gate.audit_failure.reason));
}

json pending_terminal(const WriteKernelRequest& request, const IdempotencyEntry& incumbent,
                      const IdempotencyKeyHash& key_hash,
                      const IdempotencyRequestFingerprint& fingerprint) {
    return error_terminal("IDEMPOTENCY_PENDING",
                          "an invocation with this idempotency key is still pending",
                          {{"operation", operation_name(request.operation)},
                           {"key_hash", key_hash.value()},
                           {"fingerprint", fingerprint.value()},
                           {"invocation_id", incumbent.invocation_id},
                           {"temporary_message_ids", incumbent.temporary_message_ids}},
                          kGeneric);
}

json conflict_terminal(const WriteKernelRequest& request, const IdempotencyEntry& incumbent,
                       const IdempotencyKeyHash& key_hash,
                       const IdempotencyRequestFingerprint& fingerprint) {
    return error_terminal("IDEMPOTENCY_CONFLICT",
                          "idempotency key was already used for a different request",
                          {{"operation", operation_name(request.operation)},
                           {"key_hash", key_hash.value()},
                           {"expected_fingerprint", incumbent.request_fingerprint.value()},
                           {"actual_fingerprint", fingerprint.value()}},
                          kUsage);
}

std::optional<write_contract::Plan> incumbent_plan(const WriteKernelRequest& request,
                                                   const IdempotencyEntry& incumbent) {
    std::string error;
    return write_contract::make_plan(request.operation, request.account, incumbent.plan, error);
}

std::optional<write_contract::StoredTerminal>
incumbent_terminal(const WriteKernelRequest& request, const IdempotencyEntry& incumbent) {
    if (!incumbent.terminal) {
        return std::nullopt;
    }
    std::string error;
    return write_contract::make_stored_terminal(request.operation, *incumbent.terminal, error);
}

WriteKernelResult confirm_replay(const WriteKernelRequest& request, const WriteKernelHooks& hooks,
                                 const IdempotencyEntry& incumbent) {
    auto plan = incumbent_plan(request, incumbent);
    auto terminal = incumbent_terminal(request, incumbent);
    if (!plan || !terminal || !write_contract::terminal_matches_plan(*terminal, *plan)) {
        return plain_audit_fatal();
    }
    if (destructive(request.operation)) {
        if (!hooks.confirm) {
            return plain_audit_fatal();
        }
        WriteConfirmationOutcome confirmation;
        try {
            confirmation = hooks.confirm(*plan, true);
        } catch (...) {
            return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
        }
        if (deadline_expired(request.deadline)) {
            return rejected(
                timeout_terminal(request.operation, "replay_confirmation", "completed_unchanged"));
        }
        if (cancelled(request)) {
            return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
        }
        if (confirmation.status != WriteConfirmationStatus::ConfirmedYes &&
            confirmation.status != WriteConfirmationStatus::ConfirmedTty) {
            return {WriteKernelStatus::Rejected, confirmation.terminal, std::nullopt};
        }
    }
    return {WriteKernelStatus::Replayed, terminal->value(), std::move(plan)};
}

std::optional<WriteKernelResult>
incumbent_result(const WriteKernelRequest& request, const WriteKernelHooks& hooks,
                 const IdempotencyLookup& lookup, const IdempotencyKeyHash& key_hash,
                 const IdempotencyRequestFingerprint& fingerprint) {
    if (lookup.status == IdempotencyLookupStatus::Miss) {
        return std::nullopt;
    }
    if (lookup.incumbent == nullptr) {
        return plain_audit_fatal();
    }
    if (lookup.status == IdempotencyLookupStatus::Pending) {
        return WriteKernelResult{
            WriteKernelStatus::Pending,
            pending_terminal(request, *lookup.incumbent, key_hash, fingerprint), std::nullopt};
    }
    if (lookup.status == IdempotencyLookupStatus::Conflict) {
        return WriteKernelResult{
            WriteKernelStatus::Conflict,
            conflict_terminal(request, *lookup.incumbent, key_hash, fingerprint), std::nullopt};
    }
    return confirm_replay(request, hooks, *lookup.incumbent);
}

json file_snapshot_json(const FileSnapshot& file) {
    return {{"path", file.path},         {"name", file.name},        {"size", file.size},
            {"sha256", file.sha256},     {"device", file.device},    {"inode", file.inode},
            {"mtime_ns", file.mtime_ns}, {"ctime_ns", file.ctime_ns}};
}

std::string timestamp(const WriteKernelRequest& request, const WriteKernelHooks& hooks) {
    return hooks.timestamp ? hooks.timestamp() : request.intent_timestamp;
}

bool append_checkpoint(IdempotencyFoundation& foundation, AccountAuditCoordinator::Guard& guard,
                       const WriteKernelRequest& request, const WriteKernelHooks& hooks,
                       AccountAuditOperation operation, std::uint32_t sequence,
                       AccountAuditStage stage, json data,
                       std::vector<AccountAuditStage>& completed) {
    std::string error;
    auto checkpoint =
        make_account_audit_checkpoint({{request.invocation_id, timestamp(request, hooks)},
                                       request.account,
                                       operation,
                                       sequence,
                                       stage,
                                       std::move(data)},
                                      error);
    if (!checkpoint) {
        return false;
    }
    AccountAuditFailure failure;
    if (!foundation.audit().append_checkpoint(*checkpoint, guard, failure)) {
        return false;
    }
    if (completed.empty() || completed.back() != stage ||
        stage != AccountAuditStage::ForwardProgress) {
        completed.push_back(stage);
    }
    return true;
}

bool append_outcome(IdempotencyFoundation& foundation, AccountAuditCoordinator::Guard& guard,
                    const WriteKernelRequest& request, const WriteKernelHooks& hooks,
                    AccountAuditOperation operation, const write_contract::StoredTerminal& terminal,
                    AccountAuditMutationState mutation,
                    const std::vector<AccountAuditStage>& completed) {
    std::string error;
    auto outcome = make_account_audit_outcome({{request.invocation_id, timestamp(request, hooks)},
                                               request.account,
                                               operation,
                                               terminal.success(),
                                               mutation,
                                               completed,
                                               terminal.value()},
                                              error);
    if (!outcome) {
        return false;
    }
    AccountAuditFailure failure;
    return foundation.audit().append_outcome(*outcome, guard, failure);
}

std::optional<json> config_cas_terminal(const WriteKernelRequest& request,
                                        const config::GrantVerificationResult& result) {
    switch (result.status) {
    case config::GrantVerificationStatus::Matched:
        return std::nullopt;
    case config::GrantVerificationStatus::Denied:
        return error_terminal("WRITE_DENIED", "write authority is no longer valid",
                              {{"account", request.account}, {"reason", "invalid_config_grant"}},
                              kDenied);
    case config::GrantVerificationStatus::Conflict:
        return error_terminal(
            "CONFIG_CONFLICT", "config changed after request admission",
            {{"path", request.config_path},
             {"expected", request.config_snapshot},
             {"current", result.snapshot ? result.snapshot->identity : std::string("missing")}},
            kGeneric);
    case config::GrantVerificationStatus::Invalid:
    case config::GrantVerificationStatus::IoError:
        return error_terminal("CONFIG_INVALID", "cannot use current config.toml",
                              {{"path", request.config_path},
                               {"reason", result.error ? config::reason_name(result.error->reason)
                                                       : std::string_view{"io_error"}}},
                              kGeneric);
    case config::GrantVerificationStatus::TimedOut:
        return timeout_terminal(request.operation, "preflight", pre_intent_idempotency(request));
    case config::GrantVerificationStatus::Cancelled:
        return json();
    }
    return json();
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
WriteKernelResult WriteKernel::run(const WriteKernelRequest& request,
                                   const WriteKernelHooks& hooks) const {
    bool intent_may_be_durable = false;
    const auto audit_fatal = [&]() noexcept {
        if (intent_may_be_durable && hooks.audit_fatal_shutdown) {
            bool shutdown_notified = false;
            try {
                hooks.audit_fatal_shutdown();
                shutdown_notified = true;
            } catch (...) {
                shutdown_notified = false;
            }
            static_cast<void>(shutdown_notified);
        }
        return plain_audit_fatal();
    };
    try {
        const auto operation = audit_operation(request.operation);
        if (!operation || request.request_source_bytes == 0 ||
            request.request_source_bytes > proto::kMaximumRequestSourceBytes ||
            request.config_path.empty() || !hooks.admit || !hooks.plan ||
            (!request.dry_run && (!request.sample_now || !hooks.audit_fatal_shutdown))) {
            return audit_fatal();
        }

        if (request.dry_run) {
            std::optional<WriteAdmissionOutcome> admitted;
            try {
                admitted.emplace(hooks.admit());
            } catch (...) {
                return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
            }
            if (auto interruption =
                    post_hook_interruption(request, pre_intent_idempotency(request))) {
                return std::move(*interruption);
            }
            if (auto* terminal = std::get_if<json>(&*admitted)) {
                return rejected(std::move(*terminal));
            }
            auto admission = std::get<WriteAdmission>(std::move(*admitted));
            if (admission.arguments.operation() != request.operation) {
                return audit_fatal();
            }
            std::optional<WritePlanningOutcome> planning;
            try {
                planning.emplace(hooks.plan(admission));
            } catch (...) {
                return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
            }
            if (auto interruption =
                    post_hook_interruption(request, pre_intent_idempotency(request))) {
                return std::move(*interruption);
            }
            if (auto* terminal = std::get_if<json>(&*planning)) {
                return rejected(std::move(*terminal));
            }
            auto plan = std::get<write_contract::Plan>(std::move(*planning));
            if (plan.operation() != request.operation || plan.account() != request.account) {
                return audit_fatal();
            }
            return {WriteKernelStatus::DryRunPlanned, std::nullopt, std::move(plan)};
        }
        if (!foundation_) {
            return audit_fatal();
        }

        const AccountAuditScanControl scan_control{request.deadline, request.cancelled};
        const FileSpoolControl spool_control{request.deadline.expires_at,
                                             request.cancellation_token, request.cancelled};
        std::optional<WriteAdmission> admission;
        {
            auto epoch_result = foundation_->acquire_epoch(scan_control);
            if (auto* failure = std::get_if<AccountAuditFailure>(&epoch_result)) {
                return failure->interruption == AccountAuditFailure::Interruption::Deadline
                           ? rejected(timeout_terminal(request.operation, "preflight",
                                                       pre_intent_idempotency(request)))
                           : WriteKernelResult{WriteKernelStatus::Rejected, std::nullopt,
                                               std::nullopt};
            }
            auto epoch = std::get<AccountAuditCoordinator::Guard>(std::move(epoch_result));
            const auto initial_sampled_now = request.sample_now();
            const auto gate = foundation_->run_core_gate(epoch, initial_sampled_now, {},
                                                         spool_control, hooks.spool_hooks);
            if (gate.status != IdempotencyCoreGateStatus::Clean) {
                return gate_failure(request, gate, *foundation_);
            }
            std::optional<WriteAdmissionOutcome> admitted;
            try {
                admitted.emplace(hooks.admit());
            } catch (...) {
                return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
            }
            if (auto interruption =
                    post_hook_interruption(request, pre_intent_idempotency(request))) {
                return std::move(*interruption);
            }
            if (auto* terminal = std::get_if<json>(&*admitted)) {
                return rejected(std::move(*terminal));
            }
            admission.emplace(std::get<WriteAdmission>(std::move(*admitted)));
            if (admission->arguments.operation() != request.operation) {
                return audit_fatal();
            }
            if (request.idempotency_key_hash) {
                const auto lookup = IdempotencyStore::lookup(
                    gate.snapshot, *request.idempotency_key_hash, admission->request_fingerprint);
                if (auto result =
                        incumbent_result(request, hooks, lookup, *request.idempotency_key_hash,
                                         admission->request_fingerprint)) {
                    return std::move(*result);
                }
            }
        }

        if (!admission) {
            return audit_fatal();
        }
        std::optional<WritePlanningOutcome> planning;
        try {
            planning.emplace(hooks.plan(*admission));
        } catch (...) {
            return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
        }
        if (auto interruption = post_hook_interruption(request, pre_intent_idempotency(request))) {
            return std::move(*interruption);
        }
        if (auto* terminal = std::get_if<json>(&*planning)) {
            return rejected(std::move(*terminal));
        }
        auto proposed_plan = std::get<write_contract::Plan>(std::move(*planning));
        if (proposed_plan.operation() != request.operation ||
            proposed_plan.account() != request.account) {
            return audit_fatal();
        }

        auto epoch_result = foundation_->acquire_epoch(scan_control);
        if (auto* failure = std::get_if<AccountAuditFailure>(&epoch_result)) {
            return failure->interruption == AccountAuditFailure::Interruption::Deadline
                       ? rejected(timeout_terminal(request.operation, "preflight",
                                                   pre_intent_idempotency(request)))
                       : WriteKernelResult{WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
        }
        auto epoch = std::get<AccountAuditCoordinator::Guard>(std::move(epoch_result));
        const auto commit_sampled_now = request.sample_now();
        const auto gate = foundation_->run_core_gate(epoch, commit_sampled_now, {}, spool_control,
                                                     hooks.spool_hooks);
        if (gate.status != IdempotencyCoreGateStatus::Clean) {
            return gate_failure(request, gate, *foundation_);
        }
        if (request.idempotency_key_hash) {
            const auto lookup = IdempotencyStore::lookup(
                gate.snapshot, *request.idempotency_key_hash, admission->request_fingerprint);
            if (auto result =
                    incumbent_result(request, hooks, lookup, *request.idempotency_key_hash,
                                     admission->request_fingerprint)) {
                return std::move(*result);
            }
        }

        std::optional<ConfirmationSource> confirmation_source;
        if (destructive(request.operation)) {
            if (!hooks.confirm) {
                return audit_fatal();
            }
            WriteConfirmationOutcome confirmation;
            try {
                confirmation = hooks.confirm(proposed_plan, false);
            } catch (...) {
                return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
            }
            if (auto interruption =
                    post_hook_interruption(request, pre_intent_idempotency(request))) {
                return std::move(*interruption);
            }
            if (confirmation.status != WriteConfirmationStatus::ConfirmedYes &&
                confirmation.status != WriteConfirmationStatus::ConfirmedTty) {
                return {WriteKernelStatus::Rejected, confirmation.terminal, std::nullopt};
            }
            confirmation_source = confirmation.status == WriteConfirmationStatus::ConfirmedYes
                                      ? ConfirmationSource::Yes
                                      : ConfirmationSource::Tty;
        }

        if (request.authority_source == AuthoritySource::Config) {
            if (!hooks.verify_config_grant) {
                return audit_fatal();
            }
            const config::MutationControl control{request.deadline.expires_at,
                                                  request.cancellation_token};
            std::optional<config::GrantVerificationResult> verification;
            try {
                verification.emplace(
                    hooks.verify_config_grant(request.config_snapshot, request.account, control));
            } catch (...) {
                return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
            }
            if (auto interruption =
                    post_hook_interruption(request, pre_intent_idempotency(request))) {
                return std::move(*interruption);
            }
            if (verification->status != config::GrantVerificationStatus::Matched) {
                auto terminal = config_cas_terminal(request, *verification);
                if (!terminal || terminal->is_null()) {
                    return {WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
                }
                return rejected(std::move(*terminal));
            }
        }

        std::string contract_error;
        auto intent = make_account_audit_intent(
            {{request.invocation_id, request.intent_timestamp},
             request.account,
             *operation,
             admission->arguments.value(),
             proposed_plan.value(),
             admission->request_fingerprint.value(),
             request.config_snapshot,
             std::string(authority_source_name(request.authority_source)),
             confirmation_source ? std::optional<std::string>{std::string(
                                       confirmation_source_name(*confirmation_source))}
                                 : std::nullopt,
             request.idempotency_key_hash
                 ? std::optional<std::string>{request.idempotency_key_hash->value()}
                 : std::nullopt,
             request.request_source_bytes},
            contract_error);
        if (!intent) {
            return audit_fatal();
        }
        AccountAuditAppendPermit permit;
        const auto append_inspection = foundation_->audit().prepare_append(
            *intent, AccountAuditPinSource{IdempotencyStore::pins(gate.snapshot)}, epoch, permit);
        if (append_inspection.status != AccountAuditInspectionStatus::Clean || !permit.valid()) {
            IdempotencyCoreGateResult failure;
            failure.status = append_inspection.status == AccountAuditInspectionStatus::Interrupted
                                 ? IdempotencyCoreGateStatus::Interrupted
                                 : IdempotencyCoreGateStatus::AuditUnavailable;
            failure.audit_failure = append_inspection.failure;
            failure.terminal = append_inspection.terminal;
            return gate_failure(request, failure, *foundation_);
        }
        AccountAuditAppendReceipt receipt;
        AccountAuditFailure audit_failure;
        intent_may_be_durable = true;
        if (!foundation_->audit().append_intent(*intent, std::move(permit), epoch, receipt,
                                                audit_failure)) {
            return audit_fatal();
        }

        std::uint32_t checkpoint_sequence = 0;
        std::vector<AccountAuditStage> completed;
        if (request.idempotency_key_hash) {
            if (hooks.before_insert) {
                try {
                    hooks.before_insert(receipt, epoch);
                } catch (...) {
                    return audit_fatal();
                }
            }
            auto pending = make_idempotency_pending_entry(
                {*request.idempotency_key_hash, admission->request_fingerprint, *operation,
                 request.invocation_id, receipt.audit_generation, commit_sampled_now,
                 proposed_plan.value()},
                request.account, foundation_->store().path());
            if (std::holds_alternative<IdempotencyFailure>(pending)) {
                return audit_fatal();
            }
            const auto& entry = std::get<IdempotencyEntry>(pending);
            const auto inserted = foundation_->store().insert_if_absent(entry, epoch);
            if (inserted.status == IdempotencyInsertStatus::UnexpectedIncumbent) {
                auto closure = foundation_->close_unexpected_incumbent(receipt, epoch);
                if (closure.status == IdempotencyUnexpectedIncumbentClosureStatus::AuditFatal ||
                    !closure.terminal) {
                    return audit_fatal();
                }
                return {WriteKernelStatus::DurabilityFatal, std::move(closure.terminal),
                        std::nullopt};
            }
            if (inserted.status != IdempotencyInsertStatus::Inserted) {
                return audit_fatal();
            }
            if (!append_checkpoint(*foundation_, epoch, request, hooks, *operation,
                                   ++checkpoint_sequence, AccountAuditStage::IdempotencyPending,
                                   {{"key_hash", request.idempotency_key_hash->value()},
                                    {"request_fingerprint", admission->request_fingerprint.value()},
                                    {"expires_at", entry.expires_at},
                                    {"reserved_terminal_bytes", entry.reserved_terminal_bytes}},
                                   completed)) {
                return audit_fatal();
            }
        }

        WritePostIntentPreparation post_intent;
        std::optional<AccountAuditSpoolHold> current_spool_hold;
        if (hooks.post_intent) {
            try {
                post_intent = hooks.post_intent(proposed_plan, *admission);
            } catch (...) {
                return audit_fatal();
            }
        }
        if (post_intent.spool) {
            if (request.operation != proto::M3Operation::SavedAttach ||
                !validate_account_audit_persisted_spool(*post_intent.spool,
                                                        request.invocation_id) ||
                !append_checkpoint(*foundation_, epoch, request, hooks, *operation,
                                   ++checkpoint_sequence, AccountAuditStage::SpoolReady,
                                   {{"file", file_snapshot_json(post_intent.spool->file)},
                                    {"relative_path", post_intent.spool->relative_path}},
                                   completed)) {
                return audit_fatal();
            }
            if (request.idempotency_key_hash) {
                const auto updated = foundation_->store().update_spool(
                    *request.idempotency_key_hash, request.invocation_id, *post_intent.spool,
                    epoch);
                if (updated.status == IdempotencyWriteStatus::Failed) {
                    return audit_fatal();
                }
            }
            current_spool_hold = foundation_->audit().hold_current_spool(
                receipt, *post_intent.spool, epoch, audit_failure);
            if (!current_spool_hold) {
                return audit_fatal();
            }
        } else if (request.operation == proto::M3Operation::SavedAttach &&
                   !post_intent.terminal_without_dispatch) {
            return audit_fatal();
        }

        const auto cleanup_current_spool = [&](bool clear_completed_store) {
            if (!current_spool_hold) {
                return true;
            }
            try {
                auto cleanup =
                    hooks.cleanup_spool
                        ? hooks.cleanup_spool(std::move(*current_spool_hold), epoch)
                        : cleanup_spool_file_with_hold(std::move(*current_spool_hold), epoch);
                current_spool_hold.reset();
                auto* release = std::get_if<AccountAuditSpoolReleaseReceipt>(&cleanup);
                if (release == nullptr || !foundation_->audit().release_current_spool(
                                              std::move(*release), receipt, epoch, audit_failure)) {
                    return false;
                }
                if (clear_completed_store && request.idempotency_key_hash) {
                    const auto cleared = foundation_->store().clear_spool(
                        *request.idempotency_key_hash, request.invocation_id, epoch);
                    if (cleared.status == IdempotencyWriteStatus::Failed) {
                        return false;
                    }
                }
                return true;
            } catch (...) {
                current_spool_hold.reset();
                return false;
            }
        };

        const auto finish_without_dispatch = [&](const write_contract::StoredTerminal& terminal,
                                                 bool deliver_terminal = true,
                                                 bool complete_without_mutation = false) {
            if (!write_contract::terminal_matches_plan(terminal, proposed_plan) ||
                !append_outcome(*foundation_, epoch, request, hooks, *operation, terminal,
                                AccountAuditMutationState::None, completed)) {
                return audit_fatal();
            }
            if (request.idempotency_key_hash) {
                const auto transition =
                    complete_without_mutation
                        ? foundation_->store().complete(*request.idempotency_key_hash,
                                                        request.invocation_id, terminal.value(),
                                                        epoch)
                        : foundation_->store().remove_owned(*request.idempotency_key_hash,
                                                            request.invocation_id, epoch);
                if (transition.status == IdempotencyWriteStatus::Failed) {
                    return audit_fatal();
                }
            }
            static_cast<void>(cleanup_current_spool(false));
            return WriteKernelResult{
                deliver_terminal ? WriteKernelStatus::Completed : WriteKernelStatus::Rejected,
                deliver_terminal ? std::optional<json>{terminal.value()} : std::nullopt,
                proposed_plan};
        };

        if (post_intent.terminal_without_dispatch) {
            const bool valid_noop = post_intent.complete_without_mutation &&
                                    request.operation == proto::M3Operation::ChatMarkRead &&
                                    proposed_plan.value()["tdlib_request"].is_null() &&
                                    proposed_plan.value()["last_message_id"].is_null() &&
                                    post_intent.terminal_without_dispatch->success();
            if (post_intent.complete_without_mutation != valid_noop) {
                return audit_fatal();
            }
            return finish_without_dispatch(*post_intent.terminal_without_dispatch, true,
                                           valid_noop);
        }

        if (!hooks.revalidate_auth_and_schedule || !hooks.dispatch) {
            return audit_fatal();
        }
        if (deadline_expired(request.deadline)) {
            auto terminal = write_contract::make_error_terminal(
                request.operation, "TIMEOUT", "request timed out",
                {{"operation", operation_name(request.operation)},
                 {"phase", "preflight"},
                 {"state", "ready"},
                 {"outcome", "not_started"},
                 {"idempotency", post_intent_idempotency(request)}},
                kTimeout, contract_error);
            return terminal ? finish_without_dispatch(*terminal) : audit_fatal();
        }
        if (cancelled(request)) {
            const auto terminal = cancellation_terminal(request.operation);
            return terminal ? finish_without_dispatch(*terminal, false) : audit_fatal();
        }
        std::optional<WriteDispatchAdmissionOutcome> dispatch_admission;
        try {
            dispatch_admission.emplace(hooks.revalidate_auth_and_schedule(proposed_plan));
        } catch (...) {
            return audit_fatal();
        }
        if (auto* terminal = std::get_if<write_contract::StoredTerminal>(&*dispatch_admission)) {
            return finish_without_dispatch(*terminal);
        }
        if (std::holds_alternative<WriteDispatchStopped>(*dispatch_admission)) {
            const auto terminal = cancellation_terminal(request.operation);
            return terminal ? finish_without_dispatch(*terminal, false) : audit_fatal();
        }
        auto dispatch_preparation =
            std::get<WriteDispatchPreparation>(std::move(*dispatch_admission));
        if (deadline_expired(request.deadline)) {
            auto terminal = write_contract::make_error_terminal(
                request.operation, "TIMEOUT", "request timed out",
                {{"operation", operation_name(request.operation)},
                 {"phase", "preflight"},
                 {"state", "ready"},
                 {"outcome", "not_started"},
                 {"idempotency", post_intent_idempotency(request)}},
                kTimeout, contract_error);
            return terminal ? finish_without_dispatch(*terminal) : audit_fatal();
        }
        if (cancelled(request)) {
            const auto terminal = cancellation_terminal(request.operation);
            return terminal ? finish_without_dispatch(*terminal, false) : audit_fatal();
        }
        if (!append_checkpoint(*foundation_, epoch, request, hooks, *operation,
                               ++checkpoint_sequence, AccountAuditStage::DispatchStarted,
                               dispatch_preparation.proof, completed)) {
            return audit_fatal();
        }
        WriteDurableObservationSink observations([&](AccountAuditStage stage, json value) {
            if (stage == AccountAuditStage::TemporaryIdsObserved) {
                if (!append_checkpoint(*foundation_, epoch, request, hooks, *operation,
                                       ++checkpoint_sequence, stage,
                                       {{"temporary_message_ids", value}}, completed)) {
                    return false;
                }
                if (request.idempotency_key_hash) {
                    const auto updated = foundation_->store().update_temporary_message_ids(
                        *request.idempotency_key_hash, request.invocation_id, value, epoch);
                    if (updated.status == IdempotencyWriteStatus::Failed) {
                        return false;
                    }
                }
                return true;
            }
            if (stage != AccountAuditStage::ForwardProgress ||
                !append_checkpoint(*foundation_, epoch, request, hooks, *operation,
                                   ++checkpoint_sequence, stage, {{"items", value}}, completed)) {
                return false;
            }
            if (request.idempotency_key_hash) {
                const auto updated = foundation_->store().update_forward_progress(
                    *request.idempotency_key_hash, request.invocation_id, value, epoch);
                if (updated.status == IdempotencyWriteStatus::Failed) {
                    return false;
                }
            }
            return true;
        });
        std::optional<WriteDispatchOutcome> dispatch;
        try {
            dispatch.emplace(hooks.dispatch(proposed_plan, dispatch_preparation, observations));
        } catch (...) {
            return audit_fatal();
        }
        if (!observations.durable()) {
            return audit_fatal();
        }
        if (dispatch->terminal.operation() != request.operation ||
            !write_contract::terminal_matches_plan(dispatch->terminal, proposed_plan) ||
            (dispatch->retain_pending &&
             (request.operation != proto::M3Operation::MsgForward ||
              dispatch->mutation_state == AccountAuditMutationState::None))) {
            return audit_fatal();
        }
        const bool suppress_terminal = cancelled(request);
        if (suppress_terminal && dispatch->mutation_state != AccountAuditMutationState::None) {
            return {WriteKernelStatus::Rejected, std::nullopt, std::move(proposed_plan)};
        }
        if (dispatch->mutation_confirmed &&
            !append_checkpoint(*foundation_, epoch, request, hooks, *operation,
                               ++checkpoint_sequence, AccountAuditStage::MutationConfirmed,
                               {{"terminal", dispatch->terminal.value()}}, completed)) {
            return audit_fatal();
        }
        if (!append_outcome(*foundation_, epoch, request, hooks, *operation, dispatch->terminal,
                            dispatch->mutation_state, completed)) {
            return audit_fatal();
        }
        if (request.idempotency_key_hash) {
            IdempotencyWriteResult transition;
            if (dispatch->mutation_state == AccountAuditMutationState::None) {
                transition = foundation_->store().remove_owned(*request.idempotency_key_hash,
                                                               request.invocation_id, epoch);
            } else if (dispatch->mutation_state == AccountAuditMutationState::Confirmed &&
                       !dispatch->retain_pending) {
                transition = foundation_->store().complete(*request.idempotency_key_hash,
                                                           request.invocation_id,
                                                           dispatch->terminal.value(), epoch);
            } else {
                transition.status = IdempotencyWriteStatus::Unchanged;
            }
            if (transition.status == IdempotencyWriteStatus::Failed) {
                return audit_fatal();
            }
        }
        if (dispatch->mutation_state != AccountAuditMutationState::Possible &&
            !dispatch->retain_pending) {
            static_cast<void>(cleanup_current_spool(dispatch->mutation_state ==
                                                    AccountAuditMutationState::Confirmed));
        }
        if (hooks.cleanup) {
            bool cleanup_completed = false;
            try {
                cleanup_completed = hooks.cleanup(proposed_plan, *dispatch);
            } catch (...) {
                cleanup_completed = false;
            }
            static_cast<void>(cleanup_completed);
        }
        return {suppress_terminal ? WriteKernelStatus::Rejected : WriteKernelStatus::Completed,
                suppress_terminal ? std::optional<json>{}
                                  : std::optional<json>{dispatch->terminal.value()},
                std::move(proposed_plan)};
    } catch (...) {
        return intent_may_be_durable
                   ? audit_fatal()
                   : WriteKernelResult{WriteKernelStatus::Rejected, std::nullopt, std::nullopt};
    }
}

} // namespace tgcli::daemon
