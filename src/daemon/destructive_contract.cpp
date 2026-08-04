#include "daemon/destructive_contract.hpp"

#include "common/paths.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <utility>

namespace tgcli::daemon {

namespace detail {

struct DestructiveContractAccess {
    static AuditIntent intent(nlohmann::json document) {
        return AuditIntent(std::move(document));
    }

    static AuditCheckpoint checkpoint(nlohmann::json document) {
        return AuditCheckpoint(std::move(document));
    }

    static AuditOutcome outcome(nlohmann::json document) {
        return AuditOutcome(std::move(document));
    }
};

} // namespace detail

namespace {

using nlohmann::json;

constexpr std::array kLogoutStages{AuditStage::IntentSynced, AuditStage::LogoutSendStarted,
                                   AuditStage::LogoutClosedConfirmed};

const std::array<std::vector<AuditStage>, 4> kRemovalStageBranches{
    std::vector<AuditStage>{
        AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteLogoutSendStarted,
        AuditStage::RemoteConfirmed, AuditStage::ClientCloseStarted, AuditStage::ClientClosed,
        AuditStage::ConfigRemoveStarted, AuditStage::ConfigRemoved, AuditStage::DataRemoveStarted,
        AuditStage::DataRemoved, AuditStage::StateRemoveStarted, AuditStage::StateRemoved},
    std::vector<AuditStage>{
        AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteLogoutSendStarted,
        AuditStage::RemoteNotPresent, AuditStage::ClientCloseStarted, AuditStage::ClientClosed,
        AuditStage::ConfigRemoveStarted, AuditStage::ConfigRemoved, AuditStage::DataRemoveStarted,
        AuditStage::DataRemoved, AuditStage::StateRemoveStarted, AuditStage::StateRemoved},
    std::vector<AuditStage>{
        AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteNotPresent,
        AuditStage::ClientCloseStarted, AuditStage::ClientClosed, AuditStage::ConfigRemoveStarted,
        AuditStage::ConfigRemoved, AuditStage::DataRemoveStarted, AuditStage::DataRemoved,
        AuditStage::StateRemoveStarted, AuditStage::StateRemoved},
    std::vector<AuditStage>{AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteKept,
                            AuditStage::ClientCloseStarted, AuditStage::ClientClosed,
                            AuditStage::ConfigRemoveStarted, AuditStage::ConfigRemoved,
                            AuditStage::DataRemoveStarted, AuditStage::DataRemoved,
                            AuditStage::StateRemoveStarted, AuditStage::StateRemoved},
};

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) {
        return false;
    }
    return std::all_of(fields.begin(), fields.end(), [&value](std::string_view field_name) {
        return value.contains(std::string(field_name));
    });
}

bool one_of(std::string_view value, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool string_one_of(const json& value, std::initializer_list<std::string_view> choices) {
    return value.is_string() && one_of(value.get_ref<const std::string&>(), choices);
}

bool integer(const json& value) {
    return value.is_number_integer() || value.is_number_unsigned();
}

bool nullable_integer(const json& value) {
    return value.is_null() || integer(value);
}

bool valid_account(const json& value) {
    return value.is_string() && paths::valid_account_name(value.get_ref<const std::string&>());
}

bool valid_invocation_id(std::string_view value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool fixed_digits(std::string_view value, std::size_t offset, std::size_t length) {
    return offset + length <= value.size() &&
           std::all_of(value.begin() + static_cast<std::ptrdiff_t>(offset),
                       value.begin() + static_cast<std::ptrdiff_t>(offset + length),
                       [](unsigned char character) { return std::isdigit(character) != 0; });
}

unsigned decimal_at(std::string_view value, std::size_t offset, std::size_t length) {
    unsigned result = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
        result = result * 10U + static_cast<unsigned>(value[index] - '0');
    }
    return result;
}

bool leap_year(unsigned year) {
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

bool valid_utc_rfc3339(std::string_view value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value.back() != 'Z' || !fixed_digits(value, 0, 4) ||
        !fixed_digits(value, 5, 2) || !fixed_digits(value, 8, 2) || !fixed_digits(value, 11, 2) ||
        !fixed_digits(value, 14, 2) || !fixed_digits(value, 17, 2)) {
        return false;
    }
    if (value.size() != 20) {
        if (value[19] != '.' || value.size() == 21 || !fixed_digits(value, 20, value.size() - 21)) {
            return false;
        }
    }
    const auto year = decimal_at(value, 0, 4);
    const auto month = decimal_at(value, 5, 2);
    const auto day = decimal_at(value, 8, 2);
    const auto hour = decimal_at(value, 11, 2);
    const auto minute = decimal_at(value, 14, 2);
    const auto second = decimal_at(value, 17, 2);
    if (year == 0 || month == 0 || month > 12 || day == 0 || hour > 23 || minute > 59 ||
        second > 60) {
        return false;
    }
    constexpr std::array<unsigned, 12> days_per_month{31, 28, 31, 30, 31, 30,
                                                      31, 31, 30, 31, 30, 31};
    const auto max_day = days_per_month.at(month - 1) + (month == 2 && leap_year(year) ? 1U : 0U);
    return day <= max_day;
}

bool validate_record_identity(const AuditRecordIdentity& identity, std::string& error) {
    if (!valid_invocation_id(identity.invocation_id)) {
        error = "audit invocation_id must be 32 lowercase hexadecimal characters";
        return false;
    }
    if (!valid_utc_rfc3339(identity.timestamp)) {
        error = "audit timestamp must be a canonical UTC RFC 3339 string";
        return false;
    }
    return true;
}

bool validate_intent_sources(AuthoritySource authority_source,
                             ConfirmationSource confirmation_source, std::string& error) {
    if (authority_source_name(authority_source).empty() ||
        confirmation_source_name(confirmation_source).empty()) {
        error = "audit intent source is outside the closed contract enum";
        return false;
    }
    return true;
}

bool has_stage(const std::vector<AuditStage>& stages, AuditStage target) {
    return std::find(stages.begin(), stages.end(), target) != stages.end();
}

json serialize_stages(const std::vector<AuditStage>& stages) {
    json result = json::array();
    for (const auto stage : stages) {
        result.push_back(audit_stage_name(stage));
    }
    return result;
}

json serialize_nullable_string(const std::optional<std::string>& value) {
    return value ? json(value.value_or(std::string{})) : json(nullptr);
}

DestructiveCommand command_for(const proto::DestructivePlan& plan) {
    return std::holds_alternative<proto::LogoutPlan>(plan) ? DestructiveCommand::Logout
                                                           : DestructiveCommand::AccountRemove;
}

std::string account_for(const proto::DestructivePlan& plan) {
    return std::visit([](const auto& value) { return value.account(); }, plan);
}

bool bytewise_less(const std::string& lhs, const std::string& rhs) {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](unsigned char left, unsigned char right) { return left < right; });
}

