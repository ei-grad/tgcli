#pragma once

#include "common/config.hpp"
#include "common/paths.hpp"
#include "daemon/removal_filesystem.hpp"
#include "daemon/removal_journal.hpp"

#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

struct RemovalPlanningError {
    std::string code;
    std::string message;
    nlohmann::json details;
    int exit_code = 1;
};

struct PlannedAccountRemoval {
    proto::AccountRemovePlan plan;
    CapturedRemovalRoot data_root;
    CapturedRemovalRoot state_root;
    std::shared_ptr<const config::ConfigSnapshot> config;
    std::optional<config::AccountConfig> account_config;
    std::optional<RemovalTombstone> recovery;
};

struct RemovalPlanningResult {
    std::optional<PlannedAccountRemoval> planned;
    std::optional<RemovalPlanningError> error;

    explicit operator bool() const {
        return planned.has_value() && !error.has_value();
    }
};

[[nodiscard]] RemovalPlanningResult plan_account_removal(
    const config::Store& store, const RemovalJournal& journal,
    const paths::Environment& environment, std::string account, bool keep_session,
    const std::optional<std::string>& requested_reassignment,
    const config::MutationControl& control = {},
    const std::shared_ptr<const testing::RemovalFilesystemHooks>& filesystem_hooks = {});

} // namespace tgcli::daemon
