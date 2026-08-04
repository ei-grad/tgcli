#include "daemon/removal_recovery.hpp"

namespace tgcli::daemon {

namespace {

bool config_is_captured(const RemovalTombstone& tombstone, const config::ConfigSnapshot& snapshot) {
    return snapshot.identity == tombstone.plan.config_snapshot() &&
           snapshot.accounts.contains(tombstone.account);
}

bool config_is_removed(const RemovalTombstone& tombstone, const config::ConfigSnapshot& snapshot) {
    return !snapshot.accounts.contains(tombstone.account) &&
           snapshot.default_account == tombstone.plan.reassign_default();
}

bool root_is_pre_mutation(RemovalRootObservation observation, bool planned_present) {
    return planned_present ? observation == RemovalRootObservation::Captured
                           : observation == RemovalRootObservation::PlannedAbsent;
}

bool root_is_post_mutation(RemovalRootObservation observation, bool planned_present) {
    return planned_present ? observation == RemovalRootObservation::Absent
                           : observation == RemovalRootObservation::PlannedAbsent;
}

bool root_is_started(RemovalRootObservation observation, bool planned_present) {
    return planned_present ? observation == RemovalRootObservation::Captured ||
                                 observation == RemovalRootObservation::Staged ||
                                 observation == RemovalRootObservation::Absent
                           : observation == RemovalRootObservation::PlannedAbsent;
}

RemovalRecoveryDecision invalid() {
    return {std::nullopt, "identity_ambiguous"};
}

RemovalRecoveryDecision action(RemovalRecoveryAction value) {
    return {value, {}};
}

} // namespace

// This closed switch is the recovery truth table. Keeping every durable stage beside all four
// identity predicates makes illegal cross-stage combinations explicit and fail-closed.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
RemovalRecoveryDecision decide_removal_recovery(const RemovalTombstone& tombstone,
                                                const RemovalRecoveryFacts& facts) {
    if (tombstone.stage == AuditStage::OutcomeSynced) {
        return action(RemovalRecoveryAction::Complete);
    }
    if (facts.audit.outcome && !facts.audit.intent) {
        return invalid();
    }
    const bool data_present = tombstone.plan.data_root().has_value();
    const bool state_present = tombstone.plan.state_root().has_value();
    const bool captured_config = config_is_captured(tombstone, facts.config.get());
    const bool removed_config = config_is_removed(tombstone, facts.config.get());
    const bool data_before = root_is_pre_mutation(facts.data, data_present);
    const bool state_before = root_is_pre_mutation(facts.state, state_present);
    const bool data_after = root_is_post_mutation(facts.data, data_present);
    const bool state_after = root_is_post_mutation(facts.state, state_present);
    const auto continue_or_finish = [&](RemovalRecoveryAction next) {
        return action(facts.audit.outcome ? RemovalRecoveryAction::RecordOutcomeSynced : next);
    };

    switch (tombstone.stage) {
    case AuditStage::Planned:
    case AuditStage::IntentSynced:
        if (!captured_config || !data_before || !state_before) {
            return invalid();
        }
        if (facts.audit.outcome && facts.audit.intent) {
            return action(RemovalRecoveryAction::RecordOutcomeSynced);
        }
        if (!facts.audit.intent || tombstone.stage == AuditStage::Planned) {
            return action(RemovalRecoveryAction::EnsureIntent);
        }
        return continue_or_finish(RemovalRecoveryAction::ReevaluateRemote);
    case AuditStage::RemoteLogoutSendStarted:
        if (!captured_config || !data_before || !state_before || !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(RemovalRecoveryAction::ReevaluateRemote);
    case AuditStage::RemoteConfirmed:
    case AuditStage::RemoteNotPresent:
    case AuditStage::RemoteKept:
    case AuditStage::ClientCloseStarted:
        if (!captured_config || !data_before || !state_before || !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(RemovalRecoveryAction::QuiesceClient);
    case AuditStage::ClientClosed:
        if (!captured_config || !data_before || !state_before || !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(RemovalRecoveryAction::BeginConfigRemoval);
    case AuditStage::ConfigRemoveStarted:
        if ((!captured_config && !removed_config) || !data_before || !state_before ||
            !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(captured_config ? RemovalRecoveryAction::RetryConfigRemoval
                                                  : RemovalRecoveryAction::RecordConfigRemoved);
    case AuditStage::ConfigRemoved:
        if (!removed_config || !data_before || !state_before || !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(RemovalRecoveryAction::FinishDataRemoval);
    case AuditStage::DataRemoveStarted:
        if (!removed_config || !root_is_started(facts.data, data_present) || !state_before ||
            !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(RemovalRecoveryAction::FinishDataRemoval);
    case AuditStage::DataRemoved:
        if (!removed_config || !data_after || !state_before || !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(RemovalRecoveryAction::FinishStateRemoval);
    case AuditStage::StateRemoveStarted:
        if (!removed_config || !data_after || !root_is_started(facts.state, state_present) ||
            !facts.audit.intent) {
            return invalid();
        }
        return continue_or_finish(RemovalRecoveryAction::FinishStateRemoval);
    case AuditStage::StateRemoved:
        if (!removed_config || !data_after || !state_after || !facts.audit.intent) {
            return invalid();
        }
        return action(facts.audit.outcome ? RemovalRecoveryAction::RecordOutcomeSynced
                                          : RemovalRecoveryAction::EnsureOutcome);
    case AuditStage::OutcomeSynced:
        break;
    case AuditStage::LogoutSendStarted:
    case AuditStage::LogoutClosedConfirmed:
        return invalid();
    }
    return invalid();
}

bool can_resume_removal_without_tdlib(const RemovalTombstone& tombstone) {
    switch (tombstone.stage) {
    case AuditStage::RemoteConfirmed:
    case AuditStage::RemoteNotPresent:
    case AuditStage::RemoteKept:
    case AuditStage::ClientCloseStarted:
    case AuditStage::ClientClosed:
    case AuditStage::ConfigRemoveStarted:
    case AuditStage::ConfigRemoved:
    case AuditStage::DataRemoveStarted:
    case AuditStage::DataRemoved:
    case AuditStage::StateRemoveStarted:
    case AuditStage::StateRemoved:
        return true;
    case AuditStage::Planned:
    case AuditStage::IntentSynced:
    case AuditStage::RemoteLogoutSendStarted:
    case AuditStage::OutcomeSynced:
    case AuditStage::LogoutSendStarted:
    case AuditStage::LogoutClosedConfirmed:
        return false;
    }
    return false;
}

} // namespace tgcli::daemon