struct PostIntentErrorRule {
    std::string_view code;
    bool logout;
    bool account_remove;
};

constexpr std::array kPostIntentErrorRules{
    PostIntentErrorRule{"AUTH_FUNCTION_DENIED", true, true},
    PostIntentErrorRule{"CONFIG_INVALID", false, true},
    PostIntentErrorRule{"CONFIG_CONFLICT", false, true},
    PostIntentErrorRule{"REMOTE_LOGOUT_UNCONFIRMED", true, true},
    PostIntentErrorRule{"LOCAL_CLEANUP_FAILED", false, true},
    PostIntentErrorRule{"DAEMON_CONTROL_FAILED", false, true},
    PostIntentErrorRule{"RATE_LIMITED", true, true},
    PostIntentErrorRule{"TDLIB_ERROR", true, true},
    PostIntentErrorRule{"TIMEOUT", true, true},
    PostIntentErrorRule{"DAEMON_SHUTDOWN", true, true},
    PostIntentErrorRule{"INTERNAL", true, true},
};

bool removal_stage_branch_matches_plan(const proto::AccountRemovePlan& plan,
                                       const std::vector<AuditStage>& completed_stages) {
    const bool kept = has_stage(completed_stages, AuditStage::RemoteKept);
    const bool remote_logout = has_stage(completed_stages, AuditStage::RemoteLogoutSendStarted) ||
                               has_stage(completed_stages, AuditStage::RemoteConfirmed) ||
                               has_stage(completed_stages, AuditStage::RemoteNotPresent);
    return (!kept || plan.keep_session()) && (!remote_logout || plan.remote_logout());
}

bool cleanup_paths_match_plan(const json& details, const proto::AccountRemovePlan& plan) {
    std::vector<std::string> actual;
    actual.reserve(details["removed"].size() + details["retained"].size());
    for (const auto& value : details["removed"]) {
        actual.push_back(value.get<std::string>());
    }
    for (const auto& value : details["retained"]) {
        actual.push_back(value.get<std::string>());
    }
    std::ranges::sort(actual, bytewise_less);

    std::vector<std::string> expected{plan.delete_paths().begin(), plan.delete_paths().end()};
    std::ranges::sort(expected, bytewise_less);
    return actual == expected;
}

bool validate_logout_error_binding(const StructuredOutcomeError& outcome_error,
                                   std::string& error) {
    if (outcome_error.code() == "AUTH_FUNCTION_DENIED" &&
        outcome_error.details()["function"] != "logOut") {
        error = "logout auth-function error must identify logOut";
        return false;
    }
    return true;
}

bool validate_removal_error_binding(const proto::AccountRemovePlan& removal,
                                    const std::vector<AuditStage>& completed_stages,
                                    const StructuredOutcomeError& outcome_error,
                                    std::string& error) {
    const auto& details = outcome_error.details();
    if (!removal_stage_branch_matches_plan(removal, completed_stages)) {
        error = "removal audit stages do not match the plan's remote policy";
        return false;
    }
    if (one_of(outcome_error.code(), {"CONFIG_INVALID", "CONFIG_CONFLICT"}) &&
        details["path"] != removal.config_path()) {
        error = "structured config error path does not match the removal plan";
        return false;
    }
    if (outcome_error.code() == "CONFIG_CONFLICT" &&
        (details["expected"] != removal.config_snapshot() ||
         details["current"] == details["expected"])) {
        error = "structured config conflict does not match the planned snapshot transition";
        return false;
    }
    if (outcome_error.code() == "LOCAL_CLEANUP_FAILED" &&
        !cleanup_paths_match_plan(details, removal)) {
        error = "structured cleanup paths do not partition the removal roots";
        return false;
    }
    if (outcome_error.code() == "REMOTE_LOGOUT_UNCONFIRMED" && removal.keep_session()) {
        error = "keep-session removal cannot report an unconfirmed remote logout";
        return false;
    }
    if (outcome_error.code() == "AUTH_FUNCTION_DENIED") {
        const auto& function = details["function"];
        if (function != "close" && (function != "logOut" || removal.keep_session())) {
            error = "removal auth-function error does not match the plan's TDLib work";
            return false;
        }
    }
    if (outcome_error.code() == "DAEMON_CONTROL_FAILED" && details["operation"] != "stop") {
        error = "removal daemon-control error must identify stop";
        return false;
    }
    return true;
}

