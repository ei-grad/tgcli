#include "cli/routing.hpp"

#include "common/config.hpp"
#include "common/exit_codes.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace tgcli::cli {

namespace {

std::string command_key(const std::vector<std::string>& command) {
    std::string key;
    for (const auto& part : command) {
        if (!key.empty()) {
            key += ' ';
        }
        key += part;
    }
    return key;
}

bool accepts_implicit_main(const std::vector<std::string>& command) {
    const auto key = command_key(command);
    constexpr std::array<std::string_view, 6> allowed{
        "login", "doctor", "daemon status", "daemon stop", "daemon restart", "daemon run"};
    return std::ranges::find(allowed, key) != allowed.end();
}

RoutingResult usage(std::string argument, std::string message, std::string reason) {
    return {{},
            RoutingError{"USAGE",
                         std::move(message),
                         {{"argument", std::move(argument)}, {"reason", std::move(reason)}},
                         kUsage},
            true};
}

bool remains_available_without_config(const std::vector<std::string>& command) {
    const auto key = command_key(command);
    return key == "doctor" || key == "daemon status";
}

std::optional<RoutingResult>
route_explicit_account_without_config(const std::optional<std::string>& explicit_account,
                                      const std::vector<std::string>& command,
                                      const RoutingError& config_error) {
    if (!explicit_account || command != std::vector<std::string>{"logout"}) {
        return std::nullopt;
    }
    std::string selection_error;
    const auto selection =
        paths::select_account({explicit_account, std::nullopt, std::nullopt}, selection_error);
    if (!selection) {
        return usage("--account", std::move(selection_error), "invalid_argument");
    }
    return RoutingResult{selection, config_error, false};
}

RoutingResult route_without_snapshot(const std::vector<std::string>& command,
                                     const std::optional<std::string>& explicit_account,
                                     const std::optional<std::string>& environment_account,
                                     const RoutingError& route_error) {
    if (remains_available_without_config(command)) {
        std::string selection_error;
        const auto selection = paths::select_account(
            {explicit_account, environment_account, std::nullopt}, selection_error);
        if (!selection) {
            if (explicit_account) {
                return usage("--account", std::move(selection_error), "invalid_argument");
            }
            return usage("TGCLI_ACCOUNT", std::move(selection_error), "invalid_environment");
        }
        return {selection, {}, false};
    }
    if (auto routed =
            route_explicit_account_without_config(explicit_account, command, route_error)) {
        return std::move(*routed);
    }
    return {{}, route_error, true};
}

} // namespace

bool is_config_global_command(const std::vector<std::string>& command) {
    const auto key = command_key(command);
    constexpr std::array<std::string_view, 4> commands{"account add", "account list",
                                                       "account show", "account use"};
    return std::ranges::find(commands, key) != commands.end();
}

RoutingResult resolve_account_route(const std::vector<std::string>& command,
                                    const paths::Environment& environment,
                                    const std::optional<std::string>& explicit_account,
                                    const std::optional<std::string>& environment_account) {
    const config::Store store(paths::config_file(environment), environment.uid);
    config::SnapshotManager snapshots(store);
    const bool initialized = snapshots.initialize();
    const auto published = snapshots.current();
    if (!initialized || !published || !published->snapshot) {
        const auto fallback =
            config::ConfigError{config::ConfigReason::IoError, "config load failed"};
        const auto error = published ? published->error.value_or(fallback) : fallback;
        const RoutingError route_error{
            "CONFIG_INVALID",
            "cannot use current config.toml",
            {{"path", store.path()}, {"reason", config::reason_name(error.reason)}},
            kGeneric};
        return route_without_snapshot(command, explicit_account, environment_account, route_error);
    }
    const auto& snapshot = published->snapshot;
    if (published->error) {
        const auto error = published->error.value_or(
            config::ConfigError{config::ConfigReason::IoError, "config load failed"});
        const RoutingError route_error{
            "CONFIG_INVALID",
            "cannot use current config.toml",
            {{"path", store.path()}, {"reason", config::reason_name(error.reason)}},
            kGeneric};
        if (auto routed =
                route_explicit_account_without_config(explicit_account, command, route_error)) {
            return std::move(*routed);
        }
        return {{}, route_error, true};
    }

    std::string selection_error;
    const auto selection = paths::select_account(
        {explicit_account, environment_account, snapshot->default_account}, selection_error);
    if (!selection) {
        if (explicit_account) {
            return usage("--account", std::move(selection_error), "invalid_argument");
        }
        if (environment_account) {
            return usage("TGCLI_ACCOUNT", std::move(selection_error), "invalid_environment");
        }
        return {{},
                RoutingError{"CONFIG_INVALID",
                             "default account is invalid",
                             {{"path", store.path()}, {"reason", "invalid_default"}},
                             kGeneric},
                true};
    }

    if (snapshot->accounts.contains(selection->name)) {
        return {selection, {}, true};
    }
    if (selection->source == paths::AccountSelectionSource::ImplicitMain &&
        accepts_implicit_main(command)) {
        return {selection, {}, true};
    }
    // An auto-spawned first-run daemon is re-exec'd with an explicit account
    // argument after the parent selected implicit main.
    if (command == std::vector<std::string>{"daemon", "run"} && selection->name == "main" &&
        snapshot->accounts.empty()) {
        return {selection, {}, true};
    }
    return {{},
            RoutingError{"ACCOUNT_NOT_FOUND",
                         "account is not configured",
                         {{"account", selection->name}},
                         kNotFound},
            true};
}

} // namespace tgcli::cli
