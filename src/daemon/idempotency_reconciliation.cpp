#include "daemon/idempotency_reconciliation.hpp"

#include "common/daemon_lock.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;
using PinIdentity = std::tuple<std::uint64_t, std::string, std::string, AccountAuditOperation>;

json audit_incomplete_terminal(std::string_view account, std::string_view audit_path,
                               AccountAuditMutationState mutation,
                               const std::vector<AccountAuditStage>& stages) {
    json completed = json::array();
    for (const auto stage : stages) {
        completed.push_back(account_audit_stage_name(stage));
    }
    return {{"kind", "error"},
            {"code", "AUDIT_INCOMPLETE"},
            {"message", "a prior audited invocation did not reach a terminal proof"},
            {"details",
             {{"account", account},
              {"path", audit_path},
              {"mutation_state", account_audit_mutation_state_name(mutation)},
              {"completed_stages", std::move(completed)}}},
            {"exit_code", 1}};
}

std::string current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (::gmtime_r(&seconds, &utc) == nullptr) {
        return {};
    }
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
    }
    return buffer.data();
}

std::string recovery_timestamp(const IdempotencyRecoveryTimestamp& timestamp) {
    return timestamp ? timestamp() : current_timestamp();
}

void notify_boundary(const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks,
                     std::string_view boundary) {
    if (hooks && hooks->after_boundary) {
        hooks->after_boundary(boundary);
    }
}

IdempotencyEntry* find_entry(IdempotencySnapshot& snapshot, std::string_view key_hash) {
    const auto found =
        std::ranges::lower_bound(snapshot.entries, key_hash, {}, [](const IdempotencyEntry& entry) {
            return std::string_view(entry.key_hash.value());
        });
    return found == snapshot.entries.end() || found->key_hash.value() != key_hash ? nullptr
                                                                                  : &*found;
}

const IdempotencyEntry* find_entry(const IdempotencySnapshot& snapshot, std::string_view key_hash) {
    const auto found =
        std::ranges::lower_bound(snapshot.entries, key_hash, {}, [](const IdempotencyEntry& entry) {
            return std::string_view(entry.key_hash.value());
        });
    return found == snapshot.entries.end() || found->key_hash.value() != key_hash ? nullptr
                                                                                  : &*found;
}

bool erase_entry(IdempotencySnapshot& snapshot, std::string_view key_hash) {
    const auto found =
        std::ranges::lower_bound(snapshot.entries, key_hash, {}, [](const IdempotencyEntry& entry) {
            return std::string_view(entry.key_hash.value());
        });
    if (found == snapshot.entries.end() || found->key_hash.value() != key_hash) {
        return false;
    }
    snapshot.entries.erase(found);
    return true;
}

bool array_prefix(const json& prefix, const json& complete) {
    if (!prefix.is_array() || !complete.is_array() || prefix.size() > complete.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (prefix.at(index) != complete.at(index)) {
            return false;
        }
    }
    return true;
}

bool forward_can_advance(const json& before, const json& after) {
    if (!before.is_array() || !after.is_array() || before.size() != after.size()) {
        return false;
    }
    for (std::size_t index = 0; index < before.size(); ++index) {
        const auto& prior = before.at(index);
        const auto& next = after.at(index);
        if (!prior.is_object() || !next.is_object() || prior["source_id"] != next["source_id"] ||
            (prior["status"] != "pending" && prior != next) ||
            (prior["status"] == "pending" && next["status"] == "pending" && prior != next)) {
            return false;
        }
    }
    return true;
}

bool pending_payload_matches(const IdempotencyEntry& entry, const json& payload) {
    return payload.is_object() && payload.size() == 4 && payload.contains("key_hash") &&
           payload.contains("request_fingerprint") && payload.contains("expires_at") &&
           payload.contains("reserved_terminal_bytes") &&
           payload["key_hash"] == entry.key_hash.value() &&
           payload["request_fingerprint"] == entry.request_fingerprint.value() &&
           payload["expires_at"] == entry.expires_at &&
           payload["reserved_terminal_bytes"] == entry.reserved_terminal_bytes;
}

bool completed_terminal_retains_pending(const AccountAuditCompletedGroupView& view) {
    if (!view.outcome) {
        return true;
    }
    const auto& outcome = *view.outcome;
    if (outcome["mutation_state"] == "possible") {
        return true;
    }
    return view.operation == AccountAuditOperation::MsgForward &&
           std::ranges::any_of(view.forward_progress, [](const json& item) {
               return item.is_object() && item.value("status", std::string{}) == "pending";
           });
}

bool completed_spool_is_immediately_eligible(const AccountAuditCompletedGroupView& view,
                                             std::uint64_t sampled_now, bool sampled_now_valid) {
    if (!view.spool || !view.outcome) {
        return false;
    }
    if (!completed_terminal_retains_pending(view)) {
        return true;
    }
    if (!sampled_now_valid) {
        return false;
    }
    if (view.idempotency_key_hash && view.idempotency_pending &&
        (*view.idempotency_pending)["expires_at"].is_number_unsigned()) {
        return sampled_now >= (*view.idempotency_pending)["expires_at"].get<std::uint64_t>();
    }
    if (view.intent_unix_seconds < 0 ||
        static_cast<std::uint64_t>(view.intent_unix_seconds) >
            kIdempotencyMaximumUnixSeconds - kIdempotencyRetentionSeconds) {
        return false;
    }
    return sampled_now >=
           static_cast<std::uint64_t>(view.intent_unix_seconds) + kIdempotencyRetentionSeconds;
}

struct SpoolFact {
    std::uint64_t audit_generation = 0;
    std::string invocation_id;
    SpoolRef spool;
    std::optional<std::string> key_hash;
    bool retain = true;
};

struct CompletedRelationState {
    IdempotencySnapshot desired;
    std::map<PinIdentity, std::string> pin_keys;
    std::set<PinIdentity> seen;
    std::vector<SpoolFact> spools;
    std::string contradiction;
    std::uint64_t sampled_now = 0;
    bool sampled_now_valid = false;
    std::string account;
};