bool validate_post_intent_error_binding(const proto::DestructivePlan& plan,
                                        const std::vector<AuditStage>& completed_stages,
                                        const StructuredOutcomeError& outcome_error,
                                        std::string& error) {
    if (!has_stage(completed_stages, AuditStage::IntentSynced)) {
        error = "failure audit outcome requires a durable intent";
        return false;
    }

    const auto command = command_for(plan);
    if (outcome_error.code() == "CONFIRMATION_REQUIRED" &&
        (outcome_error.details()["action"] != destructive_command_name(command) ||
         outcome_error.details()["target"] != proto::serialize(plan))) {
        error = "confirmation error target does not match the destructive plan";
        return false;
    }
    const auto* const rule = std::find_if(
        kPostIntentErrorRules.begin(), kPostIntentErrorRules.end(),
        [&outcome_error](const auto& item) { return item.code == outcome_error.code(); });
    if (rule == kPostIntentErrorRules.end() ||
        (command == DestructiveCommand::Logout && !rule->logout) ||
        (command == DestructiveCommand::AccountRemove && !rule->account_remove)) {
        error = "structured error is not legal for this post-intent command";
        return false;
    }

    const auto& details = outcome_error.details();
    const auto account = account_for(plan);
    if (details.contains("account") && details["account"] != account) {
        error = "structured error account does not match the destructive plan";
        return false;
    }

    if (one_of(outcome_error.code(), {"RATE_LIMITED", "TDLIB_ERROR", "TIMEOUT", "INTERNAL"}) &&
        details["operation"] != destructive_command_name(command)) {
        error = "structured error operation does not match the destructive command";
        return false;
    }

    if (command == DestructiveCommand::Logout) {
        return validate_logout_error_binding(outcome_error, error);
    }

    const auto& removal = std::get<proto::AccountRemovePlan>(plan);
    return validate_removal_error_binding(removal, completed_stages, outcome_error, error);
}

bool sorted_unique_string_array(const json& value, bool accounts = false) {
    if (!value.is_array()) {
        return false;
    }
    std::optional<std::string> prior;
    for (const auto& entry : value) {
        if (!entry.is_string() || (accounts && !valid_account(entry))) {
            return false;
        }
        const auto current = entry.get<std::string>();
        if (prior && !bytewise_less(*prior, current)) {
            return false;
        }
        prior = current;
    }
    return true;
}

bool valid_state(const json& value, bool nullable = false) {
    if (nullable && value.is_null()) {
        return true;
    }
    return string_one_of(value, {"unknown", "wait_tdlib_parameters", "wait_phone_number",
                                 "wait_premium_purchase", "wait_email_address", "wait_email_code",
                                 "wait_code", "wait_other_device_confirmation", "wait_registration",
                                 "wait_password", "ready", "logging_out", "closing", "closed"});
}

bool valid_operation(const json& value) {
    return string_one_of(value, {"auth_bootstrap", "login", "logout", "me", "account_add",
                                 "account_list", "account_show", "account_use", "account_remove",
                                 "doctor", "daemon_status", "daemon_stop", "daemon_restart",
                                 "daemon_run", "config_reload", "audit"});
}

bool valid_stage_array(const json& value, DestructiveCommand command) {
    if (!value.is_array()) {
        return false;
    }
    std::vector<AuditStage> stages;
    stages.reserve(value.size());
    for (const auto& entry : value) {
        if (!entry.is_string()) {
            return false;
        }
        auto stage = parse_audit_stage(entry.get_ref<const std::string&>());
        if (!stage) {
            return false;
        }
        stages.push_back(*stage);
    }
    std::string ignored;
    return validate_audit_stage_prefix(command, stages, ignored);
}

