#pragma once

#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon::raw::audit_v3 {

enum class RecoveryAction {
    NoMutation,
    EmitUnconfirmed,
    RepairConfirmedResult,
    RepairPossibleError,
    Complete,
    FailClosed,
};

struct RecoveryDecision {
    std::string invocation_id;
    RecoveryAction action = RecoveryAction::FailClosed;
};

struct ScanResult {
    bool valid = false;
    std::vector<RecoveryDecision> decisions;
};

[[nodiscard]] bool valid_intent(const nlohmann::json& value);
[[nodiscard]] bool valid_checkpoint(const nlohmann::json& value);
[[nodiscard]] bool valid_outcome(const nlohmann::json& value);
[[nodiscard]] ScanResult scan(std::span<const nlohmann::json> records);

} // namespace tgcli::daemon::raw::audit_v3