void select_contradiction(CompletedRelationState& state, std::string detail) {
    if (state.contradiction.empty()) {
        state.contradiction = std::move(detail);
    }
}

bool apply_audit_progress(IdempotencyEntry& entry, const json& temporary_ids,
                          const json& forward_progress, CompletedRelationState& state) {
    if (entry.state == IdempotencyEntryState::Completed) {
        return entry.temporary_message_ids.empty() && entry.forward_progress.empty();
    }
    if (entry.temporary_message_ids != temporary_ids) {
        if (!array_prefix(entry.temporary_message_ids, temporary_ids)) {
            select_contradiction(state, "store temporary ids are ahead of or conflict with audit");
            return false;
        }
        entry.temporary_message_ids = temporary_ids;
    }
    if (entry.forward_progress != forward_progress) {
        const bool advances = entry.forward_progress.empty() ||
                              forward_can_advance(entry.forward_progress, forward_progress);
        if (!advances || forward_progress.empty()) {
            select_contradiction(state,
                                 "store forward progress is ahead of or conflicts with audit");
            return false;
        }
        entry.forward_progress = forward_progress;
    }
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact audit/store relation matrix.
void consume_completed_view(const AccountAuditCompletedGroupView& view,
                            CompletedRelationState& state) {
    if (!state.contradiction.empty()) {
        return;
    }
    if (!view.outcome) {
        select_contradiction(state, "completed audit view has no outcome");
        return;
    }
    const auto mutation = (*view.outcome)["mutation_state"].get<std::string>();
    if (view.idempotency_key_hash && !view.idempotency_pending && mutation != "none") {
        select_contradiction(state, "completed keyed mutation has no idempotency checkpoint");
        return;
    }
    if (view.idempotency_key_hash && view.idempotency_pending && mutation != "none") {
        const auto expires_at = (*view.idempotency_pending)["expires_at"].get<std::uint64_t>();
        const auto* entry = find_entry(state.desired, *view.idempotency_key_hash);
        if (state.sampled_now_valid && state.sampled_now < expires_at &&
            (entry == nullptr || entry->invocation_id != view.invocation_id ||
             entry->audit_generation != view.audit_generation ||
             entry->request_fingerprint.value() != view.request_fingerprint ||
             entry->operation != view.operation)) {
            select_contradiction(state, "unexpired completed audit group lost its store entry");
            return;
        }
    }
    const PinIdentity identity{view.audit_generation, view.invocation_id, view.request_fingerprint,
                               view.operation};
    const auto pin = state.pin_keys.find(identity);
    if (pin != state.pin_keys.end()) {
        state.seen.emplace(identity);
        if (!view.idempotency_key_hash || *view.idempotency_key_hash != pin->second) {
            select_contradiction(state, "store pin resolves to another audit key");
            return;
        }
        auto* entry = find_entry(state.desired, pin->second);
        if (entry == nullptr || entry->invocation_id != view.invocation_id ||
            entry->audit_generation != view.audit_generation ||
            entry->request_fingerprint.value() != view.request_fingerprint ||
            entry->operation != view.operation || entry->plan != view.plan) {
            select_contradiction(state, "store entry immutable fields disagree with audit");
            return;
        }
        if (view.idempotency_pending) {
            if (entry->state == IdempotencyEntryState::Pending &&
                !pending_payload_matches(*entry, *view.idempotency_pending)) {
                select_contradiction(state, "store entry disagrees with idempotency checkpoint");
                return;
            }
        }
        if (entry->spool && (!view.spool || *entry->spool != *view.spool)) {
            select_contradiction(state, "store spool reference disagrees with audit");
            return;
        }
        if (entry->state == IdempotencyEntryState::Pending && !entry->spool && view.spool) {
            entry->spool = view.spool;
        }
        if (!apply_audit_progress(*entry, view.temporary_message_ids, view.forward_progress,
                                  state)) {
            return;
        }
        if (mutation == "none") {
            static_cast<void>(erase_entry(state.desired, pin->second));
        } else if (!completed_terminal_retains_pending(view)) {
            const auto& terminal = (*view.outcome)["terminal"];
            if (entry->state == IdempotencyEntryState::Completed) {
                if (!entry->terminal || *entry->terminal != terminal) {
                    select_contradiction(state, "completed store terminal disagrees with audit");
                    return;
                }
            } else {
                entry->state = IdempotencyEntryState::Completed;
                entry->reserved_terminal_bytes = 0;
                entry->temporary_message_ids = json::array();
                entry->forward_progress = json::array();
                entry->terminal = terminal;
            }
        } else if (entry->state != IdempotencyEntryState::Pending) {
            select_contradiction(state, "dispatch-unknown audit group has a terminal store entry");
            return;
        }
    }
    if (view.spool) {
        bool retain = completed_terminal_retains_pending(view) &&
                      !completed_spool_is_immediately_eligible(view, state.sampled_now,
                                                               state.sampled_now_valid);
        if (view.idempotency_key_hash) {
            if (const auto* entry = find_entry(state.desired, *view.idempotency_key_hash);
                entry != nullptr && entry->invocation_id == view.invocation_id &&
                entry->state == IdempotencyEntryState::Pending) {
                retain = true;
            }
        }
        state.spools.push_back({view.audit_generation, view.invocation_id, *view.spool,
                                view.idempotency_key_hash, retain});
    }
}

std::optional<SpoolRef> open_group_spool(const AccountAuditOpenGroup& group) {
    const auto found = std::ranges::find_if(
        group.checkpoints.rbegin(), group.checkpoints.rend(),
        [](const json& checkpoint) { return checkpoint["stage"] == "spool_ready"; });
    if (found == group.checkpoints.rend()) {
        return std::nullopt;
    }
    const auto& data = (*found)["data"];
    if (!data.is_object() || !data.contains("relative_path") || !data.contains("file")) {
        return std::nullopt;
    }
    const auto& file = data["file"];
    SpoolRef spool{.relative_path = data["relative_path"].get<std::string>(),
                   .file = {.path = file["path"].get<std::string>(),
                            .name = file["name"].get<std::string>(),
                            .size = file["size"].get<std::uint64_t>(),
                            .sha256 = file["sha256"].get<std::string>(),
                            .device = file["device"].get<std::uint64_t>(),
                            .inode = file["inode"].get<std::uint64_t>(),
                            .mtime_ns = file["mtime_ns"].get<std::int64_t>(),
                            .ctime_ns = file["ctime_ns"].get<std::int64_t>()}};
    return valid_spool_reference(spool, group.intent["invocation_id"].get_ref<const std::string&>())
               ? std::optional<SpoolRef>(std::move(spool))
               : std::nullopt;
}

const json* latest_checkpoint_data(const AccountAuditOpenGroup& group, std::string_view stage) {
    const auto found =
        std::ranges::find_if(group.checkpoints.rbegin(), group.checkpoints.rend(),
                             [&](const json& checkpoint) { return checkpoint["stage"] == stage; });
    return found == group.checkpoints.rend() ? nullptr : &(*found)["data"];
}

bool reconcile_open_relation(const AccountAuditOpenGroup& group, CompletedRelationState& state) {
    const auto operation =
        parse_account_audit_operation(group.intent["command"].get_ref<const std::string&>());
    if (!operation) {
        select_contradiction(state, "open audit group lost its operation");
        return false;
    }
    if (group.intent["idempotency_key_hash"].is_null()) {
        return true;
    }
    const auto key = group.intent["idempotency_key_hash"].get<std::string>();
    const auto parsed_key = parse_idempotency_key_hash(key);
    if (!parsed_key) {
        select_contradiction(state, "open audit group has invalid key hash");
        return false;
    }
    auto* entry = find_entry(state.desired, key);
    const auto* pending = latest_checkpoint_data(group, "idempotency_pending");
    if (entry == nullptr) {
        if (pending != nullptr) {
            select_contradiction(state, "idempotency checkpoint has no matching store entry");
            return false;
        }
        return true;
    }
    if (entry->invocation_id != group.intent["invocation_id"].get_ref<const std::string&>()) {
        if (pending != nullptr) {
            select_contradiction(state, "open audit group checkpoint points at an incumbent");
            return false;
        }
        return true;
    }
    const PinIdentity identity{group.audit_generation, entry->invocation_id,
                               entry->request_fingerprint.value(), entry->operation};
    state.seen.emplace(identity);
    if (entry->state != IdempotencyEntryState::Pending ||
        entry->audit_generation != group.audit_generation || entry->operation != *operation ||
        entry->request_fingerprint.value() !=
            group.intent["request_fingerprint"].get_ref<const std::string&>() ||
        entry->plan != group.intent["plan"] ||
        (pending != nullptr && !pending_payload_matches(*entry, *pending))) {
        select_contradiction(state, "open audit group disagrees with its store entry");
        return false;
    }
    if (const auto spool = open_group_spool(group)) {
        if (entry->spool && *entry->spool != *spool) {
            select_contradiction(state, "open audit spool disagrees with store");
            return false;
        }
        if (!entry->spool) {
            entry->spool = *spool;
        }
    } else if (entry->spool) {
        select_contradiction(state, "store spool is ahead of open audit group");
        return false;
    }
    const auto* temporary = latest_checkpoint_data(group, "temporary_ids_observed");
    const auto* progress = latest_checkpoint_data(group, "forward_progress");
    return apply_audit_progress(
        *entry, temporary != nullptr ? (*temporary)["temporary_message_ids"] : json::array(),
        progress != nullptr ? (*progress)["items"] : json::array(), state);
}

std::vector<ExpectedSpoolObject> expected_spools(const CompletedRelationState& relations,
                                                 const AccountAuditOpenGroup* open) {
    std::map<std::string, std::string, std::less<>> by_invocation;
    for (const auto& fact : relations.spools) {
        by_invocation.emplace(fact.invocation_id, fact.spool.file.name);
    }
    if (open != nullptr) {
        if (const auto spool = open_group_spool(*open)) {
            by_invocation.emplace(open->intent["invocation_id"].get<std::string>(),
                                  spool->file.name);
        }
    }
    std::vector<ExpectedSpoolObject> result;
    result.reserve(by_invocation.size());
    for (auto& [invocation, name] : by_invocation) {
        result.push_back({invocation, std::move(name)});
    }
    return result;
}

AccountAuditSpoolHold* find_hold(std::vector<AccountAuditSpoolHold>& holds, const SpoolFact& fact) {
    const auto found = std::ranges::find_if(holds, [&](const AccountAuditSpoolHold& hold) {
        return hold.valid() && hold.audit_generation() == fact.audit_generation &&
               hold.invocation_id() == fact.invocation_id && hold.spool() == fact.spool;
    });
    return found == holds.end() ? nullptr : &*found;
}

IdempotencyCoreGateResult spool_failure_result(FileSpoolError failure) {
    IdempotencyCoreGateResult result;
    result.status = IdempotencyCoreGateStatus::SpoolUnavailable;
    result.spool_failure = std::move(failure);
    return result;
}

IdempotencyCoreGateResult store_failure_result(IdempotencyFailure failure) {
    IdempotencyCoreGateResult result;
    result.status = failure.interruption ? IdempotencyCoreGateStatus::Interrupted
                                         : IdempotencyCoreGateStatus::StoreUnavailable;
    result.store_failure = std::move(failure);
    return result;
}

IdempotencyCoreGateResult audit_failure_result(const AccountAuditInspection& inspection) {
    IdempotencyCoreGateResult result;
    result.audit_failure = inspection.failure;
    result.terminal = inspection.terminal;
    switch (inspection.status) {
    case AccountAuditInspectionStatus::Interrupted:
        result.status = IdempotencyCoreGateStatus::Interrupted;
        break;
    case AccountAuditInspectionStatus::Contradiction:
    case AccountAuditInspectionStatus::LegacyOpen:
        result.status = IdempotencyCoreGateStatus::AuditIncomplete;
        break;
    case AccountAuditInspectionStatus::Unavailable:
        result.status = IdempotencyCoreGateStatus::AuditUnavailable;
        break;
    case AccountAuditInspectionStatus::Clean:
    case AccountAuditInspectionStatus::Open:
        result.status = IdempotencyCoreGateStatus::AuditIncomplete;
        break;
    }
    return result;
}

} // namespace