// The contract deliberately assigns a distinct closed detail shape to each M1 error code.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool valid_error_details(std::string_view code, const json& details) {
    if (code == "USAGE") {
        return details.empty() ||
               (exact_fields(details, {"argument", "reason"}) &&
                (details["argument"].is_null() || details["argument"].is_string()) &&
                string_one_of(details["reason"],
                              {"missing_argument", "invalid_argument", "mutually_exclusive",
                               "unknown_command", "invalid_environment", "unsupported_mode"}));
    }
    if (code == "INSECURE_SECRET_INPUT") {
        return exact_fields(details, {"argument", "replacement"}) &&
               details["argument"] == "--bot-token" && details["replacement"] == "--bot";
    }
    if (code == "ACCOUNT_EXISTS" || code == "ACCOUNT_NOT_FOUND") {
        return exact_fields(details, {"account"}) && valid_account(details["account"]);
    }
    if (code == "DEFAULT_REASSIGNMENT_REQUIRED") {
        return exact_fields(details, {"account", "candidates"}) &&
               valid_account(details["account"]) &&
               sorted_unique_string_array(details["candidates"], true);
    }
    if (code == "CONFIG_INVALID") {
        return exact_fields(details, {"path", "reason"}) && details["path"].is_string() &&
               string_one_of(details["reason"],
                             {"path_invalid", "wrong_owner", "wrong_type", "wrong_mode",
                              "wrong_link_count", "too_large", "parse_error", "type_error",
                              "invalid_default", "conflicting_credentials", "io_error",
                              "sync_error"});
    }
    if (code == "CONFIG_CONFLICT") {
        return exact_fields(details, {"path", "expected", "current"}) &&
               details["path"].is_string() && details["expected"].is_string() &&
               details["current"].is_string() &&
               proto::valid_config_snapshot_identity(
                   details["expected"].get_ref<const std::string&>()) &&
               proto::valid_config_snapshot_identity(
                   details["current"].get_ref<const std::string&>());
    }
    if (code == "HOOK_FAILED") {
        return exact_fields(details, {"hook", "reason", "status"}) &&
               string_one_of(details["hook"], {"api_id_cmd", "api_hash_cmd", "db_key_cmd",
                                               "password_cmd", "bot_token_cmd"}) &&
               string_one_of(details["reason"],
                             {"spawn", "exit", "signal", "timeout", "stdout_empty",
                              "stdout_invalid", "stdout_too_large", "stderr_too_large"}) &&
               nullable_integer(details["status"]);
    }
    if (code == "AUTH_FLOW_IN_PROGRESS") {
        return exact_fields(details, {"account", "state"}) && valid_account(details["account"]) &&
               valid_state(details["state"]);
    }
    if (code == "NOT_AUTHED") {
        return exact_fields(details, {"account", "state", "reason"}) &&
               valid_account(details["account"]) && valid_state(details["state"]) &&
               string_one_of(details["reason"],
                             {"not_ready", "authorization_lost", "login_required"});
    }
    if (code == "AUTH_INPUT_REQUIRED" || code == "AUTH_CANCELLED") {
        return exact_fields(details, {"account", "state", "challenge"}) &&
               valid_account(details["account"]) && valid_state(details["state"]) &&
               details["challenge"].is_string() &&
               proto::parse_challenge_kind(details["challenge"].get_ref<const std::string&>())
                   .has_value();
    }
    if (code == "AUTH_CREDENTIAL_REJECTED") {
        return exact_fields(details, {"account", "state", "credential", "tdlib_code"}) &&
               valid_account(details["account"]) && valid_state(details["state"]) &&
               string_one_of(details["credential"],
                             {"app_credentials", "database_key", "phone_number", "bot_token",
                              "email_address", "email_code", "authentication_code",
                              "registration_name", "password"}) &&
               integer(details["tdlib_code"]);
    }
    if (code == "AUTH_PREMIUM_REQUIRED") {
        return exact_fields(details, {"account", "state", "store_product_id", "premium_day_count",
                                      "support_email_address", "support_email_subject"}) &&
               valid_account(details["account"]) && details["state"] == "wait_premium_purchase" &&
               details["store_product_id"].is_string() && integer(details["premium_day_count"]) &&
               details["support_email_address"].is_string() &&
               details["support_email_subject"].is_string();
    }
    if (code == "UNSUPPORTED_AUTH_STATE") {
        return exact_fields(details, {"account", "tdlib_type_id"}) &&
               valid_account(details["account"]) && integer(details["tdlib_type_id"]);
    }
    if (code == "AUTH_FUNCTION_DENIED") {
        return exact_fields(details, {"account", "state", "function"}) &&
               valid_account(details["account"]) && valid_state(details["state"]) &&
               string_one_of(details["function"],
                             {"getAuthorizationState", "setTdlibParameters",
                              "setAuthenticationPhoneNumber", "requestQrCodeAuthentication",
                              "checkAuthenticationBotToken", "setAuthenticationEmailAddress",
                              "checkAuthenticationEmailCode", "checkAuthenticationCode",
                              "registerUser", "checkAuthenticationPassword", "getMe", "logOut",
                              "close", "other"});
    }
    if (code == "PROTOCOL_ANSWER_INVALID") {
        return exact_fields(details, {"request_id", "reason"}) &&
               details["request_id"].is_number_unsigned() &&
               string_one_of(details["reason"],
                             {"future_sequence", "nonce_mismatch", "generation_mismatch",
                              "malformed", "unknown_request"});
    }
    if (code == "WRITE_DENIED") {
        return exact_fields(details, {"account", "reason"}) && valid_account(details["account"]) &&
               string_one_of(details["reason"],
                             {"explicit_deny", "no_grant", "invalid_config_grant"});
    }
    if (code == "CONFIRMATION_REQUIRED") {
        if (!exact_fields(details, {"account", "action", "target"}) ||
            !valid_account(details["account"]) || !details["action"].is_string()) {
            return false;
        }
        std::string ignored;
        auto plan = proto::parse_destructive_plan(details["target"], ignored);
        if (!plan || account_for(*plan) != details["account"].get_ref<const std::string&>()) {
            return false;
        }
        return (details["action"] == "logout" &&
                std::holds_alternative<proto::LogoutPlan>(*plan)) ||
               (details["action"] == "account_remove" &&
                std::holds_alternative<proto::AccountRemovePlan>(*plan));
    }
    if (code == "AUDIT_UNAVAILABLE") {
        return exact_fields(details, {"account", "path", "reason"}) &&
               valid_account(details["account"]) && details["path"].is_string() &&
               string_one_of(details["reason"], {"path_invalid", "open_failed", "write_failed",
                                                 "sync_failed", "rotate_failed"});
    }
    if (code == "AUDIT_INCOMPLETE") {
        if (!exact_fields(details, {"account", "path", "mutation_state", "completed_stages"}) ||
            !valid_account(details["account"]) || !details["path"].is_string() ||
            !string_one_of(details["mutation_state"], {"none", "possible", "confirmed"}) ||
            !valid_stage_array(details["completed_stages"], DestructiveCommand::Logout)) {
            return false;
        }
        std::vector<AuditStage> stages;
        for (const auto& entry : details["completed_stages"]) {
            const auto stage = parse_audit_stage(entry.get_ref<const std::string&>());
            if (!stage) {
                return false;
            }
            stages.push_back(*stage);
        }
        std::string ignored;
        const auto mutation = derive_mutation_state(DestructiveCommand::Logout, stages, ignored);
        return mutation && details["mutation_state"].get_ref<const std::string&>() ==
                               mutation_state_name(*mutation);
    }
    if (code == "REMOTE_LOGOUT_UNCONFIRMED") {
        return exact_fields(details, {"account", "state", "reason"}) &&
               valid_account(details["account"]) && valid_state(details["state"]) &&
               string_one_of(details["reason"], {"tdlib_error", "timeout", "generation_lost",
                                                 "transport_lost", "state_unproven"});
    }
    if (code == "LOCAL_CLEANUP_FAILED") {
        if (!exact_fields(details, {"account", "reason", "removed", "retained"}) ||
            !valid_account(details["account"]) ||
            !string_one_of(details["reason"], {"path_changed", "path_invalid", "mount_boundary",
                                               "io_error", "sync_error"}) ||
            !sorted_unique_string_array(details["removed"]) ||
            !sorted_unique_string_array(details["retained"])) {
            return false;
        }
        return std::ranges::all_of(details["removed"], [&details](const auto& removed) {
            return std::find(details["retained"].begin(), details["retained"].end(), removed) ==
                   details["retained"].end();
        });
    }
    if (code == "REMOVAL_INCOMPLETE") {
        if (!exact_fields(details, {"account", "path", "invocation_id", "stage", "completed_stages",
                                    "reason"}) ||
            !valid_account(details["account"]) || !details["path"].is_string() ||
            !details["invocation_id"].is_string() ||
            !valid_invocation_id(details["invocation_id"].get_ref<const std::string&>()) ||
            !details["stage"].is_string() ||
            !string_one_of(details["reason"],
                           {"prior_crash", "identity_ambiguous", "outcome_missing"}) ||
            !valid_stage_array(details["completed_stages"], DestructiveCommand::AccountRemove) ||
            details["completed_stages"].empty()) {
            return false;
        }
        const auto stage = parse_audit_stage(details["stage"].get_ref<const std::string&>());
        return stage && *stage != AuditStage::OutcomeSynced &&
               details["completed_stages"].back() == details["stage"];
    }
    if (code == "DAEMON_NOT_RUNNING") {
        return exact_fields(details, {"account", "socket"}) && valid_account(details["account"]) &&
               details["socket"].is_string();
    }
    if (code == "DAEMON_CONTROL_FAILED") {
        return exact_fields(details, {"account", "operation", "reason"}) &&
               valid_account(details["account"]) &&
               string_one_of(details["operation"], {"status", "stop", "restart"}) &&
               string_one_of(details["reason"],
                             {"surface_invalid", "identity_changed", "handshake_failed",
                              "shutdown_failed", "replacement_failed"});
    }
    if (code == "RATE_LIMITED") {
        return exact_fields(details, {"operation", "tdlib_code", "retry_after"}) &&
               valid_operation(details["operation"]) && details["tdlib_code"] == 429 &&
               integer(details["retry_after"]);
    }
    if (code == "TDLIB_ERROR") {
        return exact_fields(details, {"operation", "tdlib_code"}) &&
               valid_operation(details["operation"]) && integer(details["tdlib_code"]);
    }
    if (code == "TIMEOUT") {
        return exact_fields(details, {"operation", "state"}) &&
               valid_operation(details["operation"]) && valid_state(details["state"], true);
    }
    if (code == "DAEMON_SHUTDOWN") {
        return exact_fields(details, {"reason"}) && details["reason"] == "daemon_shutdown";
    }
    if (code == "INTERNAL") {
        return exact_fields(details, {"operation", "reason"}) &&
               valid_operation(details["operation"]) && details["reason"] == "internal_error";
    }
    return false;
}

