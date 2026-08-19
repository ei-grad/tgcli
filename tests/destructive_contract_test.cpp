#include "daemon/destructive_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace tgcli;

namespace {

constexpr auto kSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
    "ino:2;size:3;ctime_ns:4";
constexpr auto kOtherSnapshot =
    "sha256:1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
    "ino:2;size:3;ctime_ns:4";

daemon::AuditRecordIdentity identity() {
    return {"00112233445566778899aabbccddeeff", "2026-08-04T12:34:56.123456Z"};
}

proto::LogoutPlan logout_plan() {
    std::string error;
    auto plan = proto::make_logout_plan("main", error);
    INFO(error);
    REQUIRE(plan.has_value());
    return *plan;
}

proto::AccountRemovePlan removal_plan(bool keep_session = false) {
    proto::AccountRemovePlanInput input{
        .account = "work",
        .keep_session = keep_session,
        .delete_paths = {"/data/accounts/work", "/state/accounts/work"},
        .config_path = "/config/tgcli/config.toml",
        .config_snapshot = kSnapshot,
        .data_root = proto::RootIdentity{"/data/accounts/work", 1, 2, 1000},
        .state_root = proto::RootIdentity{"/state/accounts/work", 1, 3, 1000},
        .reassign_default = "main",
    };
    std::string error;
    auto plan = proto::make_account_remove_plan(std::move(input), error);
    INFO(error);
    REQUIRE(plan.has_value());
    return *plan;
}

std::vector<daemon::AuditStage> removal_branch(daemon::AuditStage remote, bool sent = false) {
    std::vector<daemon::AuditStage> result{daemon::AuditStage::Planned,
                                           daemon::AuditStage::IntentSynced};
    if (sent) {
        result.push_back(daemon::AuditStage::RemoteLogoutSendStarted);
    }
    result.push_back(remote);
    const std::array suffix{
        daemon::AuditStage::ClientCloseStarted,  daemon::AuditStage::ClientClosed,
        daemon::AuditStage::ConfigRemoveStarted, daemon::AuditStage::ConfigRemoved,
        daemon::AuditStage::DataRemoveStarted,   daemon::AuditStage::DataRemoved,
        daemon::AuditStage::StateRemoveStarted,  daemon::AuditStage::StateRemoved,
    };
    result.reserve(result.size() + suffix.size());
    for (const auto stage : suffix) {
        result.push_back(stage);
    }
    return result;
}

std::set<std::string> keys(const json& value) {
    std::set<std::string> result;
    for (auto it = value.begin(); it != value.end(); ++it) {
        result.insert(it.key());
    }
    return result;
}

bool contains_forbidden_secret_key(const json& value) {
    static const std::set<std::string> forbidden{"answer",    "api_hash",     "argv",
                                                 "bot_token", "database_key", "password",
                                                 "raw_argv",  "secret"};
    std::vector<const json*> pending{&value};
    while (!pending.empty()) {
        const auto* current = pending.back();
        pending.pop_back();
        if (current->is_object()) {
            for (auto it = current->begin(); it != current->end(); ++it) {
                if (forbidden.contains(it.key())) {
                    return true;
                }
                pending.push_back(&it.value());
            }
        } else if (current->is_array()) {
            for (const auto& entry : *current) {
                pending.push_back(&entry);
            }
        }
    }
    return false;
}

daemon::StructuredOutcomeError outcome_error(const json& value) {
    std::string error;
    auto parsed = daemon::parse_structured_outcome_error(value, error);
    INFO(error);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

} // namespace

TEST_CASE("destructive authority follows dry-run and request precedence exhaustively",
          "[destructive][authority]") {
    for (const auto authority : {proto::WriteAuthority::Unset, proto::WriteAuthority::Grant,
                                 proto::WriteAuthority::Deny}) {
        for (const bool config_valid : {false, true}) {
            for (const bool config_grant : {false, true}) {
                proto::RequestContext request;
                request.dry_run = true;
                request.write_authority = authority;
                CHECK(std::holds_alternative<daemon::DryRunAuthority>(
                    daemon::evaluate_destructive_authority(
                        request, {.grant_valid = config_valid, .allow_write = config_grant})));
            }
        }
    }

    for (const bool config_valid : {false, true}) {
        for (const bool config_grant : {false, true}) {
            proto::RequestContext request;
            request.write_authority = proto::WriteAuthority::Deny;
            CHECK(daemon::evaluate_destructive_authority(
                      request, {.grant_valid = config_valid, .allow_write = config_grant}) ==
                  daemon::AuthorityDecision{
                      daemon::DeniedAuthority{daemon::WriteDenialReason::ExplicitDeny}});

            request.write_authority = proto::WriteAuthority::Grant;
            CHECK(daemon::evaluate_destructive_authority(
                      request, {.grant_valid = config_valid, .allow_write = config_grant}) ==
                  daemon::AuthorityDecision{
                      daemon::GrantedAuthority{daemon::AuthoritySource::Request}});
        }
    }

    proto::RequestContext request;
    request.write_authority = proto::WriteAuthority::Unset;
    CHECK(daemon::evaluate_destructive_authority(request,
                                                 {.grant_valid = false, .allow_write = false}) ==
          daemon::AuthorityDecision{
              daemon::DeniedAuthority{daemon::WriteDenialReason::InvalidConfigGrant}});
    CHECK(daemon::evaluate_destructive_authority(request,
                                                 {.grant_valid = false, .allow_write = true}) ==
          daemon::AuthorityDecision{
              daemon::DeniedAuthority{daemon::WriteDenialReason::InvalidConfigGrant}});
    CHECK(daemon::evaluate_destructive_authority(request,
                                                 {.grant_valid = true, .allow_write = false}) ==
          daemon::AuthorityDecision{daemon::DeniedAuthority{daemon::WriteDenialReason::NoGrant}});
    CHECK(daemon::evaluate_destructive_authority(request,
                                                 {.grant_valid = true, .allow_write = true}) ==
          daemon::AuthorityDecision{daemon::GrantedAuthority{daemon::AuthoritySource::Config}});

    request.write_authority = static_cast<proto::WriteAuthority>(99);
    CHECK(daemon::evaluate_destructive_authority(request,
                                                 {.grant_valid = true, .allow_write = true}) ==
          daemon::AuthorityDecision{daemon::DeniedAuthority{daemon::WriteDenialReason::NoGrant}});
    request.dry_run = true;
    CHECK(std::holds_alternative<daemon::DryRunAuthority>(daemon::evaluate_destructive_authority(
        request, {.grant_valid = true, .allow_write = true})));
}

TEST_CASE("audit stage validator accepts every legal command prefix", "[destructive][audit]") {
    const std::vector logout{daemon::AuditStage::IntentSynced,
                             daemon::AuditStage::LogoutSendStarted,
                             daemon::AuditStage::LogoutClosedConfirmed};
    for (std::size_t length = 1; length <= logout.size(); ++length) {
        std::string error;
        CHECK(daemon::validate_audit_stage_prefix(
            daemon::DestructiveCommand::Logout,
            std::vector(logout.begin(), logout.begin() + static_cast<std::ptrdiff_t>(length)),
            error));
    }

    const std::array branches{
        removal_branch(daemon::AuditStage::RemoteConfirmed, true),
        removal_branch(daemon::AuditStage::RemoteNotPresent, true),
        removal_branch(daemon::AuditStage::RemoteNotPresent),
        removal_branch(daemon::AuditStage::RemoteKept),
    };
    for (const auto& branch : branches) {
        for (std::size_t length = 1; length <= branch.size(); ++length) {
            std::string error;
            INFO("branch length " << length);
            CHECK(daemon::validate_audit_stage_prefix(
                daemon::DestructiveCommand::AccountRemove,
                std::vector(branch.begin(), branch.begin() + static_cast<std::ptrdiff_t>(length)),
                error));
        }
    }
}

TEST_CASE("audit stage validator rejects duplicates skips reorder and terminal leakage",
          "[destructive][audit]") {
    const std::vector<std::vector<daemon::AuditStage>> invalid_logout{
        {},
        {daemon::AuditStage::IntentSynced, daemon::AuditStage::IntentSynced},
        {daemon::AuditStage::IntentSynced, daemon::AuditStage::LogoutClosedConfirmed},
        {daemon::AuditStage::LogoutSendStarted},
        {daemon::AuditStage::IntentSynced, daemon::AuditStage::RemoteKept},
        {daemon::AuditStage::IntentSynced, daemon::AuditStage::LogoutSendStarted,
         daemon::AuditStage::LogoutClosedConfirmed, daemon::AuditStage::OutcomeSynced},
    };
    for (const auto& stages : invalid_logout) {
        std::string error;
        CHECK_FALSE(
            daemon::validate_audit_stage_prefix(daemon::DestructiveCommand::Logout, stages, error));
    }

    const std::vector<std::vector<daemon::AuditStage>> invalid_removal{
        {},
        {daemon::AuditStage::IntentSynced, daemon::AuditStage::Planned},
        {daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
         daemon::AuditStage::RemoteConfirmed},
        {daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
         daemon::AuditStage::RemoteKept, daemon::AuditStage::RemoteKept},
        {daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
         daemon::AuditStage::LogoutSendStarted},
        {daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
         daemon::AuditStage::RemoteKept, daemon::AuditStage::ClientClosed},
        removal_branch(daemon::AuditStage::RemoteKept),
    };
    for (std::size_t index = 0; index < invalid_removal.size(); ++index) {
        auto stages = invalid_removal[index];
        if (index + 1 == invalid_removal.size()) {
            stages.push_back(daemon::AuditStage::OutcomeSynced);
        }
        std::string error;
        CHECK_FALSE(daemon::validate_audit_stage_prefix(daemon::DestructiveCommand::AccountRemove,
                                                        stages, error));
    }

    std::string error;
    CHECK_FALSE(daemon::validate_audit_stage_prefix(
        static_cast<daemon::DestructiveCommand>(99),
        {daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced}, error));
    CHECK_FALSE(daemon::derive_mutation_state(
                    static_cast<daemon::DestructiveCommand>(99),
                    {daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced}, error)
                    .has_value());
    CHECK_FALSE(daemon::validate_audit_stage_prefix(
        daemon::DestructiveCommand::Logout,
        {daemon::AuditStage::IntentSynced, static_cast<daemon::AuditStage>(99)}, error));
}

TEST_CASE("mutation state is derived only from durable stage pairs", "[destructive][audit]") {
    using daemon::AuditStage;
    using daemon::DestructiveCommand;
    using daemon::MutationState;
    const std::vector cases{
        std::pair{std::vector{AuditStage::IntentSynced}, MutationState::None},
        std::pair{std::vector{AuditStage::IntentSynced, AuditStage::LogoutSendStarted},
                  MutationState::Possible},
        std::pair{std::vector{AuditStage::IntentSynced, AuditStage::LogoutSendStarted,
                              AuditStage::LogoutClosedConfirmed},
                  MutationState::Confirmed},
    };
    for (const auto& [stages, expected] : cases) {
        std::string error;
        CHECK(daemon::derive_mutation_state(DestructiveCommand::Logout, stages, error) == expected);
    }

    std::string error;
    CHECK(daemon::derive_mutation_state(
              DestructiveCommand::AccountRemove,
              {AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteKept},
              error) == MutationState::None);
    CHECK(daemon::derive_mutation_state(
              DestructiveCommand::AccountRemove,
              {AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteLogoutSendStarted},
              error) == MutationState::Possible);
    CHECK(daemon::derive_mutation_state(DestructiveCommand::AccountRemove,
                                        {AuditStage::Planned, AuditStage::IntentSynced,
                                         AuditStage::RemoteLogoutSendStarted,
                                         AuditStage::RemoteConfirmed},
                                        error) == MutationState::Confirmed);
    CHECK(
        daemon::derive_mutation_state(
            DestructiveCommand::AccountRemove,
            {AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteLogoutSendStarted,
             AuditStage::RemoteNotPresent, AuditStage::ClientCloseStarted, AuditStage::ClientClosed,
             AuditStage::ConfigRemoveStarted, AuditStage::ConfigRemoved},
            error) == MutationState::Possible);
}

TEST_CASE("structured outcome errors accept only the exact closed M1 table",
          "[destructive][audit]") {
    const auto plan = logout_plan();
    const std::vector<json> valid{
        {{"code", "USAGE"}, {"details", json::object()}},
        {{"code", "USAGE"}, {"details", {{"argument", nullptr}, {"reason", "invalid_argument"}}}},
        {{"code", "INSECURE_SECRET_INPUT"},
         {"details", {{"argument", "--bot-token"}, {"replacement", "--bot"}}}},
        {{"code", "ACCOUNT_EXISTS"}, {"details", {{"account", "main"}}}},
        {{"code", "DEFAULT_REASSIGNMENT_REQUIRED"},
         {"details", {{"account", "main"}, {"candidates", json::array({"a", "b"})}}}},
        {{"code", "ACCOUNT_NOT_FOUND"}, {"details", {{"account", "main"}}}},
        {{"code", "CONFIG_INVALID"}, {"details", {{"path", "/config"}, {"reason", "parse_error"}}}},
        {{"code", "CONFIG_CONFLICT"},
         {"details", {{"path", "/config"}, {"expected", kSnapshot}, {"current", "missing"}}}},
        {{"code", "HOOK_FAILED"},
         {"details", {{"hook", "password_cmd"}, {"reason", "exit"}, {"status", 1}}}},
        {{"code", "AUTH_FLOW_IN_PROGRESS"}, {"details", {{"account", "main"}, {"state", "ready"}}}},
        {{"code", "NOT_AUTHED"},
         {"details", {{"account", "main"}, {"state", "closed"}, {"reason", "not_ready"}}}},
        {{"code", "AUTH_INPUT_REQUIRED"},
         {"details", {{"account", "main"}, {"state", "wait_password"}, {"challenge", "password"}}}},
        {{"code", "AUTH_CANCELLED"},
         {"details",
          {{"account", "main"}, {"state", "wait_code"}, {"challenge", "authentication_code"}}}},
        {{"code", "AUTH_CREDENTIAL_REJECTED"},
         {"details",
          {{"account", "main"},
           {"state", "wait_code"},
           {"credential", "authentication_code"},
           {"tdlib_code", 400}}}},
        {{"code", "AUTH_PREMIUM_REQUIRED"},
         {"details",
          {{"account", "main"},
           {"state", "wait_premium_purchase"},
           {"store_product_id", "product"},
           {"premium_day_count", 30},
           {"support_email_address", "support@example.test"},
           {"support_email_subject", "subject"}}}},
        {{"code", "UNSUPPORTED_AUTH_STATE"},
         {"details", {{"account", "main"}, {"tdlib_type_id", 123}}}},
        {{"code", "AUTH_FUNCTION_DENIED"},
         {"details", {{"account", "main"}, {"state", "ready"}, {"function", "logOut"}}}},
        {{"code", "PROTOCOL_ANSWER_INVALID"},
         {"details", {{"request_id", std::uint64_t{7}}, {"reason", "malformed"}}}},
        {{"code", "WRITE_DENIED"},
         {"details", {{"account", "main"}, {"reason", "invalid_config_grant"}}}},
        {{"code", "CONFIRMATION_REQUIRED"},
         {"details",
          {{"account", "main"}, {"action", "logout"}, {"target", proto::serialize(plan)}}}},
        {{"code", "AUDIT_UNAVAILABLE"},
         {"details", {{"account", "main"}, {"path", "/audit"}, {"reason", "sync_failed"}}}},
        {{"code", "AUDIT_INCOMPLETE"},
         {"details",
          {{"account", "main"},
           {"path", "/audit"},
           {"mutation_state", "none"},
           {"completed_stages", json::array({"intent_synced"})}}}},
        {{"code", "REMOTE_LOGOUT_UNCONFIRMED"},
         {"details", {{"account", "main"}, {"state", "closing"}, {"reason", "timeout"}}}},
        {{"code", "LOCAL_CLEANUP_FAILED"},
         {"details",
          {{"account", "main"},
           {"reason", "path_changed"},
           {"removed", json::array({"/data"})},
           {"retained", json::array({"/state"})}}}},
        {{"code", "REMOVAL_INCOMPLETE"},
         {"details",
          {{"account", "main"},
           {"path", "/tombstone"},
           {"invocation_id", "00112233445566778899aabbccddeeff"},
           {"stage", "intent_synced"},
           {"completed_stages", json::array({"planned", "intent_synced"})},
           {"reason", "prior_crash"}}}},
        {{"code", "DAEMON_NOT_RUNNING"}, {"details", {{"account", "main"}, {"socket", "/socket"}}}},
        {{"code", "DAEMON_CONTROL_FAILED"},
         {"details", {{"account", "main"}, {"operation", "stop"}, {"reason", "shutdown_failed"}}}},
        {{"code", "RATE_LIMITED"},
         {"details", {{"operation", "logout"}, {"tdlib_code", 429}, {"retry_after", 1}}}},
        {{"code", "TDLIB_ERROR"}, {"details", {{"operation", "logout"}, {"tdlib_code", 500}}}},
        {{"code", "TIMEOUT"}, {"details", {{"operation", "logout"}, {"state", nullptr}}}},
        {{"code", "DAEMON_SHUTDOWN"}, {"details", {{"reason", "daemon_shutdown"}}}},
        {{"code", "INTERNAL"},
         {"details", {{"operation", "logout"}, {"reason", "internal_error"}}}},
    };

    for (const auto& value : valid) {
        std::string error;
        INFO(value.dump());
        auto parsed = daemon::parse_structured_outcome_error(value, error);
        INFO(error);
        REQUIRE(parsed.has_value());
        CHECK(daemon::serialize(*parsed) == value);
    }

    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value["extra"] = true; },
             [](json& value) { value["code"] = "FUTURE_ERROR"; },
             [](json& value) { value["details"]["password"] = "sentinel-secret"; },
             [](json& value) { value["details"]["reason"] = 1; },
             [](json& value) { value["details"].erase("operation"); },
         }) {
        json invalid{{"code", "INTERNAL"},
                     {"details", {{"operation", "logout"}, {"reason", "internal_error"}}}};
        mutate(invalid);
        std::string error;
        CHECK_FALSE(daemon::parse_structured_outcome_error(invalid, error).has_value());
    }

    const json inconsistent_audit_error{{"code", "AUDIT_INCOMPLETE"},
                                        {"details",
                                         {{"account", "main"},
                                          {"path", "/audit"},
                                          {"mutation_state", "confirmed"},
                                          {"completed_stages", json::array({"intent_synced"})}}}};
    std::string error;
    CHECK_FALSE(
        daemon::parse_structured_outcome_error(inconsistent_audit_error, error).has_value());
}