json unexpected_idempotency_incumbent_terminal(AccountAuditOperation operation) {
    return {
        {"kind", "error"},
        {"code", "INTERNAL"},
        {"message", "internal error"},
        {"details",
         {{"operation", account_audit_operation_name(operation)}, {"reason", "internal_error"}}},
        {"exit_code", 1}};
}

IdempotencyFoundation::IdempotencyFoundation(
    std::string state_directory, std::string account, uid_t expected_uid,
    std::shared_ptr<AccountAuditCoordinator> coordinator, IdempotencyStore store,
    std::shared_ptr<const testing::AccountAuditHooks> audit_hooks)
    : state_directory_(std::move(state_directory)), account_(std::move(account)),
      expected_uid_(expected_uid), coordinator_(std::move(coordinator)), store_(std::move(store)),
      audit_(state_directory_, account_, expected_uid_, std::move(audit_hooks)) {}

std::variant<IdempotencyFoundation, IdempotencyFailure>
IdempotencyFoundation::create(std::string state_directory, std::string account, uid_t expected_uid,
                              std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease,
                              std::shared_ptr<const testing::AccountAuditHooks> audit_hooks,
                              std::shared_ptr<const testing::IdempotencyStoreHooks> store_hooks) {
    auto store_result =
        IdempotencyStore::create(state_directory, account, expected_uid, std::move(store_hooks));
    if (auto* failure = std::get_if<IdempotencyFailure>(&store_result)) {
        return std::move(*failure);
    }
    auto store = std::move(std::get<IdempotencyStore>(store_result));
    std::string error;
    auto coordinator = AccountAuditCoordinator::create(state_directory, account, expected_uid,
                                                       std::move(daemon_lock_lease), error);
    if (!coordinator) {
        return IdempotencyFailure{AccountAuditDurabilityReason::LockFailed,
                                  std::move(account),
                                  store.path(),
                                  std::move(error),
                                  {}};
    }
    return IdempotencyFoundation(std::move(state_directory), std::move(account), expected_uid,
                                 std::move(coordinator), std::move(store), std::move(audit_hooks));
}