std::optional<AuditOutcome> make_outcome(AuditRecordIdentity identity,
                                         const proto::DestructivePlan& plan,
                                         const std::vector<AuditStage>& completed_stages,
                                         bool success, json result, json error_value,
                                         std::string& error) {
    if (!validate_record_identity(identity, error)) {
        return std::nullopt;
    }
    const auto command = command_for(plan);
    const auto mutation = derive_mutation_state(command, completed_stages, error);
    if (!mutation) {
        return std::nullopt;
    }
    return detail::DestructiveContractAccess::outcome(
        {{"schema_version", 1},
         {"phase", "outcome"},
         {"invocation_id", std::move(identity.invocation_id)},
         {"timestamp", std::move(identity.timestamp)},
         {"account", account_for(plan)},
         {"command", destructive_command_name(command)},
         {"success", success},
         {"mutation_state", mutation_state_name(*mutation)},
         {"completed_stages", serialize_stages(completed_stages)},
         {"result", std::move(result)},
         {"error", std::move(error_value)}});
}

} // namespace

AuthorityDecision evaluate_destructive_authority(const proto::RequestContext& request,
                                                 ConfigWriteAuthority config) {
    if (request.dry_run) {
        return DryRunAuthority{};
    }
    switch (request.write_authority) {
    case proto::WriteAuthority::Deny:
        return DeniedAuthority{WriteDenialReason::ExplicitDeny};
    case proto::WriteAuthority::Grant:
        return GrantedAuthority{AuthoritySource::Request};
    case proto::WriteAuthority::Unset:
        break;
    default:
        return DeniedAuthority{WriteDenialReason::NoGrant};
    }
    if (!config.grant_valid) {
        return DeniedAuthority{WriteDenialReason::InvalidConfigGrant};
    }
    if (config.allow_write) {
        return GrantedAuthority{AuthoritySource::Config};
    }
    return DeniedAuthority{WriteDenialReason::NoGrant};
}