TEST_CASE("audit factories derive exact records from typed inputs", "[destructive][audit]") {
    const auto logout = logout_plan();
    const auto removal = removal_plan();
    std::string error;

    auto logout_intent = daemon::make_logout_audit_intent(identity(), logout, "missing",
                                                          daemon::AuthoritySource::Request,
                                                          daemon::ConfirmationSource::Yes, error);
    INFO(error);
    REQUIRE(logout_intent.has_value());
    const auto logout_intent_json = daemon::serialize(*logout_intent);
    CHECK(keys(logout_intent_json) ==
          std::set<std::string>{"account", "arguments", "authority_source", "command",
                                "config_snapshot", "confirmation_source", "invocation_id", "phase",
                                "plan", "schema_version", "timestamp"});
    CHECK(logout_intent_json["arguments"] == json::object());
    CHECK(logout_intent_json["plan"] == proto::serialize(logout));
    CHECK(logout_intent_json["authority_source"] == "request");
    CHECK(logout_intent_json["confirmation_source"] == "yes");
    CHECK(logout_intent_json["schema_version"].is_number_integer());
    CHECK(logout_intent_json["phase"].is_string());
    CHECK(logout_intent_json["invocation_id"].is_string());
    CHECK(logout_intent_json["timestamp"].is_string());
    CHECK(logout_intent_json["account"].is_string());
    CHECK(logout_intent_json["command"].is_string());
    CHECK(logout_intent_json["config_snapshot"].is_string());
    CHECK(logout_intent_json["arguments"].is_object());
    CHECK(logout_intent_json["plan"].is_object());

    auto removal_intent = daemon::make_account_remove_audit_intent(
        identity(), removal, daemon::AuthoritySource::Config, daemon::ConfirmationSource::Tty,
        error);
    INFO(error);
    REQUIRE(removal_intent.has_value());
    const auto removal_intent_json = daemon::serialize(*removal_intent);
    CHECK(removal_intent_json["arguments"] ==
          json{{"keep_session", false}, {"reassign_default", "main"}});
    CHECK(removal_intent_json["config_snapshot"] == kSnapshot);
    CHECK(removal_intent_json["plan"] == proto::serialize(removal));

    auto checkpoint = daemon::make_logout_audit_checkpoint(
        identity(), logout, daemon::AuditStage::LogoutSendStarted, error);
    INFO(error);
    REQUIRE(checkpoint.has_value());
    const auto checkpoint_json = daemon::serialize(*checkpoint);
    CHECK(keys(checkpoint_json) == std::set<std::string>{"account", "command", "invocation_id",
                                                         "phase", "schema_version", "stage",
                                                         "timestamp"});
    CHECK(checkpoint_json["stage"] == "logout_send_started");
    CHECK(checkpoint_json["schema_version"].is_number_integer());
    CHECK(checkpoint_json["phase"].is_string());
    CHECK(checkpoint_json["invocation_id"].is_string());
    CHECK(checkpoint_json["timestamp"].is_string());
    CHECK(checkpoint_json["account"].is_string());
    CHECK(checkpoint_json["command"].is_string());

    auto logout_outcome = daemon::make_logout_success_audit_outcome(
        identity(), logout,
        {daemon::AuditStage::IntentSynced, daemon::AuditStage::LogoutSendStarted,
         daemon::AuditStage::LogoutClosedConfirmed},
        error);
    INFO(error);
    REQUIRE(logout_outcome.has_value());
    const auto logout_outcome_json = daemon::serialize(*logout_outcome);
    CHECK(keys(logout_outcome_json) ==
          std::set<std::string>{"account", "command", "completed_stages", "error", "invocation_id",
                                "mutation_state", "phase", "result", "schema_version", "success",
                                "timestamp"});
    CHECK(logout_outcome_json["success"] == true);
    CHECK(logout_outcome_json["mutation_state"] == "confirmed");
    CHECK(logout_outcome_json["result"] == json{{"account", "main"}, {"logged_out", true}});
    CHECK(logout_outcome_json["error"].is_null());
    CHECK(logout_outcome_json["schema_version"].is_number_integer());
    CHECK(logout_outcome_json["phase"].is_string());
    CHECK(logout_outcome_json["invocation_id"].is_string());
    CHECK(logout_outcome_json["timestamp"].is_string());
    CHECK(logout_outcome_json["account"].is_string());
    CHECK(logout_outcome_json["command"].is_string());
    CHECK(logout_outcome_json["mutation_state"].is_string());
    CHECK(logout_outcome_json["success"].is_boolean());
    CHECK(logout_outcome_json["completed_stages"].is_array());
    CHECK(logout_outcome_json["result"].is_object());

    auto structured_error = daemon::parse_structured_outcome_error(
        json{{"code", "INTERNAL"},
             {"details", {{"operation", "logout"}, {"reason", "internal_error"}}}},
        error);
    REQUIRE(structured_error.has_value());
    auto failure = daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{logout},
                                                      {daemon::AuditStage::IntentSynced},
                                                      *structured_error, error);
    INFO(error);
    REQUIRE(failure.has_value());
    const auto failure_json = daemon::serialize(*failure);
    CHECK(failure_json["success"] == false);
    CHECK(failure_json["mutation_state"] == "none");
    CHECK(failure_json["result"].is_null());
    CHECK(failure_json["error"] == daemon::serialize(*structured_error));

    for (const auto& document : {logout_intent_json, removal_intent_json, checkpoint_json,
                                 logout_outcome_json, failure_json}) {
        CHECK_FALSE(contains_forbidden_secret_key(document));
    }
}