AccountAuditCoordinator::Guard IdempotencyFoundation::acquire_epoch() {
    return coordinator_->lock();
}

IdempotencyFoundation::EpochResult
IdempotencyFoundation::acquire_epoch(AccountAuditScanControl control) {
    return coordinator_->lock(std::move(control));
}

IdempotencyStore& IdempotencyFoundation::store() {
    return store_;
}

AccountAuditLog& IdempotencyFoundation::audit() {
    return audit_;
}

const std::shared_ptr<AccountAuditCoordinator>& IdempotencyFoundation::coordinator() const {
    return coordinator_;
}

IdempotencyUnexpectedIncumbentClosure
IdempotencyFoundation::close_unexpected_incumbent(const AccountAuditAppendReceipt& intent_receipt,
                                                  const AccountAuditCoordinator::Guard& guard,
                                                  const IdempotencyRecoveryTimestamp& timestamp) {
    const auto inspection = audit_.inspect(guard);
    if (inspection.status != AccountAuditInspectionStatus::Open || !inspection.oldest_open) {
        auto failure = inspection.failure;
        if (failure.detail.empty()) {
            failure = {AccountAuditDurabilityReason::Contradiction,
                       "unexpected-incumbent closure has no exact open intent"};
        }
        return {IdempotencyUnexpectedIncumbentClosureStatus::AuditFatal, {}, std::move(failure)};
    }
    const auto& open = *inspection.oldest_open;
    const auto operation =
        parse_account_audit_operation(open.intent["command"].get_ref<const std::string&>());
    if (intent_receipt.audit_generation == 0 ||
        open.audit_generation != intent_receipt.audit_generation ||
        open.intent["invocation_id"] != intent_receipt.invocation_id ||
        open.intent["request_fingerprint"] != intent_receipt.request_fingerprint || !operation ||
        *operation != intent_receipt.operation ||
        intent_receipt.operation == AccountAuditOperation::SessionTerminate ||
        !open.checkpoints.empty()) {
        return {IdempotencyUnexpectedIncumbentClosureStatus::AuditFatal,
                {},
                {AccountAuditDurabilityReason::Contradiction,
                 "unexpected-incumbent closure receipt does not match the open intent"}};
    }
    const auto terminal = unexpected_idempotency_incumbent_terminal(intent_receipt.operation);
    std::string error;
    auto outcome =
        make_account_audit_outcome({{intent_receipt.invocation_id, recovery_timestamp(timestamp)},
                                    account_,
                                    intent_receipt.operation,
                                    false,
                                    AccountAuditMutationState::None,
                                    {},
                                    terminal},
                                   error);
    if (!outcome) {
        return {IdempotencyUnexpectedIncumbentClosureStatus::AuditFatal,
                {},
                {AccountAuditDurabilityReason::SchemaError, std::move(error)}};
    }
    AccountAuditFailure failure;
    if (!audit_.append_outcome(*outcome, guard, failure)) {
        return {IdempotencyUnexpectedIncumbentClosureStatus::AuditFatal, {}, std::move(failure)};
    }
    return {IdempotencyUnexpectedIncumbentClosureStatus::DurableFatal, terminal, {}};
}

