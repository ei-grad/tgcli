#include "daemon/removal_planner.hpp"

#include "common/exit_codes.hpp"

#include <algorithm>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

RemovalPlanningResult failure(std::string code, std::string message, json details, int exit_code) {
    return {
        {},
        RemovalPlanningError{std::move(code), std::move(message), std::move(details), exit_code}};
}

RemovalPlanningResult config_failure(const config::Store& store, const config::LoadResult& loaded) {
    if (loaded.timed_out) {
        return failure("TIMEOUT", "account removal planning timed out",
                       {{"operation", "account_remove"}, {"state", nullptr}}, kTimeout);
    }
    if (loaded.cancelled) {
        return failure("INTERNAL", "account removal planning was cancelled",
                       {{"operation", "account_remove"}, {"reason", "internal_error"}}, kGeneric);
    }
    const auto reason = loaded.error ? config::reason_name(loaded.error->reason) : "io_error";
    return failure("CONFIG_INVALID", "cannot use current config.toml",
                   {{"path", store.path()}, {"reason", reason}}, kGeneric);
}

json sorted_paths(std::array<std::string, 2> paths) {
    std::ranges::sort(paths);
    return json::array({paths[0], paths[1]});
}

RemovalPlanningResult root_failure(std::string_view account,
                                   const std::array<std::string, 2>& paths,
                                   const RemovalFilesystemFailure& root_error) {
    return failure("LOCAL_CLEANUP_FAILED", "account roots cannot be safely removed",
                   {{"account", account},
                    {"reason", root_error.reason.empty() ? "path_invalid" : root_error.reason},
                    {"removed", json::array()},
                    {"retained", sorted_paths(paths)}},
                   kGeneric);
}

std::optional<std::string> resulting_default(const config::ConfigSnapshot& snapshot,
                                             const std::string& account,
                                             const std::optional<std::string>& requested,
                                             RemovalPlanningResult& error) {
    const bool is_default = snapshot.default_account == std::optional{account};
    if (!is_default) {
        if (requested) {
            error = failure("USAGE", "default reassignment is only valid for the current default",
                            {{"argument", "--reassign-default"}, {"reason", "invalid_argument"}},
                            kUsage);
            return std::nullopt;
        }
        return snapshot.default_account;
    }
    if (snapshot.accounts.size() == 1) {
        if (requested) {
            error = failure("USAGE", "the sole account cannot reassign the default",
                            {{"argument", "--reassign-default"}, {"reason", "invalid_argument"}},
                            kUsage);
        }
        return std::nullopt;
    }
    if (!requested) {
        json candidates = json::array();
        for (const auto& [name, ignored] : snapshot.accounts) {
            static_cast<void>(ignored);
            if (name != account) {
                candidates.push_back(name);
            }
        }
        error = failure("DEFAULT_REASSIGNMENT_REQUIRED",
                        "removing the default account requires an explicit replacement",
                        {{"account", account}, {"candidates", std::move(candidates)}}, kUsage);
        return std::nullopt;
    }
    if (*requested == account) {
        error =
            failure("USAGE", "the replacement default must differ from the removed account",
                    {{"argument", "--reassign-default"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    if (!snapshot.accounts.contains(*requested)) {
        error = failure("ACCOUNT_NOT_FOUND", "replacement account is not configured",
                        {{"account", *requested}}, kNotFound);
        return std::nullopt;
    }
    return requested;
}

} // namespace

RemovalPlanningResult plan_account_removal(
    const config::Store& store, const RemovalJournal& journal,
    const paths::Environment& environment, std::string account, bool keep_session,
    const std::optional<std::string>& requested_reassignment,
    const config::MutationControl& control,
    const std::shared_ptr<const testing::RemovalFilesystemHooks>& filesystem_hooks) {
    if (!paths::valid_account_name(account)) {
        return failure("USAGE", "invalid account name",
                       {{"argument", "name"}, {"reason", "invalid_argument"}}, kUsage);
    }
    const auto inspection = journal.inspect_account(account);
    if (inspection.status == RemovalInspectionStatus::Invalid) {
        return failure("AUDIT_UNAVAILABLE", "removal journal cannot be inspected",
                       {{"account", account},
                        {"path", inspection.path.empty() ? journal.directory() : inspection.path},
                        {"reason", inspection.failure.reason.empty() ? "path_invalid"
                                                                     : inspection.failure.reason}},
                       kDenied);
    }

    const auto loaded = store.load(control);
    if (!loaded) {
        return config_failure(store, loaded);
    }
    if (inspection.status == RemovalInspectionStatus::Incomplete && inspection.tombstone) {
        const auto& tombstone = *inspection.tombstone;
        if (tombstone.plan.keep_session() != keep_session ||
            (requested_reassignment &&
             tombstone.plan.reassign_default() != requested_reassignment)) {
            return failure("REMOVAL_INCOMPLETE", "removal retry does not match the durable plan",
                           {{"account", account},
                            {"path", inspection.path},
                            {"invocation_id", tombstone.invocation_id},
                            {"stage", audit_stage_name(tombstone.stage)},
                            {"completed_stages", serialize(tombstone)["completed_stages"]},
                            {"reason", "identity_ambiguous"}},
                           kGeneric);
        }
        const auto account_config = loaded.snapshot->accounts.find(account);
        return {
            PlannedAccountRemoval{tombstone.plan,
                                  {tombstone.plan.delete_paths()[0], tombstone.plan.data_root()},
                                  {tombstone.plan.delete_paths()[1], tombstone.plan.state_root()},
                                  loaded.snapshot,
                                  account_config == loaded.snapshot->accounts.end()
                                      ? std::nullopt
                                      : std::optional{account_config->second},
                                  tombstone},
            {}};
    }

    const auto found = loaded.snapshot->accounts.find(account);
    if (found == loaded.snapshot->accounts.end()) {
        return failure("ACCOUNT_NOT_FOUND", "account is not configured", {{"account", account}},
                       kNotFound);
    }
    RemovalPlanningResult default_error;
    const auto default_account =
        resulting_default(*loaded.snapshot, account, requested_reassignment, default_error);
    if (default_error.error) {
        return default_error;
    }

    const std::array delete_paths{paths::account_data_dir(account, environment),
                                  paths::account_state_dir(account, environment)};
    RemovalFilesystemFailure root_error;
    auto data =
        capture_removal_root(delete_paths[0], environment.uid, root_error, filesystem_hooks);
    if (!data) {
        return root_failure(account, delete_paths, root_error);
    }
    auto state =
        capture_removal_root(delete_paths[1], environment.uid, root_error, filesystem_hooks);
    if (!state) {
        return root_failure(account, delete_paths, root_error);
    }

    std::string plan_error;
    auto plan = proto::make_account_remove_plan({.account = account,
                                                 .keep_session = keep_session,
                                                 .delete_paths = delete_paths,
                                                 .config_path = store.path(),
                                                 .config_snapshot = loaded.snapshot->identity,
                                                 .data_root = data->identity,
                                                 .state_root = state->identity,
                                                 .reassign_default = default_account},
                                                plan_error);
    if (!plan) {
        return failure("CONFIG_INVALID", "account removal paths are invalid",
                       {{"path", store.path()}, {"reason", "path_invalid"}}, kGeneric);
    }
    return {PlannedAccountRemoval{std::move(*plan), std::move(*data), std::move(*state),
                                  loaded.snapshot, found->second, std::nullopt},
            {}};
}

} // namespace tgcli::daemon
