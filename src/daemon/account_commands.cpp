#include "daemon/account_commands.hpp"

#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "daemon/account_removal.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/request_session.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

void usage(RequestSession& session, std::optional<std::string> argument, std::string_view message,
           std::string_view reason = "invalid_argument") {
    session.error("USAGE", std::string(message),
                  {{"argument", argument ? json(*argument) : json(nullptr)}, {"reason", reason}},
                  kUsage);
}

void config_invalid(RequestSession& session, const ConfigGlobalContext& context,
                    const config::ConfigError& error) {
    session.error(
        "CONFIG_INVALID", "cannot use current config.toml",
        {{"path", context.store.get().path()}, {"reason", config::reason_name(error.reason)}},
        kGeneric);
}

void timeout(RequestSession& session, std::string_view operation) {
    session.error("TIMEOUT", "config operation timed out",
                  {{"operation", operation}, {"state", nullptr}}, kTimeout);
}

std::shared_ptr<const config::ConfigSnapshot> load_current(RequestSession& session,
                                                           const ConfigGlobalContext& context,
                                                           std::string_view operation) {
    if (session.cancellation_requested()) {
        return {};
    }
    if (RequestSession::Clock::now() >= session.deadline()) {
        timeout(session, operation);
        return {};
    }
    auto loaded = context.store.get().load({session.deadline(), session.cancellation_token()});
    if (loaded.cancelled || session.cancellation_requested()) {
        return {};
    }
    if (loaded.timed_out) {
        timeout(session, operation);
        return {};
    }
    if (RequestSession::Clock::now() >= session.deadline()) {
        timeout(session, operation);
        return {};
    }
    if (!loaded) {
        config_invalid(session, context,
                       loaded.error.value_or(config::ConfigError{config::ConfigReason::IoError,
                                                                 "config load failed"}));
        return {};
    }
    return std::move(loaded.snapshot);
}

bool validate_internal_args(const proto::Request& request, RequestSession& session,
                            bool expects_account, std::string& account) {
    const auto expected_size = expects_account ? std::size_t{2} : std::size_t{1};
    if (!request.args.is_object() || request.args.size() != expected_size ||
        !request.args.contains("global_account_supplied") ||
        !request.args["global_account_supplied"].is_boolean() ||
        (expects_account &&
         (!request.args.contains("account") || !request.args["account"].is_string()))) {
        usage(session, std::nullopt, "invalid account command arguments");
        return false;
    }
    if (request.args["global_account_supplied"].get<bool>()) {
        usage(session, "--account", "--account cannot target an account subcommand",
              "mutually_exclusive");
        return false;
    }
    if (!expects_account) {
        return true;
    }
    account = request.args["account"].get<std::string>();
    if (!paths::valid_account_name(account)) {
        usage(session, "name", "invalid account name");
        return false;
    }
    return true;
}

std::string credential_source(bool has_value, bool has_command) {
    if (has_value) {
        return "value";
    }
    return has_command ? "command" : "missing";
}