std::string_view authority_source_name(AuthoritySource source) {
    switch (source) {
    case AuthoritySource::Request:
        return "request";
    case AuthoritySource::Config:
        return "config";
    }
    return {};
}

std::string_view write_denial_reason_name(WriteDenialReason reason) {
    switch (reason) {
    case WriteDenialReason::ExplicitDeny:
        return "explicit_deny";
    case WriteDenialReason::NoGrant:
        return "no_grant";
    case WriteDenialReason::InvalidConfigGrant:
        return "invalid_config_grant";
    }
    return {};
}

std::string_view destructive_command_name(DestructiveCommand command) {
    switch (command) {
    case DestructiveCommand::Logout:
        return "logout";
    case DestructiveCommand::AccountRemove:
        return "account_remove";
    }
    return {};
}

std::string_view confirmation_source_name(ConfirmationSource source) {
    switch (source) {
    case ConfirmationSource::Yes:
        return "yes";
    case ConfirmationSource::Tty:
        return "tty";
    }
    return {};
}

std::string_view mutation_state_name(MutationState state) {
    switch (state) {
    case MutationState::None:
        return "none";
    case MutationState::Possible:
        return "possible";
    case MutationState::Confirmed:
        return "confirmed";
    }
    return {};
}

std::string_view account_remove_remote_result_name(AccountRemoveRemoteResult result) {
    switch (result) {
    case AccountRemoveRemoteResult::Confirmed:
        return "confirmed";
    case AccountRemoveRemoteResult::NotPresent:
        return "not_present";
    case AccountRemoveRemoteResult::Kept:
        return "kept";
    }
    return {};
}

std::string_view audit_stage_name(AuditStage stage) {
    switch (stage) {
    case AuditStage::Planned:
        return "planned";
    case AuditStage::IntentSynced:
        return "intent_synced";
    case AuditStage::LogoutSendStarted:
        return "logout_send_started";
    case AuditStage::LogoutClosedConfirmed:
        return "logout_closed_confirmed";
    case AuditStage::RemoteLogoutSendStarted:
        return "remote_logout_send_started";
    case AuditStage::RemoteConfirmed:
        return "remote_confirmed";
    case AuditStage::RemoteNotPresent:
        return "remote_not_present";
    case AuditStage::RemoteKept:
        return "remote_kept";
    case AuditStage::ClientCloseStarted:
        return "client_close_started";
    case AuditStage::ClientClosed:
        return "client_closed";
    case AuditStage::ConfigRemoveStarted:
        return "config_remove_started";
    case AuditStage::ConfigRemoved:
        return "config_removed";
    case AuditStage::DataRemoveStarted:
        return "data_remove_started";
    case AuditStage::DataRemoved:
        return "data_removed";
    case AuditStage::StateRemoveStarted:
        return "state_remove_started";
    case AuditStage::StateRemoved:
        return "state_removed";
    case AuditStage::OutcomeSynced:
        return "outcome_synced";
    }
    return {};
}