TEST_CASE("failure outcomes accept only the closed post-intent command table",
          "[destructive][audit]") {
    const auto logout = logout_plan();
    const auto removal = removal_plan();
    const std::array logout_errors{
        json{{"code", "AUTH_FUNCTION_DENIED"},
             {"details", {{"account", "main"}, {"state", "ready"}, {"function", "logOut"}}}},
        json{{"code", "REMOTE_LOGOUT_UNCONFIRMED"},
             {"details", {{"account", "main"}, {"state", "closing"}, {"reason", "timeout"}}}},
        json{{"code", "RATE_LIMITED"},
             {"details", {{"operation", "logout"}, {"tdlib_code", 429}, {"retry_after", 1}}}},
        json{{"code", "TDLIB_ERROR"}, {"details", {{"operation", "logout"}, {"tdlib_code", 500}}}},
        json{{"code", "TIMEOUT"}, {"details", {{"operation", "logout"}, {"state", nullptr}}}},
        json{{"code", "DAEMON_SHUTDOWN"}, {"details", {{"reason", "daemon_shutdown"}}}},
        json{{"code", "INTERNAL"},
             {"details", {{"operation", "logout"}, {"reason", "internal_error"}}}},
    };
    for (const auto& value : logout_errors) {
        auto structured = outcome_error(value);
        std::string error;
        INFO(value.dump());
        CHECK(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{logout},
                                                 {daemon::AuditStage::IntentSynced}, structured,
                                                 error)
                  .has_value());
        INFO(error);
    }

    const std::array removal_errors{
        json{{"code", "AUTH_FUNCTION_DENIED"},
             {"details", {{"account", "work"}, {"state", "ready"}, {"function", "close"}}}},
        json{{"code", "CONFIG_INVALID"},
             {"details", {{"path", "/config/tgcli/config.toml"}, {"reason", "parse_error"}}}},
        json{{"code", "CONFIG_CONFLICT"},
             {"details",
              {{"path", "/config/tgcli/config.toml"},
               {"expected", kSnapshot},
               {"current", "missing"}}}},
        json{{"code", "REMOTE_LOGOUT_UNCONFIRMED"},
             {"details", {{"account", "work"}, {"state", "closing"}, {"reason", "timeout"}}}},
        json{{"code", "LOCAL_CLEANUP_FAILED"},
             {"details",
              {{"account", "work"},
               {"reason", "io_error"},
               {"removed", json::array({"/data/accounts/work"})},
               {"retained", json::array({"/state/accounts/work"})}}}},
        json{{"code", "DAEMON_CONTROL_FAILED"},
             {"details",
              {{"account", "work"}, {"operation", "stop"}, {"reason", "shutdown_failed"}}}},
        json{{"code", "RATE_LIMITED"},
             {"details",
              {{"operation", "account_remove"}, {"tdlib_code", 429}, {"retry_after", 1}}}},
        json{{"code", "TDLIB_ERROR"},
             {"details", {{"operation", "account_remove"}, {"tdlib_code", 500}}}},
        json{{"code", "TIMEOUT"},
             {"details", {{"operation", "account_remove"}, {"state", nullptr}}}},
        json{{"code", "DAEMON_SHUTDOWN"}, {"details", {{"reason", "daemon_shutdown"}}}},
        json{{"code", "INTERNAL"},
             {"details", {{"operation", "account_remove"}, {"reason", "internal_error"}}}},
    };
    for (const auto& value : removal_errors) {
        auto structured = outcome_error(value);
        std::string error;
        INFO(value.dump());
        CHECK(daemon::make_failure_audit_outcome(
                  identity(), proto::DestructivePlan{removal},
                  {daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced}, structured,
                  error)
                  .has_value());
        INFO(error);
    }
}