namespace {

IdempotencyCoreGateResult relation_failure(const IdempotencyStore& store, std::string detail) {
    return store_failure_result({AccountAuditDurabilityReason::Contradiction,
                                 store.account(),
                                 store.path(),
                                 std::move(detail),
                                 {}});
}

IdempotencyCoreGateResult spool_contradiction(std::string_view account,
                                              std::string_view audit_path) {
    IdempotencyCoreGateResult result;
    result.status = IdempotencyCoreGateStatus::AuditIncomplete;
    result.terminal =
        audit_incomplete_terminal(account, audit_path, AccountAuditMutationState::None, {});
    result.audit_failure = {AccountAuditDurabilityReason::Contradiction,
                            "spool inventory contradicts durable audit/store facts"};
    return result;
}

bool contains_string(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

std::optional<IdempotencyCoreGateResult>
validate_spool_relation(const SpoolInventory& inventory, const CompletedRelationState& relations,
                        const AccountAuditOpenGroup* open, std::string_view account,
                        std::string_view audit_path) {
    auto reconciliation_result =
        reconcile_spool_inventory(inventory, expected_spools(relations, open));
    if (auto* error = std::get_if<FileSpoolError>(&reconciliation_result)) {
        return spool_failure_result(std::move(*error));
    }
    const auto& reconciliation = std::get<SpoolReconciliation>(reconciliation_result);
    if (reconciliation.contradiction) {
        return spool_contradiction(account, audit_path);
    }
    for (const auto& fact : relations.spools) {
        const bool missing =
            std::ranges::any_of(reconciliation.missing, [&](const ExpectedSpoolObject& item) {
                return item.invocation_id == fact.invocation_id;
            });
        if (fact.retain &&
            (!contains_string(reconciliation.ready_invocations, fact.invocation_id) || missing)) {
            return spool_contradiction(account, audit_path);
        }
    }
    if (open != nullptr && open->has_spool && open->dispatch_started) {
        const auto invocation = open->intent["invocation_id"].get<std::string>();
        if (!contains_string(reconciliation.ready_invocations, invocation)) {
            return spool_contradiction(account, audit_path);
        }
    }
    return std::nullopt;
}

std::optional<IdempotencyCoreGateResult>
cleanup_fact(const SpoolFact& fact, std::vector<AccountAuditSpoolHold>& holds,
             AccountAuditRecoveryPermit& permit, IdempotencyStore* store,
             const AccountAuditCoordinator::Guard& guard, const FileSpoolControl& spool_control,
             const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks,
             const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks) {
    auto* hold = find_hold(holds, fact);
    if (hold == nullptr) {
        return relation_failure(*store, "audit spool hold is missing");
    }
    auto cleanup =
        cleanup_spool_file_with_hold(std::move(*hold), guard, spool_control, spool_hooks);
    if (auto* error = std::get_if<FileSpoolError>(&cleanup)) {
        return spool_failure_result(std::move(*error));
    }
    auto receipt = std::get<AccountAuditSpoolReleaseReceipt>(std::move(cleanup));
    notify_boundary(hooks, "spool_cleanup_synced");
    if (store != nullptr && fact.key_hash) {
        const auto key = parse_idempotency_key_hash(*fact.key_hash);
        if (!key) {
            return relation_failure(*store, "spool cleanup lost its key hash");
        }
        const auto inspection = store->inspect(guard);
        if (inspection.status != IdempotencyInspectionStatus::Clean) {
            return store_failure_result(inspection.failure);
        }
        const auto* entry = find_entry(inspection.snapshot, *fact.key_hash);
        if (entry != nullptr && entry->invocation_id == fact.invocation_id &&
            entry->state == IdempotencyEntryState::Completed && entry->spool) {
            auto cleared = store->clear_spool(*key, fact.invocation_id, guard);
            if (cleared.status == IdempotencyWriteStatus::Failed) {
                return store_failure_result(std::move(cleared.failure));
            }
            notify_boundary(hooks, "store_spool_cleared");
        }
    }
    AccountAuditFailure release_failure;
    if (!permit.release_spool_hold(std::move(receipt), guard, release_failure)) {
        IdempotencyCoreGateResult result;
        result.status = IdempotencyCoreGateStatus::AuditUnavailable;
        result.audit_failure = std::move(release_failure);
        return result;
    }
    notify_boundary(hooks, "spool_hold_released");
    return std::nullopt;
}

std::optional<IdempotencyCoreGateResult> append_recovery_checkpoint(
    AccountAuditLog& audit, const AccountAuditOpenGroup& group, AccountAuditOperation operation,
    std::uint32_t sequence, const json& terminal, const AccountAuditCoordinator::Guard& guard,
    const IdempotencyRecoveryTimestamp& timestamp,
    const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks) {
    std::string error;
    auto checkpoint = make_account_audit_checkpoint(
        {{group.intent["invocation_id"].get<std::string>(), recovery_timestamp(timestamp)},
         group.intent["account"].get<std::string>(),
         operation,
         sequence,
         AccountAuditStage::MutationConfirmed,
         {{"terminal", terminal}}},
        error);
    AccountAuditFailure failure;
    if (!checkpoint || !audit.append_checkpoint(*checkpoint, guard, failure)) {
        IdempotencyCoreGateResult result;
        result.status = IdempotencyCoreGateStatus::AuditIncomplete;
        result.audit_failure =
            failure.detail.empty()
                ? AccountAuditFailure{AccountAuditDurabilityReason::SchemaError, std::move(error)}
                : std::move(failure);
        return result;
    }
    notify_boundary(hooks, "recovery_mutation_proof_synced");
    return std::nullopt;
}

std::optional<IdempotencyCoreGateResult> append_recovery_outcome(
    AccountAuditLog& audit, const AccountAuditOpenGroup& group, AccountAuditOperation operation,
    AccountAuditMutationState mutation, const std::vector<AccountAuditStage>& completed_stages,
    const json& terminal, const AccountAuditCoordinator::Guard& guard,
    const IdempotencyRecoveryTimestamp& timestamp,
    const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks) {
    std::string error;
    auto outcome = make_account_audit_outcome(
        {{group.intent["invocation_id"].get<std::string>(), recovery_timestamp(timestamp)},
         group.intent["account"].get<std::string>(),
         operation,
         terminal["kind"] == "result",
         mutation,
         completed_stages,
         terminal},
        error);
    AccountAuditFailure failure;
    if (!outcome || !audit.append_outcome(*outcome, guard, failure)) {
        IdempotencyCoreGateResult result;
        result.status = IdempotencyCoreGateStatus::AuditIncomplete;
        result.audit_failure =
            failure.detail.empty()
                ? AccountAuditFailure{AccountAuditDurabilityReason::SchemaError, std::move(error)}
                : std::move(failure);
        return result;
    }
    notify_boundary(hooks, "recovery_outcome_synced");
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact durable recovery boundaries.
std::optional<IdempotencyCoreGateResult> recover_open_group(
    const AccountAuditOpenGroup& group, IdempotencyStore* store, AccountAuditLog& audit,
    AccountAuditRecoveryPermit& permit, std::vector<AccountAuditSpoolHold>& holds,
    const AccountAuditCoordinator::Guard& guard, const AccountAuditPinSource& pins,
    const IdempotencyRecoveryTimestamp& timestamp, const FileSpoolControl& spool_control,
    const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks,
    const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks) {
    std::string classify_error;
    auto plan = classify_account_audit_recovery(group, group.intent["account"].get<std::string>(),
                                                audit.path(), pins, classify_error);
    if (!plan) {
        IdempotencyCoreGateResult result;
        result.status = IdempotencyCoreGateStatus::AuditIncomplete;
        result.terminal =
            audit_incomplete_terminal(group.intent["account"].get<std::string>(), audit.path(),
                                      AccountAuditMutationState::None, group.completed_stages);
        result.audit_failure = {AccountAuditDurabilityReason::Contradiction,
                                std::move(classify_error)};
        return result;
    }
    const auto operation =
        parse_account_audit_operation(group.intent["command"].get_ref<const std::string&>());
    if (!operation) {
        IdempotencyCoreGateResult result;
        result.status = IdempotencyCoreGateStatus::AuditIncomplete;
        result.audit_failure = {AccountAuditDurabilityReason::Contradiction,
                                "open group lost its operation"};
        return result;
    }
    std::optional<SpoolFact> spool_fact;
    if (const auto spool = open_group_spool(group)) {
        spool_fact = SpoolFact{group.audit_generation,
                               group.intent["invocation_id"].get<std::string>(), *spool,
                               group.intent["idempotency_key_hash"].is_null()
                                   ? std::nullopt
                                   : std::optional<std::string>(
                                         group.intent["idempotency_key_hash"].get<std::string>()),
                               false};
    }
    std::optional<AccountAuditSpoolReleaseReceipt> cleanup_receipt;
    const auto cleanup_open_spool = [&]() -> std::optional<IdempotencyCoreGateResult> {
        if (!spool_fact) {
            return std::nullopt;
        }
        auto* hold = find_hold(holds, *spool_fact);
        if (hold == nullptr) {
            if (store != nullptr) {
                return relation_failure(*store, "open audit spool hold is missing");
            }
            IdempotencyCoreGateResult result;
            result.status = IdempotencyCoreGateStatus::AuditIncomplete;
            result.audit_failure = {AccountAuditDurabilityReason::Contradiction,
                                    "open audit spool hold is missing"};
            return result;
        }
        auto cleanup =
            cleanup_spool_file_with_hold(std::move(*hold), guard, spool_control, spool_hooks);
        if (auto* failure = std::get_if<FileSpoolError>(&cleanup)) {
            return spool_failure_result(std::move(*failure));
        }
        cleanup_receipt = std::get<AccountAuditSpoolReleaseReceipt>(std::move(cleanup));
        notify_boundary(hooks, "open_spool_cleanup_synced");
        return std::nullopt;
    };

    if (!plan->boundaries.empty() &&
        plan->boundaries.front() == AccountAuditRecoveryBoundary::DeleteSpoolAndSyncRoot) {
        if (auto failure = cleanup_open_spool()) {
            return failure;
        }
    }

    auto completed_stages = group.completed_stages;
    const std::uint32_t sequence =
        group.checkpoints.empty()
            ? 1
            : group.checkpoints.back()["checkpoint_sequence"].get<std::uint32_t>() + 1;
    if (std::ranges::find(plan->boundaries,
                          AccountAuditRecoveryBoundary::AppendMutationProofAndSync) !=
        plan->boundaries.end()) {
        if (sequence == 0) {
            IdempotencyCoreGateResult result;
            result.status = IdempotencyCoreGateStatus::AuditIncomplete;
            result.audit_failure = {AccountAuditDurabilityReason::SchemaError,
                                    "recovery checkpoint sequence overflowed"};
            return result;
        }
        if (auto failure = append_recovery_checkpoint(audit, group, *operation, sequence,
                                                      plan->terminal, guard, timestamp, hooks)) {
            return failure;
        }
        completed_stages.push_back(AccountAuditStage::MutationConfirmed);
    }
    if (std::ranges::find(plan->boundaries, AccountAuditRecoveryBoundary::AppendOutcomeAndSync) !=
        plan->boundaries.end()) {
        if (auto failure = append_recovery_outcome(audit, group, *operation, plan->mutation_state,
                                                   completed_stages, plan->terminal, guard,
                                                   timestamp, hooks)) {
            return failure;
        }
    }
    if (store != nullptr && !group.intent["idempotency_key_hash"].is_null() &&
        std::ranges::find(plan->boundaries, AccountAuditRecoveryBoundary::TransitionStoreAndSync) !=
            plan->boundaries.end()) {
        const auto key =
            parse_idempotency_key_hash(group.intent["idempotency_key_hash"].get<std::string>());
        if (!key) {
            return relation_failure(*store, "open recovery lost its key hash");
        }
        IdempotencyWriteResult transition;
        if (plan->mutation_state == AccountAuditMutationState::None) {
            transition =
                store->remove_owned(*key, group.intent["invocation_id"].get<std::string>(), guard);
        } else if (!plan->retain_store) {
            transition = store->complete(*key, group.intent["invocation_id"].get<std::string>(),
                                         plan->terminal, guard);
        } else {
            const auto inspection = store->inspect(guard);
            if (inspection.status != IdempotencyInspectionStatus::Clean) {
                return store_failure_result(inspection.failure);
            }
            transition = {IdempotencyWriteStatus::Unchanged, inspection.snapshot, {}};
        }
        if (transition.status == IdempotencyWriteStatus::Failed) {
            return store_failure_result(std::move(transition.failure));
        }
        notify_boundary(hooks, "open_store_transition_synced");
    }
    if (std::ranges::find(plan->boundaries,
                          AccountAuditRecoveryBoundary::CleanupSpoolAndSyncRoot) !=
        plan->boundaries.end()) {
        if (auto failure = cleanup_open_spool()) {
            return failure;
        }
        if (store != nullptr && spool_fact && spool_fact->key_hash) {
            const auto key = parse_idempotency_key_hash(*spool_fact->key_hash);
            if (!key) {
                return relation_failure(*store, "open cleanup lost its key hash");
            }
            const auto inspection = store->inspect(guard);
            if (inspection.status != IdempotencyInspectionStatus::Clean) {
                return store_failure_result(inspection.failure);
            }
            const auto* entry = find_entry(inspection.snapshot, *spool_fact->key_hash);
            if (entry != nullptr && entry->invocation_id == spool_fact->invocation_id &&
                entry->state == IdempotencyEntryState::Completed && entry->spool) {
                auto cleared = store->clear_spool(*key, spool_fact->invocation_id, guard);
                if (cleared.status == IdempotencyWriteStatus::Failed) {
                    return store_failure_result(std::move(cleared.failure));
                }
                notify_boundary(hooks, "open_store_spool_cleared");
            }
        }
    }
    if (cleanup_receipt) {
        AccountAuditFailure release_failure;
        if (!permit.release_spool_hold(std::move(*cleanup_receipt), guard, release_failure)) {
            IdempotencyCoreGateResult result;
            result.status = IdempotencyCoreGateStatus::AuditUnavailable;
            result.audit_failure = std::move(release_failure);
            return result;
        }
        notify_boundary(hooks, "open_spool_hold_released");
    }
    if (!plan->continue_current_request) {
        IdempotencyCoreGateResult result;
        result.status = IdempotencyCoreGateStatus::AuditIncomplete;
        result.terminal = plan->terminal;
        return result;
    }
    return std::nullopt;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact nine-step core gate order.
IdempotencyCoreGateResult IdempotencyFoundation::run_core_gate(
    const AccountAuditCoordinator::Guard& guard, std::uint64_t sampled_now,
    const IdempotencyRecoveryTimestamp& timestamp, const FileSpoolControl& spool_control,
    const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks,
    const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks) {
    const auto effective_spool_control = guard.constrain_file_spool_control(spool_control);
    auto inventory_result =
        enumerate_spool(state_directory_, expected_uid_, effective_spool_control, spool_hooks);
    if (auto* failure = std::get_if<FileSpoolError>(&inventory_result)) {
        return spool_failure_result(std::move(*failure));
    }
    auto inventory = std::move(std::get<SpoolInventory>(inventory_result));
    if (inventory.contradiction) {
        return spool_contradiction(account_, audit_.path());
    }
    auto inspection = store_.inspect(guard);
    if (inspection.status != IdempotencyInspectionStatus::Clean) {
        return store_failure_result(std::move(inspection.failure));
    }
    CompletedRelationState relations;
    relations.desired = inspection.snapshot;
    relations.sampled_now = sampled_now;
    relations.sampled_now_valid = sampled_now <= kIdempotencyMaximumUnixSeconds;
    relations.account = account_;
    for (const auto& entry : inspection.snapshot.entries) {
        relations.pin_keys.emplace(PinIdentity{entry.audit_generation, entry.invocation_id,
                                               entry.request_fingerprint.value(), entry.operation},
                                   entry.key_hash.value());
    }
    AccountAuditRecoveryPermit permit;
    const auto pin_source = AccountAuditPinSource{IdempotencyStore::pins(inspection.snapshot)};
    auto audit_inspection = audit_.prepare_recovery(
        pin_source, guard, permit, [&](const AccountAuditCompletedGroupView& view) {
            consume_completed_view(view, relations);
        });
    if (audit_inspection.status != AccountAuditInspectionStatus::Clean &&
        audit_inspection.status != AccountAuditInspectionStatus::Open) {
        return audit_failure_result(audit_inspection);
    }
    if (!relations.contradiction.empty()) {
        return relation_failure(store_, std::move(relations.contradiction));
    }
    if (audit_inspection.oldest_open &&
        !reconcile_open_relation(*audit_inspection.oldest_open, relations)) {
        return relation_failure(store_, std::move(relations.contradiction));
    }
    if (auto failure = validate_spool_relation(
            inventory, relations,
            audit_inspection.oldest_open ? &*audit_inspection.oldest_open : nullptr, account_,
            audit_.path())) {
        return std::move(*failure);
    }
    auto temp_cleanup = store_.cleanup_stale_temp(guard);
    if (temp_cleanup.status == IdempotencyWriteStatus::Failed) {
        return store_failure_result(std::move(temp_cleanup.failure));
    }
    notify_boundary(hooks, "stale_temp_reconciled");
    auto repaired =
        store_.apply_reconciled_snapshot(inspection.snapshot, std::move(relations.desired), guard);
    if (repaired.status == IdempotencyWriteStatus::Failed) {
        return store_failure_result(std::move(repaired.failure));
    }
    notify_boundary(hooks, "completed_store_reconciled");
    auto holds = permit.issue_spool_holds();
    if (audit_inspection.oldest_open) {
        const auto current = store_.inspect(guard);
        if (current.status != IdempotencyInspectionStatus::Clean) {
            return store_failure_result(current.failure);
        }
        const auto current_pins = AccountAuditPinSource{IdempotencyStore::pins(current.snapshot)};
        if (auto failure = recover_open_group(*audit_inspection.oldest_open, &store_, audit_,
                                              permit, holds, guard, current_pins, timestamp,
                                              effective_spool_control, spool_hooks, hooks)) {
            return std::move(*failure);
        }
    }
    for (const auto& fact : relations.spools) {
        if (!fact.retain) {
            if (auto failure = cleanup_fact(fact, holds, permit, &store_, guard,
                                            effective_spool_control, spool_hooks, hooks)) {
                return std::move(*failure);
            }
        }
    }
    if (!relations.sampled_now_valid) {
        return store_failure_result({AccountAuditDurabilityReason::SchemaError,
                                     account_,
                                     store_.path(),
                                     "wall-clock sample is unrepresentable",
                                     {}});
    }
    auto swept = store_.sweep_expired(sampled_now, guard);
    if (swept.status == IdempotencyWriteStatus::Failed) {
        return store_failure_result(std::move(swept.failure));
    }
    notify_boundary(hooks, "expiry_store_swept");
    for (const auto& removed : swept.removed) {
        if (!removed.spool) {
            continue;
        }
        const auto fact = std::ranges::find_if(relations.spools, [&](const SpoolFact& candidate) {
            return candidate.invocation_id == removed.invocation_id &&
                   candidate.audit_generation == removed.audit_generation &&
                   candidate.spool == *removed.spool;
        });
        if (fact == relations.spools.end()) {
            return relation_failure(store_, "expired spool lost its audit-derived hold");
        }
        if (auto failure = cleanup_fact(*fact, holds, permit, &store_, guard,
                                        effective_spool_control, spool_hooks, hooks)) {
            return std::move(*failure);
        }
    }
    auto final_inspection = store_.inspect(guard);
    if (final_inspection.status != IdempotencyInspectionStatus::Clean) {
        return store_failure_result(std::move(final_inspection.failure));
    }
    return {IdempotencyCoreGateStatus::Clean, std::move(final_inspection.snapshot), {}, {}, {}, {}};
}

IdempotencyCoreGateResult IdempotencyFoundation::run_absent_by_policy_gate(
    const AccountAuditCoordinator::Guard& guard, std::uint64_t sampled_now,
    const IdempotencyRecoveryTimestamp& timestamp, const FileSpoolControl& spool_control,
    const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks,
    const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks) {
    const auto effective_spool_control = guard.constrain_file_spool_control(spool_control);
    auto inventory_result =
        enumerate_spool(state_directory_, expected_uid_, effective_spool_control, spool_hooks);
    if (auto* failure = std::get_if<FileSpoolError>(&inventory_result)) {
        return spool_failure_result(std::move(*failure));
    }
    auto inventory = std::move(std::get<SpoolInventory>(inventory_result));
    if (inventory.contradiction) {
        return spool_contradiction(account_, audit_.path());
    }
    CompletedRelationState relations;
    relations.sampled_now = sampled_now;
    relations.sampled_now_valid = sampled_now <= kIdempotencyMaximumUnixSeconds;
    relations.account = account_;
    AccountAuditRecoveryPermit permit;
    const auto absent = AccountAuditPinSource{AbsentAccountAuditPinsByPolicy{}};
    auto audit_inspection = audit_.prepare_recovery(
        absent, guard, permit, [&](const AccountAuditCompletedGroupView& view) {
            if (!view.idempotency_key_hash) {
                consume_completed_view(view, relations);
            }
        });
    if (audit_inspection.status != AccountAuditInspectionStatus::Clean &&
        audit_inspection.status != AccountAuditInspectionStatus::Open) {
        return audit_failure_result(audit_inspection);
    }
    if (auto failure = validate_spool_relation(
            inventory, relations,
            audit_inspection.oldest_open ? &*audit_inspection.oldest_open : nullptr, account_,
            audit_.path())) {
        return std::move(*failure);
    }
    auto holds = permit.issue_spool_holds();
    if (audit_inspection.oldest_open) {
        if (auto failure = recover_open_group(*audit_inspection.oldest_open, nullptr, audit_,
                                              permit, holds, guard, absent, timestamp,
                                              effective_spool_control, spool_hooks, hooks)) {
            return std::move(*failure);
        }
    }
    for (const auto& fact : relations.spools) {
        if (!fact.retain) {
            auto* hold = find_hold(holds, fact);
            if (hold == nullptr) {
                IdempotencyCoreGateResult result;
                result.status = IdempotencyCoreGateStatus::AuditIncomplete;
                result.audit_failure = {AccountAuditDurabilityReason::Contradiction,
                                        "audit-only spool hold is missing"};
                return result;
            }
            auto cleanup = cleanup_spool_file_with_hold(std::move(*hold), guard,
                                                        effective_spool_control, spool_hooks);
            if (auto* failure = std::get_if<FileSpoolError>(&cleanup)) {
                return spool_failure_result(std::move(*failure));
            }
            AccountAuditFailure release_failure;
            if (!permit.release_spool_hold(
                    std::get<AccountAuditSpoolReleaseReceipt>(std::move(cleanup)), guard,
                    release_failure)) {
                IdempotencyCoreGateResult result;
                result.status = IdempotencyCoreGateStatus::AuditUnavailable;
                result.audit_failure = std::move(release_failure);
                return result;
            }
        }
    }
    if (!relations.sampled_now_valid) {
        IdempotencyCoreGateResult result;
        result.status = IdempotencyCoreGateStatus::AuditUnavailable;
        result.audit_failure = {AccountAuditDurabilityReason::SchemaError,
                                "wall-clock sample is unrepresentable"};
        return result;
    }
    return {IdempotencyCoreGateStatus::Clean, {}, {}, {}, {}, {}};
}

} // namespace tgcli::daemon