std::optional<AuditStage> parse_audit_stage(std::string_view name) {
    constexpr std::array stages{
        AuditStage::Planned,
        AuditStage::IntentSynced,
        AuditStage::LogoutSendStarted,
        AuditStage::LogoutClosedConfirmed,
        AuditStage::RemoteLogoutSendStarted,
        AuditStage::RemoteConfirmed,
        AuditStage::RemoteNotPresent,
        AuditStage::RemoteKept,
        AuditStage::ClientCloseStarted,
        AuditStage::ClientClosed,
        AuditStage::ConfigRemoveStarted,
        AuditStage::ConfigRemoved,
        AuditStage::DataRemoveStarted,
        AuditStage::DataRemoved,
        AuditStage::StateRemoveStarted,
        AuditStage::StateRemoved,
        AuditStage::OutcomeSynced,
    };
    const auto* const found = std::find_if(stages.begin(), stages.end(), [name](AuditStage stage) {
        return audit_stage_name(stage) == name;
    });
    return found == stages.end() ? std::nullopt : std::optional<AuditStage>(*found);
}

bool validate_audit_stage_prefix(DestructiveCommand command,
                                 const std::vector<AuditStage>& completed_stages,
                                 std::string& error) {
    bool valid = false;
    switch (command) {
    case DestructiveCommand::Logout:
        valid = !completed_stages.empty() && completed_stages.size() <= kLogoutStages.size() &&
                std::equal(completed_stages.begin(), completed_stages.end(), kLogoutStages.begin());
        break;
    case DestructiveCommand::AccountRemove:
        valid = !completed_stages.empty() &&
                std::any_of(kRemovalStageBranches.begin(), kRemovalStageBranches.end(),
                            [&completed_stages](const auto& branch) {
                                return completed_stages.size() <= branch.size() &&
                                       std::equal(completed_stages.begin(), completed_stages.end(),
                                                  branch.begin());
                            });
        break;
    default:
        error = "destructive command is outside the closed contract enum";
        return false;
    }
    if (!valid) {
        error = "completed audit stages are not a legal command prefix";
        return false;
    }
    error.clear();
    return true;
}

std::optional<MutationState> derive_mutation_state(DestructiveCommand command,
                                                   const std::vector<AuditStage>& completed_stages,
                                                   std::string& error) {
    if (!validate_audit_stage_prefix(command, completed_stages, error)) {
        return std::nullopt;
    }
    constexpr std::array pairs{
        std::pair{AuditStage::LogoutSendStarted, AuditStage::LogoutClosedConfirmed},
        std::pair{AuditStage::RemoteLogoutSendStarted, AuditStage::RemoteConfirmed},
        std::pair{AuditStage::ConfigRemoveStarted, AuditStage::ConfigRemoved},
        std::pair{AuditStage::DataRemoveStarted, AuditStage::DataRemoved},
        std::pair{AuditStage::StateRemoveStarted, AuditStage::StateRemoved},
    };
    for (const auto& [started, completed] : pairs) {
        if (has_stage(completed_stages, started) && !has_stage(completed_stages, completed)) {
            return MutationState::Possible;
        }
    }
    constexpr std::array confirmed{AuditStage::LogoutClosedConfirmed, AuditStage::RemoteConfirmed,
                                   AuditStage::ConfigRemoved, AuditStage::DataRemoved,
                                   AuditStage::StateRemoved};
    const bool has_confirmed =
        std::any_of(confirmed.begin(), confirmed.end(), [&completed_stages](AuditStage stage) {
            return has_stage(completed_stages, stage);
        });
    return has_confirmed ? MutationState::Confirmed : MutationState::None;
}

StructuredOutcomeError::StructuredOutcomeError(std::string code, json details)
    : code_(std::move(code)), details_(std::move(details)) {}

const std::string& StructuredOutcomeError::code() const {
    return code_;
}

const json& StructuredOutcomeError::details() const {
    return details_;
}

std::optional<StructuredOutcomeError> parse_structured_outcome_error(const json& value,
                                                                     std::string& error) {
    if (!exact_fields(value, {"code", "details"}) || !value["code"].is_string() ||
        !value["details"].is_object()) {
        error = "structured outcome error must contain exactly code and details";
        return std::nullopt;
    }
    const auto code = value["code"].get<std::string>();
    if (!valid_error_details(code, value["details"])) {
        error = "structured outcome error code or details do not match the M1 contract";
        return std::nullopt;
    }
    error.clear();
    return StructuredOutcomeError(code, value["details"]);
}

json serialize(const StructuredOutcomeError& error) {
    return {{"code", error.code()}, {"details", error.details()}};
}

AuditIntent::AuditIntent(json document) : document_(std::move(document)) {}
AuditCheckpoint::AuditCheckpoint(json document) : document_(std::move(document)) {}
AuditOutcome::AuditOutcome(json document) : document_(std::move(document)) {}

std::optional<AuditIntent>
make_logout_audit_intent(AuditRecordIdentity identity, const proto::LogoutPlan& plan,
                         std::string config_snapshot, AuthoritySource authority_source,
                         ConfirmationSource confirmation_source, std::string& error) {
    if (!validate_record_identity(identity, error) ||
        !validate_intent_sources(authority_source, confirmation_source, error)) {
        return std::nullopt;
    }
    if (!proto::valid_config_snapshot_identity(config_snapshot)) {
        error = "logout audit config snapshot identity is invalid";
        return std::nullopt;
    }
    error.clear();
    return detail::DestructiveContractAccess::intent(
        {{"schema_version", 1},
         {"phase", "intent"},
         {"invocation_id", std::move(identity.invocation_id)},
         {"timestamp", std::move(identity.timestamp)},
         {"account", plan.account()},
         {"command", "logout"},
         {"arguments", json::object()},
         {"plan", proto::serialize(plan)},
         {"config_snapshot", std::move(config_snapshot)},
         {"authority_source", authority_source_name(authority_source)},
         {"confirmation_source", confirmation_source_name(confirmation_source)}});
}