std::string audit_timestamp() {
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

json completed_stages_json(const std::vector<AuditStage>& completed) {
    json result = json::array();
    for (const auto stage : completed) {
        result.push_back(audit_stage_name(stage));
    }
    return result;
}

void audit_incomplete(RequestSession& session, std::string_view account,
                      const LogoutAuditLog& audit, const std::vector<AuditStage>& completed = {}) {
    std::string error;
    const auto mutation = derive_mutation_state(DestructiveCommand::Logout, completed, error);
    session.error("AUDIT_INCOMPLETE", "logout audit reconciliation is incomplete",
                  {{"account", account},
                   {"path", audit.path()},
                   {"mutation_state", mutation ? mutation_state_name(*mutation) : "none"},
                   {"completed_stages", completed_stages_json(completed)}},
                  kGeneric);
}

void audit_unavailable(RequestSession& session, std::string_view account,
                       const LogoutAuditLog& audit, std::string reason = "path_invalid") {
    session.error("AUDIT_UNAVAILABLE", "logout audit cannot be inspected",
                  {{"account", account}, {"path", audit.path()}, {"reason", std::move(reason)}},
                  kDenied);
}

bool account_show_audit_preflight(RequestSession& session, const ConfigGlobalContext& context,
                                  const std::string& account) {
    const LogoutAuditLog audit(paths::account_state_dir(account, context.environment), account,
                               context.environment.uid);
    const auto inspection = audit.inspect();
    if (inspection.status == LogoutAuditInspectionStatus::Clean) {
        return true;
    }
    if (inspection.status == LogoutAuditInspectionStatus::Invalid) {
        if (inspection.incomplete) {
            audit_incomplete(session, account, audit, inspection.incomplete->completed_stages);
        } else {
            audit_unavailable(session, account, audit,
                              inspection.failure.reason.empty() ? "path_invalid"
                                                                : inspection.failure.reason);
        }
        return false;
    }
    if (!inspection.incomplete) {
        audit_unavailable(session, account, audit);
        return false;
    }
    daemon_lock::Identity lock_identity;
    std::string lock_error;
    const int lock_descriptor = daemon_lock::acquire(
        paths::account_state_dir(account, context.environment) + "/daemon.lock", lock_identity,
        lock_error);
    if (lock_descriptor < 0) {
        audit_incomplete(session, account, audit, inspection.incomplete->completed_stages);
        return false;
    }
    auto reconciled = reconcile_definite_logout_audit(audit, audit_timestamp);
    ::close(lock_descriptor);
    if (reconciled.status == LogoutAuditReconcileStatus::Clean) {
        return true;
    }
    if (reconciled.status == LogoutAuditReconcileStatus::Invalid) {
        if (reconciled.incomplete) {
            audit_incomplete(session, account, audit, reconciled.incomplete->completed_stages);
        } else {
            audit_unavailable(session, account, audit,
                              reconciled.failure.reason.empty() ? "path_invalid"
                                                                : reconciled.failure.reason);
        }
        return false;
    }
    if (!reconciled.incomplete) {
        audit_unavailable(session, account, audit);
        return false;
    }
    audit_incomplete(session, account, audit, reconciled.incomplete->completed_stages);
    return false;
}

void mutation_failure(RequestSession& session, const ConfigGlobalContext& context,
                      const config::MutationResult& result, std::string_view expected,
                      std::string_view operation) {
    switch (result.status) {
    case config::MutationStatus::Conflict:
        session.error("CONFIG_CONFLICT", "config.toml changed before mutation",
                      {{"path", context.store.get().path()},
                       {"expected", expected},
                       {"current", result.snapshot ? result.snapshot->identity : "missing"}},
                      kGeneric);
        return;
    case config::MutationStatus::TimedOut:
        session.error("TIMEOUT", "config mutation timed out",
                      {{"operation", operation}, {"state", nullptr}}, kTimeout);
        return;
    case config::MutationStatus::Invalid:
    case config::MutationStatus::IoError:
    case config::MutationStatus::DurabilityUnknown:
        config_invalid(session, context,
                       result.error.value_or(config::ConfigError{
                           result.status == config::MutationStatus::DurabilityUnknown
                               ? config::ConfigReason::SyncError
                               : config::ConfigReason::IoError,
                           "config mutation failed"}));
        return;
    case config::MutationStatus::Cancelled:
        if (session.cancellation_requested()) {
            return;
        }
        session.error("INTERNAL", "config mutation was cancelled",
                      {{"operation", operation}, {"reason", "internal_error"}}, kGeneric);
        return;
    case config::MutationStatus::PreconditionFailed:
        session.error("INTERNAL", "config mutation precondition failed",
                      {{"operation", operation}, {"reason", "internal_error"}}, kGeneric);
        return;
    case config::MutationStatus::Applied:
        break;
    }
    session.error("INTERNAL", "invalid config mutation outcome",
                  {{"operation", operation}, {"reason", "internal_error"}}, kGeneric);
}

config::MutationControl mutation_control(const RequestSession& session) {
    return {session.deadline(), session.cancellation_token()};
}

void account_add(const ConfigGlobalContext& context, const proto::Request& request,
                 RequestSession& session) {
    std::string account;
    if (!validate_internal_args(request, session, true, account)) {
        return;
    }
    if (context.removal_journal != nullptr &&
        !preflight_account_removal_journal(*context.removal_journal, account, session)) {
        return;
    }
    const auto current = load_current(session, context, "account_add");
    if (!current) {
        return;
    }
    if (current->accounts.contains(account)) {
        session.error("ACCOUNT_EXISTS", "account already exists", {{"account", account}}, kUsage);
        return;
    }
    const bool becomes_default = current->accounts.empty();
    const auto result =
        context.store.get().add_account(current->identity, account, mutation_control(session));
    if (result.status != config::MutationStatus::Applied) {
        mutation_failure(session, context, result, current->identity, "account_add");
        return;
    }
    session.result({{"account", account}, {"created", true}, {"default", becomes_default}});
}

void account_list(const ConfigGlobalContext& context, const proto::Request& request,
                  RequestSession& session) {
    std::string ignored;
    if (!validate_internal_args(request, session, false, ignored)) {
        return;
    }
    const auto current = load_current(session, context, "account_list");
    if (!current) {
        return;
    }
    json items = json::array();
    for (const auto& [name, account] : current->accounts) {
        static_cast<void>(account);
        items.push_back(
            {{"name", name}, {"default", current->default_account == std::optional{name}}});
    }
    session.result({{"items", std::move(items)}, {"next", nullptr}});
}

void account_show(const ConfigGlobalContext& context, const proto::Request& request,
                  RequestSession& session) {
    std::string account;
    if (!validate_internal_args(request, session, true, account)) {
        return;
    }
    if (context.removal_journal != nullptr &&
        !preflight_account_removal_journal(*context.removal_journal, account, session)) {
        return;
    }
    const auto current = load_current(session, context, "account_show");
    if (!current) {
        return;
    }
    const auto found = current->accounts.find(account);
    if (found == current->accounts.end()) {
        session.error("ACCOUNT_NOT_FOUND", "account is not configured", {{"account", account}},
                      kNotFound);
        return;
    }
    std::string path_error;
    const auto data = paths::account_data_dir(account, context.environment);
    const auto state = paths::account_state_dir(account, context.environment);
    const auto socket = paths::socket_path(account, context.environment, path_error);
    if (!socket) {
        config_invalid(session, context,
                       {config::ConfigReason::PathInvalid, std::move(path_error)});
        return;
    }
    if (data.empty() || data.front() != '/' || state.empty() || state.front() != '/' ||
        socket->empty() || socket->front() != '/') {
        config_invalid(session, context,
                       {config::ConfigReason::PathInvalid,
                        "account paths must be absolute after XDG resolution"});
        return;
    }
    if (!account_show_audit_preflight(session, context, account)) {
        return;
    }
    const auto& value = found->second;
    session.result(
        {{"account", account},
         {"default", current->default_account == std::optional{account}},
         {"allow_write", value.allow_write},
         {"idle_exit", value.idle_exit ? json(value.idle_exit->count()) : json(nullptr)},
         {"credentials",
          {{"api_id", credential_source(value.api_id.has_value(), value.api_id_cmd.has_value())},
           {"api_hash",
            credential_source(value.api_hash.has_value(), value.api_hash_cmd.has_value())},
           {"db_key", value.db_key_cmd ? "command" : "none"},
           {"password", value.password_cmd ? "command" : "interactive"},
           {"bot_token", value.bot_token_cmd ? "command" : "interactive"}}},
         {"paths", {{"data", data}, {"state", state}, {"socket", socket.value()}}}});
}

void account_use(const ConfigGlobalContext& context, const proto::Request& request,
                 RequestSession& session) {
    std::string account;
    if (!validate_internal_args(request, session, true, account)) {
        return;
    }
    if (context.removal_journal != nullptr &&
        !preflight_account_removal_journal(*context.removal_journal, account, session)) {
        return;
    }
    const auto current = load_current(session, context, "account_use");
    if (!current) {
        return;
    }
    if (!current->accounts.contains(account)) {
        session.error("ACCOUNT_NOT_FOUND", "account is not configured", {{"account", account}},
                      kNotFound);
        return;
    }
    const json previous = current->default_account.has_value()
                              ? json(current->default_account.value_or(""))
                              : json(nullptr);
    const auto result =
        context.store.get().use_account(current->identity, account, mutation_control(session));
    if (result.status != config::MutationStatus::Applied) {
        mutation_failure(session, context, result, current->identity, "account_use");
        return;
    }
    session.result({{"default_account", account}, {"previous_default", previous}});
}

} // namespace

void register_account_commands(Dispatcher& dispatcher, const ConfigGlobalContext& context) {
    dispatcher.register_command(
        "account add",
        {Tier::Read, [&context](const proto::Request& request, RequestSession& session) {
             account_add(context, request, session);
         }});
    dispatcher.register_command(
        "account list",
        {Tier::Read, [&context](const proto::Request& request, RequestSession& session) {
             account_list(context, request, session);
         }});
    dispatcher.register_command(
        "account show",
        {Tier::Read, [&context](const proto::Request& request, RequestSession& session) {
             account_show(context, request, session);
         }});
    dispatcher.register_command(
        "account use",
        {Tier::Read, [&context](const proto::Request& request, RequestSession& session) {
             account_use(context, request, session);
         }});
}

} // namespace tgcli::daemon
