#pragma once

#include "common/paths.hpp"

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::cli {

struct RoutingError {
    std::string code;
    std::string message;
    nlohmann::json details = nlohmann::json::object();
    int exit_code = 1;
};

struct RoutingResult {
    std::optional<paths::AccountSelection> selection;
    std::optional<RoutingError> error;
    bool current_config_valid = true;
};

bool is_config_global_command(const std::vector<std::string>& command);

// Resolves production account routing from the current file without creating
// config or account roots. The caller bypasses this for config-global commands.
RoutingResult resolve_account_route(const std::vector<std::string>& command,
                                    const paths::Environment& environment,
                                    const std::optional<std::string>& explicit_account,
                                    const std::optional<std::string>& environment_account);

} // namespace tgcli::cli