std::optional<AuditIntent> make_account_remove_audit_intent(AuditRecordIdentity identity,
                                                            const proto::AccountRemovePlan& plan,
                                                            AuthoritySource authority_source,
                                                            ConfirmationSource confirmation_source,
                                                            std::string& error) {
    if (!validate_record_identity(identity, error) ||
        !validate_intent_sources(authority_source, confirmation_source, error)) {
        return std::nullopt;
    }
    error.clear();
    return detail::DestructiveContractAccess::intent(
        {{"schema_version", 1},
         {"phase", "intent"},
         {"invocation_id", std::move(identity.invocation_id)},
         {"timestamp", std::move(identity.timestamp)},
         {"account", plan.account()},
         {"command", "account_remove"},
         {"arguments",
          {{"keep_session", plan.keep_session()},
           {"reassign_default", serialize_nullable_string(plan.reassign_default())}}},
         {"plan", proto::serialize(plan)},
         {"config_snapshot", plan.config_snapshot()},
         {"authority_source", authority_source_name(authority_source)},
         {"confirmation_source", confirmation_source_name(confirmation_source)}});
}

std::optional<AuditCheckpoint> make_logout_audit_checkpoint(AuditRecordIdentity identity,
                                                            const proto::LogoutPlan& plan,
                                                            AuditStage stage, std::string& error) {
    if (!validate_record_identity(identity, error)) {
        return std::nullopt;
    }
    if (stage != AuditStage::LogoutSendStarted && stage != AuditStage::LogoutClosedConfirmed) {
        error = "logout checkpoint stage is invalid";
        return std::nullopt;
    }
    error.clear();
    return detail::DestructiveContractAccess::checkpoint(
        {{"schema_version", 1},
         {"phase", "checkpoint"},
         {"invocation_id", std::move(identity.invocation_id)},
         {"timestamp", std::move(identity.timestamp)},
         {"account", plan.account()},
         {"command", "logout"},
         {"stage", audit_stage_name(stage)}});
}

std::optional<AuditOutcome>
make_logout_success_audit_outcome(AuditRecordIdentity identity, const proto::LogoutPlan& plan,
                                  const std::vector<AuditStage>& completed_stages,
                                  std::string& error) {
    if (completed_stages.size() != kLogoutStages.size() ||
        !std::equal(completed_stages.begin(), completed_stages.end(), kLogoutStages.begin())) {
        error = "successful logout outcome requires the confirmed logout stage sequence";
        return std::nullopt;
    }
    return make_outcome(std::move(identity), proto::DestructivePlan{plan}, completed_stages, true,
                        {{"account", plan.account()}, {"logged_out", true}}, nullptr, error);
}

std::optional<AuditOutcome> make_account_remove_success_audit_outcome(
    AuditRecordIdentity identity, const proto::AccountRemovePlan& plan,
    AccountRemoveRemoteResult remote_result, const std::vector<AuditStage>& completed_stages,
    std::string& error) {
    const bool remote_matches_plan =
        (plan.keep_session() && remote_result == AccountRemoveRemoteResult::Kept) ||
        (plan.remote_logout() && remote_result != AccountRemoveRemoteResult::Kept);
    const AuditStage expected_remote_stage = [&] {
        switch (remote_result) {
        case AccountRemoveRemoteResult::Confirmed:
            return AuditStage::RemoteConfirmed;
        case AccountRemoveRemoteResult::NotPresent:
            return AuditStage::RemoteNotPresent;
        case AccountRemoveRemoteResult::Kept:
            return AuditStage::RemoteKept;
        }
        return AuditStage::OutcomeSynced;
    }();
    if (!remote_matches_plan || completed_stages.empty() ||
        completed_stages.back() != AuditStage::StateRemoved ||
        !has_stage(completed_stages, expected_remote_stage)) {
        error = "successful account removal outcome does not match its completed remote branch";
        return std::nullopt;
    }
    json result{{"account", plan.account()},
                {"removed", true},
                {"remote_logout", account_remove_remote_result_name(remote_result)},
                {"default_account", serialize_nullable_string(plan.reassign_default())}};
    return make_outcome(std::move(identity), proto::DestructivePlan{plan}, completed_stages, true,
                        std::move(result), nullptr, error);
}

std::optional<AuditOutcome>
make_failure_audit_outcome(AuditRecordIdentity identity, const proto::DestructivePlan& plan,
                           const std::vector<AuditStage>& completed_stages,
                           const StructuredOutcomeError& outcome_error, std::string& error) {
    if (!validate_post_intent_error_binding(plan, completed_stages, outcome_error, error)) {
        return std::nullopt;
    }
    return make_outcome(std::move(identity), plan, completed_stages, false, nullptr,
                        serialize(outcome_error), error);
}

json serialize(const AuditIntent& record) {
    return record.document_;
}

json serialize(const AuditCheckpoint& record) {
    return record.document_;
}

json serialize(const AuditOutcome& record) {
    return record.document_;
}

} // namespace tgcli::daemon