TEST_CASE("failure outcomes bind phase account operation target and removal paths",
          "[destructive][audit]") {
    const auto logout = logout_plan();
    const auto removal = removal_plan();
    const auto keep_removal = removal_plan(true);
    const std::vector logout_stages{daemon::AuditStage::IntentSynced};
    const std::vector removal_stages{daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced};
    std::string error;

    auto internal_removal = outcome_error(
        json{{"code", "INTERNAL"},
             {"details", {{"operation", "account_remove"}, {"reason", "internal_error"}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{removal},
                                                   {daemon::AuditStage::Planned}, internal_removal,
                                                   error)
                    .has_value());

    const std::array pre_intent_errors{
        json{{"code", "WRITE_DENIED"}, {"details", {{"account", "work"}, {"reason", "no_grant"}}}},
        json{{"code", "CONFIRMATION_REQUIRED"},
             {"details",
              {{"account", "work"},
               {"action", "account_remove"},
               {"target", proto::serialize(removal)}}}},
        json{{"code", "AUDIT_UNAVAILABLE"},
             {"details", {{"account", "work"}, {"path", "/audit"}, {"reason", "sync_failed"}}}},
    };
    for (const auto& value : pre_intent_errors) {
        auto structured = outcome_error(value);
        INFO(value.dump());
        CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{removal},
                                                       removal_stages, structured, error)
                        .has_value());
    }

    auto wrong_account = outcome_error(
        json{{"code", "REMOTE_LOGOUT_UNCONFIRMED"},
             {"details", {{"account", "work"}, {"state", "closing"}, {"reason", "timeout"}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{logout},
                                                   logout_stages, wrong_account, error)
                    .has_value());

    auto wrong_operation = outcome_error(
        json{{"code", "INTERNAL"},
             {"details", {{"operation", "account_remove"}, {"reason", "internal_error"}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{logout},
                                                   logout_stages, wrong_operation, error)
                    .has_value());

    std::string plan_error;
    auto other_logout = proto::make_logout_plan("other", plan_error);
    REQUIRE(other_logout.has_value());
    auto wrong_target = outcome_error(json{{"code", "CONFIRMATION_REQUIRED"},
                                           {"details",
                                            {{"account", "other"},
                                             {"action", "logout"},
                                             {"target", proto::serialize(*other_logout)}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{logout},
                                                   logout_stages, wrong_target, error)
                    .has_value());

    auto wrong_config_path = outcome_error(
        json{{"code", "CONFIG_INVALID"},
             {"details", {{"path", "/config/other.toml"}, {"reason", "parse_error"}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{removal},
                                                   removal_stages, wrong_config_path, error)
                    .has_value());

    auto wrong_expected_snapshot = outcome_error(json{{"code", "CONFIG_CONFLICT"},
                                                      {"details",
                                                       {{"path", "/config/tgcli/config.toml"},
                                                        {"expected", kOtherSnapshot},
                                                        {"current", "missing"}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{removal},
                                                   removal_stages, wrong_expected_snapshot, error)
                    .has_value());

    auto unchanged_snapshot = outcome_error(json{{"code", "CONFIG_CONFLICT"},
                                                 {"details",
                                                  {{"path", "/config/tgcli/config.toml"},
                                                   {"expected", kSnapshot},
                                                   {"current", kSnapshot}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{removal},
                                                   removal_stages, unchanged_snapshot, error)
                    .has_value());

    auto wrong_cleanup_path =
        outcome_error(json{{"code", "LOCAL_CLEANUP_FAILED"},
                           {"details",
                            {{"account", "work"},
                             {"reason", "path_changed"},
                             {"removed", json::array({"/data/accounts/work"})},
                             {"retained", json::array({"/state/accounts/other"})}}}});
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{removal},
                                                   removal_stages, wrong_cleanup_path, error)
                    .has_value());

    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{removal},
                                                   {daemon::AuditStage::Planned,
                                                    daemon::AuditStage::IntentSynced,
                                                    daemon::AuditStage::RemoteKept},
                                                   internal_removal, error)
                    .has_value());
    CHECK_FALSE(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{keep_removal},
                                                   {daemon::AuditStage::Planned,
                                                    daemon::AuditStage::IntentSynced,
                                                    daemon::AuditStage::RemoteNotPresent},
                                                   internal_removal, error)
                    .has_value());
}

TEST_CASE("failure outcomes preserve every plan-compatible removal remote branch",
          "[destructive][audit]") {
    const auto removal = removal_plan();
    const auto keep_removal = removal_plan(true);
    auto structured = outcome_error(
        json{{"code", "INTERNAL"},
             {"details", {{"operation", "account_remove"}, {"reason", "internal_error"}}}});
    const std::array cases{
        std::pair{removal,
                  std::vector{daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
                              daemon::AuditStage::RemoteLogoutSendStarted,
                              daemon::AuditStage::RemoteConfirmed}},
        std::pair{removal,
                  std::vector{daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
                              daemon::AuditStage::RemoteLogoutSendStarted,
                              daemon::AuditStage::RemoteNotPresent}},
        std::pair{removal,
                  std::vector{daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
                              daemon::AuditStage::RemoteNotPresent}},
        std::pair{keep_removal,
                  std::vector{daemon::AuditStage::Planned, daemon::AuditStage::IntentSynced,
                              daemon::AuditStage::RemoteKept}},
    };
    for (const auto& [plan, stages] : cases) {
        std::string error;
        CHECK(daemon::make_failure_audit_outcome(identity(), proto::DestructivePlan{plan}, stages,
                                                 structured, error)
                  .has_value());
        INFO(error);
    }
}

TEST_CASE("successful removal outcomes require and preserve each exact remote branch",
          "[destructive][audit]") {
    const auto remote_plan = removal_plan();
    const auto keep_plan = removal_plan(true);
    const std::array cases{
        std::tuple{remote_plan, daemon::AccountRemoveRemoteResult::Confirmed,
                   removal_branch(daemon::AuditStage::RemoteConfirmed, true), "confirmed",
                   "confirmed"},
        std::tuple{remote_plan, daemon::AccountRemoveRemoteResult::NotPresent,
                   removal_branch(daemon::AuditStage::RemoteNotPresent, true), "not_present",
                   "possible"},
        std::tuple{remote_plan, daemon::AccountRemoveRemoteResult::NotPresent,
                   removal_branch(daemon::AuditStage::RemoteNotPresent), "not_present",
                   "confirmed"},
        std::tuple{keep_plan, daemon::AccountRemoveRemoteResult::Kept,
                   removal_branch(daemon::AuditStage::RemoteKept), "kept", "confirmed"},
    };
    for (const auto& [plan, remote_result, stages, expected_remote, expected_mutation] : cases) {
        std::string error;
        auto outcome = daemon::make_account_remove_success_audit_outcome(
            identity(), plan, remote_result, stages, error);
        INFO(error);
        REQUIRE(outcome.has_value());
        const auto document = daemon::serialize(*outcome);
        CHECK(document["result"]["remote_logout"] == expected_remote);
        CHECK(document["result"]["default_account"] == "main");
        CHECK(document["mutation_state"] == expected_mutation);
    }
}

TEST_CASE("audit factories reject invalid identity stage and result combinations",
          "[destructive][audit]") {
    const auto logout = logout_plan();
    const auto removal = removal_plan();
    std::string error;

    auto bad_identity = identity();
    bad_identity.invocation_id = "UPPERCASE";
    CHECK_FALSE(daemon::make_logout_audit_intent(bad_identity, logout, kSnapshot,
                                                 daemon::AuthoritySource::Request,
                                                 daemon::ConfirmationSource::Yes, error)
                    .has_value());

    bad_identity = identity();
    bad_identity.timestamp = "2026-02-30T12:00:00Z";
    CHECK_FALSE(daemon::make_logout_audit_intent(bad_identity, logout, kSnapshot,
                                                 daemon::AuthoritySource::Request,
                                                 daemon::ConfirmationSource::Yes, error)
                    .has_value());

    CHECK_FALSE(daemon::make_logout_audit_intent(identity(), logout, kSnapshot,
                                                 static_cast<daemon::AuthoritySource>(99),
                                                 daemon::ConfirmationSource::Yes, error)
                    .has_value());
    CHECK_FALSE(daemon::make_logout_audit_intent(identity(), logout, kSnapshot,
                                                 daemon::AuthoritySource::Request,
                                                 static_cast<daemon::ConfirmationSource>(99), error)
                    .has_value());

    CHECK_FALSE(daemon::make_logout_audit_checkpoint(identity(), logout,
                                                     daemon::AuditStage::RemoteKept, error)
                    .has_value());
    CHECK_FALSE(daemon::make_logout_audit_checkpoint(identity(), logout,
                                                     static_cast<daemon::AuditStage>(99), error)
                    .has_value());
    CHECK_FALSE(daemon::make_logout_success_audit_outcome(
                    identity(), logout,
                    {daemon::AuditStage::IntentSynced, daemon::AuditStage::LogoutSendStarted},
                    error)
                    .has_value());
    CHECK_FALSE(daemon::make_account_remove_success_audit_outcome(
                    identity(), removal, daemon::AccountRemoveRemoteResult::Kept,
                    removal_branch(daemon::AuditStage::RemoteKept), error)
                    .has_value());
    CHECK_FALSE(daemon::make_account_remove_success_audit_outcome(
                    identity(), removal, static_cast<daemon::AccountRemoveRemoteResult>(99),
                    removal_branch(daemon::AuditStage::RemoteConfirmed, true), error)
                    .has_value());
}
