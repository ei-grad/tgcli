#pragma once

#include "common/config.hpp"
#include "daemon/removal_filesystem.hpp"
#include "daemon/removal_journal.hpp"

#include <functional>
#include <optional>
#include <string>

namespace tgcli::daemon {

enum class RemovalRecoveryAction {
    EnsureIntent,
    ReevaluateRemote,
    QuiesceClient,
    BeginConfigRemoval,
    RetryConfigRemoval,
    RecordConfigRemoved,
    FinishDataRemoval,
    FinishStateRemoval,
    EnsureOutcome,
    RecordOutcomeSynced,
    Complete,
};

struct RemovalRecoveryFacts {
    RemovalRecoveryFacts(const config::ConfigSnapshot& config_snapshot,
                         RemovalRootObservation data_observation,
                         RemovalRootObservation state_observation,
                         RemovalAuditPresence audit_presence)
        : config(config_snapshot), data(data_observation), state(state_observation),
          audit(audit_presence) {}

    std::reference_wrapper<const config::ConfigSnapshot> config;
    RemovalRootObservation data;
    RemovalRootObservation state;
    RemovalAuditPresence audit;
};

struct RemovalRecoveryDecision {
    std::optional<RemovalRecoveryAction> action;
    std::string reason;

    explicit operator bool() const {
        return action.has_value();
    }
};

[[nodiscard]] RemovalRecoveryDecision decide_removal_recovery(const RemovalTombstone& tombstone,
                                                              const RemovalRecoveryFacts& facts);

[[nodiscard]] bool can_resume_removal_without_tdlib(const RemovalTombstone& tombstone);

} // namespace tgcli::daemon
