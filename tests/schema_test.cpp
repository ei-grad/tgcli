#include "schema_matcher.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

using nlohmann::json;

namespace {

constexpr std::string_view kDialect = "https://json-schema.org/draft/2020-12/schema";

void check_schema_node(const json& schema) {
    std::vector<const json*> pending{&schema};
    while (!pending.empty()) {
        const auto* node = pending.back();
        pending.pop_back();

        if (node->is_array()) {
            for (const auto& child : *node) {
                pending.push_back(&child);
            }
            continue;
        }
        if (!node->is_object()) {
            continue;
        }

        CHECK_FALSE(node->contains("$id"));
        CHECK_FALSE(node->contains("format"));
        if (const auto reference = node->find("$ref"); reference != node->end()) {
            REQUIRE(reference->is_string());
            CHECK(reference->get_ref<const std::string&>().starts_with('#'));
        }
        if ((node->contains("type") && (*node)["type"] == "object") ||
            node->contains("properties")) {
            REQUIRE(node->contains("additionalProperties"));
            CHECK((*node)["additionalProperties"] == false);
        }
        for (const auto& [name, child] : node->items()) {
            static_cast<void>(name);
            pending.push_back(&child);
        }
    }
}

struct SchemaCase {
    std::string filename;
    json instance;
    std::string required_property;
    bool has_nested_object = false;
};

std::vector<SchemaCase> schema_cases() {
    return {
        {"account-add.result.schema.json",
         {{"account", "work"}, {"created", true}, {"default", false}},
         "account"},
        {"account-list.result.schema.json",
         {{"items", json::array({json{{"name", "main"}, {"default", true}}})}, {"next", nullptr}},
         "items",
         true},
        {"account-remove.result.schema.json",
         {{"account", "work"},
          {"removed", true},
          {"remote_logout", "confirmed"},
          {"default_account", "main"}},
         "account"},
        {"account-show.result.schema.json",
         {{"account", "main"},
          {"default", true},
          {"allow_write", false},
          {"idle_exit", nullptr},
          {"credentials",
           {{"api_id", "value"},
            {"api_hash", "value"},
            {"db_key", "none"},
            {"password", "interactive"},
            {"bot_token", "interactive"}}},
          {"paths",
           {{"data", "/tmp/data"}, {"state", "/tmp/state"}, {"socket", "/tmp/main.sock"}}}},
         "account",
         true},
        {"account-use.result.schema.json",
         {{"default_account", "work"}, {"previous_default", "main"}},
         "default_account"},
        {"version.result.schema.json",
         {{"version", "0.1.0"}, {"protocol", 2}, {"tdlib", "1.8.65"}},
         "version"},
        {"daemon-stop.result.schema.json", {{"stopping", true}}, "stopping"},
        {"daemon-status.result.schema.json",
         {{"account", "main"}, {"running", false}, {"socket", "/tmp/tgcli.sock"}},
         "account"},
        {"daemon-status.result.schema.json",
         {{"account", "main"},
          {"running", true},
          {"pid", 123},
          {"version", "0.1.0"},
          {"protocol", 1},
          {"socket", "/tmp/tgcli.sock"}},
         "account"},
        {"daemon-restart.result.schema.json",
         {{"account", "main"},
          {"restarted", true},
          {"pid", 124},
          {"version", "0.1.0"},
          {"protocol", 1},
          {"socket", "/tmp/tgcli.sock"}},
         "account"},
        {"login.result.schema.json",
         {{"account", "main"},
          {"auth_state", "ready"},
          {"user",
           {{"id", 123456},
            {"first_name", "Ada"},
            {"last_name", "Lovelace"},
            {"usernames", json::array({"ada"})},
            {"phone_number", "12025550123"},
            {"is_bot", false},
            {"is_premium", true}}}},
         "account",
         true},
        {"logout.result.schema.json", {{"account", "main"}, {"logged_out", true}}, "account"},
        {"logout.result.schema.json",
         {{"dry_run", true},
          {"plan",
           {{"operation", "logout"},
            {"account", "main"},
            {"remote_logout", true},
            {"tdlib_request", "logOut"}}}},
         "dry_run",
         true},
        {"me.result.schema.json",
         {{"id", 123456},
          {"first_name", "Ada"},
          {"last_name", "Lovelace"},
          {"usernames", json::array({"ada"})},
          {"phone_number", "12025550123"},
          {"is_bot", false},
          {"is_premium", true}},
         "id"},
        {"doctor.result.schema.json",
         {{"account", "main"},
          {"daemon",
           {{"running", true},
            {"in_process", false},
            {"pid", 123},
            {"version", "0.1.0"},
            {"socket", "/tmp/tgcli.sock"}}},
          {"tdlib", {{"version", "1.8.65"}}},
          {"auth", {{"state", "unknown"}}}},
         "account",
         true},
        {"doctor.result.schema.json",
         {{"account", "main"},
          {"daemon",
           {{"running", false}, {"in_process", true}, {"pid", 123}, {"version", "0.1.0"}}},
          {"tdlib", {{"version", "1.8.65"}}},
          {"auth", {{"state", "unknown"}}}},
         "account",
         true},
        {"doctor.result.schema.json",
         {{"account", "main"},
          {"daemon", {{"running", false}, {"socket", "/tmp/tgcli.sock"}}},
          {"config", {{"path", "/tmp/config.toml"}, {"exists", false}}}},
         "account",
         true},
        {"saved-tags.result.schema.json",
         {{"items", json::array({json{{"tag", "🧪"}, {"label", "experiments"}, {"count", 7}},
                                 json{{"tag", "custom:123456789"}, {"label", ""}, {"count", 2}}})},
          {"next", nullptr}},
         "items",
         true},
        {"saved-search.result.schema.json",
         {{"items", json::array({json{{"id", 200},
                                      {"chat_id", 42},
                                      {"date", "2026-07-02T12:00:00Z"},
                                      {"text", "experiment result"}},
                                 json{{"id", 199},
                                      {"chat_id", 42},
                                      {"date", "2026-07-02T11:59:00Z"},
                                      {"text", ""}}})},
          {"next", "tgcli.saved.v1.cursor"}},
         "items",
         true},
        {"resolve.result.schema.json",
         {{"kind", "message"},
          {"chat",
           {{"id", -1001},
            {"title", "Project"},
            {"type", "supergroup"},
            {"is_bot", false},
            {"usernames", json::array({"project"})}}},
          {"message_id", 123},
          {"topic", {{"kind", "forum"}, {"id", 7}}},
          {"link_type", "message"},
          {"is_public", true}},
         "kind",
         true},
    };
}

json removal_plan(bool keep_session = false) {
    return {{"operation", "account_remove"},
            {"account", "work"},
            {"remote_logout", !keep_session},
            {"keep_session", keep_session},
            {"delete_paths", json::array({"/data/work", "/state/work"})},
            {"config_path", "/config/tgcli/config.toml"},
            {"config_snapshot",
             "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
             "dev:1;ino:2;size:3;ctime_ns:4"},
            {"data_root",
             {{"path", "/data/work"},
              {"device", std::uint64_t{1}},
              {"inode", std::uint64_t{2}},
              {"owner", std::uint64_t{1000}}}},
            {"state_root", nullptr},
            {"reassign_default", "main"}};
}

json logout_plan() {
    return {{"operation", "logout"},
            {"account", "main"},
            {"remote_logout", true},
            {"tdlib_request", "logOut"}};
}

json audit_identity(std::string phase, std::string command, std::string account) {
    return {{"schema_version", 1},
            {"phase", std::move(phase)},
            {"invocation_id", "00112233445566778899aabbccddeeff"},
            {"timestamp", "2026-08-04T10:11:12Z"},
            {"account", std::move(account)},
            {"command", std::move(command)}};
}

json terminal_error(std::string code, json details) {
    return {{"error",
             {{"code", std::move(code)},
              {"message", "contract error"},
              {"details", std::move(details)}}}};
}

} // namespace

TEST_CASE("schema manifest is an exact command-to-result bijection", "[schema]") {
    const auto manifest = tgcli::test::load_schema_document("manifest.json");
    const json expected{
        {"schemaDialect", kDialect},
        {"commands",
         {{"account add", {{"result", "account-add.result.schema.json"}}},
          {"account list", {{"result", "account-list.result.schema.json"}}},
          {"account remove", {{"result", "account-remove.result.schema.json"}}},
          {"account show", {{"result", "account-show.result.schema.json"}}},
          {"account use", {{"result", "account-use.result.schema.json"}}},
          {"daemon restart", {{"result", "daemon-restart.result.schema.json"}}},
          {"daemon status", {{"result", "daemon-status.result.schema.json"}}},
          {"daemon stop", {{"result", "daemon-stop.result.schema.json"}}},
          {"doctor", {{"result", "doctor.result.schema.json"}}},
          {"login", {{"result", "login.result.schema.json"}}},
          {"logout", {{"result", "logout.result.schema.json"}}},
          {"me", {{"result", "me.result.schema.json"}}},
          {"resolve", {{"result", "resolve.result.schema.json"}}},
          {"saved search", {{"result", "saved-search.result.schema.json"}}},
          {"saved tags", {{"result", "saved-tags.result.schema.json"}}},
          {"session list", {{"result", "session-list.result.schema.json"}}},
          {"session terminate", {{"result", "session-terminate.result.schema.json"}}},
          {"version", {{"result", "version.result.schema.json"}}},
          {"wait-for", {{"result", "wait-for.result.schema.json"}}}}}};
    CHECK(manifest == expected);
    CHECK(manifest["commands"].size() == 19);

    std::set<std::string> manifested_files;
    for (const auto& [command, contract] : manifest["commands"].items()) {
        static_cast<void>(command);
        REQUIRE(contract.is_object());
        REQUIRE(contract.size() == 1);
        const auto result = contract.find("result");
        REQUIRE(result != contract.end());
        REQUIRE(result->is_string());
        manifested_files.insert(result->get<std::string>());
    }

    std::set<std::string> result_files;
    const auto schema_directory = tgcli::test::schema_path("manifest.json").parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(schema_directory)) {
        const auto filename = entry.path().filename().string();
        if (entry.is_regular_file() && filename.ends_with(".result.schema.json")) {
            result_files.insert(filename);
        }
    }
    CHECK(manifested_files == result_files);
}

TEST_CASE("result schemas use the strict local Draft 2020-12 subset", "[schema]") {
    const auto manifest = tgcli::test::load_schema_document("manifest.json");
    for (const auto& [command, contract] : manifest["commands"].items()) {
        static_cast<void>(command);
        const auto result = contract.find("result");
        REQUIRE(result != contract.end());
        const auto schema = tgcli::test::load_schema_document(result->get<std::string>());
        REQUIRE(schema.contains("$schema"));
        CHECK(schema["$schema"] == kDialect);
        check_schema_node(schema);
    }

    const auto doctor = tgcli::test::load_schema_document("doctor.result.schema.json");
    REQUIRE(doctor.contains("oneOf"));
    REQUIRE(doctor["oneOf"].is_array());
    CHECK(doctor["oneOf"].size() == 3);

    const auto account_error = tgcli::test::load_schema_document("account.error.schema.json");
    check_schema_node(account_error);
    const auto auth_error = tgcli::test::load_schema_document("auth.error.schema.json");
    check_schema_node(auth_error);
    const auto daemon_error = tgcli::test::load_schema_document("daemon.error.schema.json");
    check_schema_node(daemon_error);
    const auto saved_error = tgcli::test::load_schema_document("saved.error.schema.json");
    check_schema_node(saved_error);
    const auto session_error = tgcli::test::load_schema_document("session.error.schema.json");
    check_schema_node(session_error);
    const auto resolve_error = tgcli::test::load_schema_document("resolve.error.schema.json");
    check_schema_node(resolve_error);
    for (const auto* filename : {"logout.error.schema.json", "account-remove.error.schema.json",
                                 "audit-intent.schema.json", "audit-checkpoint.schema.json",
                                 "audit-outcome.schema.json", "removal-tombstone.schema.json"}) {
        const auto schema = tgcli::test::load_schema_document(filename);
        REQUIRE(schema.contains("$schema"));
        CHECK(schema["$schema"] == kDialect);
        check_schema_node(schema);
    }
}

TEST_CASE("resolver errors have exact command-specific shapes", "[schema][resolver][error]") {
    const std::vector<json> errors{
        terminal_error("USAGE", {{"argument", "selector"}, {"reason", "invalid_argument"}}),
        terminal_error(
            "NOT_AUTHED",
            {{"account", "main"}, {"state", "wait_code"}, {"reason", "authorization_lost"}}),
        terminal_error("BOT_UNSUPPORTED", {{"operation", "resolve"}}),
        terminal_error("NOT_FOUND", {{"selector", "@missing"}}),
        terminal_error("NOT_FOUND", {{"selector", "@missing"}, {"scope", "local_materialized"}}),
        terminal_error("AMBIGUOUS",
                       {{"selector", "Project"},
                        {"scope", "active_dialogs"},
                        {"candidates", json::array({json{{"id", -1},
                                                         {"title", "Project"},
                                                         {"type", "basic_group"},
                                                         {"is_bot", false},
                                                         {"usernames", json::array()}}})},
                        {"truncated", false}}),
        terminal_error("RATE_LIMITED",
                       {{"operation", "resolve"}, {"tdlib_code", 429}, {"retry_after", 5}}),
        terminal_error("TDLIB_ERROR", {{"operation", "resolve"}, {"tdlib_code", 400}}),
        terminal_error("TIMEOUT", {{"operation", "resolve"}, {"state", "ready"}}),
        terminal_error("INTERNAL", {{"operation", "resolve"}, {"reason", "internal_error"}}),
    };
    for (const auto& error : errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("resolve.error.schema.json"));
    }

    auto unknown = errors.front();
    unknown["error"]["details"]["extra"] = true;
    CHECK_THAT(unknown, !tgcli::test::matches_json_schema("resolve.error.schema.json"));
}

TEST_CASE("Saved Messages errors have exact command-specific shapes", "[schema][saved][error]") {
    const std::vector<json> errors{
        terminal_error("USAGE", {{"argument", "--tag"}, {"reason", "invalid_argument"}}),
        terminal_error("NOT_AUTHED",
                       {{"account", "main"}, {"state", "wait_code"}, {"reason", "not_ready"}}),
        json{{"error",
              {{"code", "BOT_UNSUPPORTED"},
               {"message", "saved commands require a user account"},
               {"details", json::object()}}}},
        terminal_error("RATE_LIMITED",
                       {{"operation", "saved_search"}, {"tdlib_code", 429}, {"retry_after", 5}}),
        terminal_error("TDLIB_ERROR", {{"operation", "saved_search"}, {"tdlib_code", 400}}),
        terminal_error("TDLIB_ERROR", {{"operation", "saved_tags"}, {"tdlib_type_id", 436294381}}),
        terminal_error(
            "TDLIB_ERROR",
            {{"operation", "saved_tags"}, {"tdlib_type_id", -989117709}, {"custom_emoji_id", 0}}),
        terminal_error("TIMEOUT", {{"operation", "saved_tags"}, {"state", "ready"}}),
        terminal_error("INTERNAL", {{"operation", "saved_search"}, {"reason", "internal_error"}}),
    };
    for (const auto& error : errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("saved.error.schema.json"));
    }

    auto unknown = errors.front();
    unknown["error"]["details"]["extra"] = true;
    CHECK_THAT(unknown, !tgcli::test::matches_json_schema("saved.error.schema.json"));
}

TEST_CASE("destructive error schemas close command shapes and uint64 request ids",
          "[schema][destructive][error]") {
    const auto logout_confirmation =
        terminal_error("CONFIRMATION_REQUIRED",
                       {{"account", "main"}, {"action", "logout"}, {"target", logout_plan()}});
    CHECK_THAT(logout_confirmation, tgcli::test::matches_json_schema("logout.error.schema.json"));

    const auto removal_confirmation = terminal_error(
        "CONFIRMATION_REQUIRED",
        {{"account", "work"}, {"action", "account_remove"}, {"target", removal_plan()}});
    CHECK_THAT(removal_confirmation,
               tgcli::test::matches_json_schema("account-remove.error.schema.json"));

    auto cross_command = logout_confirmation;
    cross_command["error"]["details"]["action"] = "account_remove";
    cross_command["error"]["details"]["target"] = removal_plan();
    CHECK_THAT(cross_command, !tgcli::test::matches_json_schema("logout.error.schema.json"));

    auto missing = removal_confirmation;
    missing["error"].erase("message");
    CHECK_THAT(missing, !tgcli::test::matches_json_schema("account-remove.error.schema.json"));

    auto additional = logout_confirmation;
    additional["error"]["details"]["answer"] = true;
    CHECK_THAT(additional, !tgcli::test::matches_json_schema("logout.error.schema.json"));

    auto invalid_enum =
        terminal_error("WRITE_DENIED", {{"account", "work"}, {"reason", "administrator_override"}});
    CHECK_THAT(invalid_enum, !tgcli::test::matches_json_schema("account-remove.error.schema.json"));

    const auto maximum = terminal_error(
        "PROTOCOL_ANSWER_INVALID",
        {{"request_id", std::numeric_limits<std::uint64_t>::max()}, {"reason", "malformed"}});
    CHECK_THAT(maximum, tgcli::test::matches_json_schema("logout.error.schema.json"));
    CHECK_THAT(maximum, tgcli::test::matches_json_schema("account-remove.error.schema.json"));
    CHECK_THAT(maximum, tgcli::test::matches_json_schema("auth.error.schema.json"));

    const auto overflow = json::parse(
        R"({"error":{"code":"PROTOCOL_ANSWER_INVALID","message":"bad","details":{"request_id":18446744073709551616,"reason":"malformed"}}})");
    CHECK_THAT(overflow, !tgcli::test::matches_json_schema("logout.error.schema.json"));
    CHECK_THAT(overflow, !tgcli::test::matches_json_schema("account-remove.error.schema.json"));
    CHECK_THAT(overflow, !tgcli::test::matches_json_schema("auth.error.schema.json"));

    const auto incomplete = terminal_error(
        "AUDIT_INCOMPLETE",
        {{"account", "main"},
         {"path", "/state/main/audit.log"},
         {"mutation_state", "possible"},
         {"completed_stages", json::array({"intent_synced", "logout_send_started"})}});
    CHECK_THAT(incomplete, tgcli::test::matches_json_schema("logout.error.schema.json"));
    auto wrong_mutation = incomplete;
    wrong_mutation["error"]["details"]["mutation_state"] = "confirmed";
    CHECK_THAT(wrong_mutation, !tgcli::test::matches_json_schema("logout.error.schema.json"));
}

TEST_CASE("audit schemas enforce command result and durable-stage relationships",
          "[schema][destructive][audit]") {
    auto logout_intent = audit_identity("intent", "logout", "main");
    logout_intent.update({{"arguments", json::object()},
                          {"plan", logout_plan()},
                          {"config_snapshot", "missing"},
                          {"authority_source", "request"},
                          {"confirmation_source", "yes"}});
    CHECK_THAT(logout_intent, tgcli::test::matches_json_schema("audit-intent.schema.json"));

    auto removal_intent = audit_identity("intent", "account_remove", "work");
    removal_intent.update({{"arguments", {{"keep_session", false}, {"reassign_default", "main"}}},
                           {"plan", removal_plan()},
                           {"config_snapshot", removal_plan()["config_snapshot"]},
                           {"authority_source", "config"},
                           {"confirmation_source", "tty"}});
    CHECK_THAT(removal_intent, tgcli::test::matches_json_schema("audit-intent.schema.json"));

    auto wrong_intent = removal_intent;
    wrong_intent["command"] = "logout";
    CHECK_THAT(wrong_intent, !tgcli::test::matches_json_schema("audit-intent.schema.json"));
    wrong_intent = removal_intent;
    wrong_intent["unexpected"] = true;
    CHECK_THAT(wrong_intent, !tgcli::test::matches_json_schema("audit-intent.schema.json"));
    wrong_intent = removal_intent;
    wrong_intent["arguments"]["keep_session"] = true;
    CHECK_THAT(wrong_intent, !tgcli::test::matches_json_schema("audit-intent.schema.json"));

    auto checkpoint = audit_identity("checkpoint", "logout", "main");
    checkpoint["stage"] = "logout_send_started";
    CHECK_THAT(checkpoint, tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));
    checkpoint["stage"] = "remote_logout_send_started";
    CHECK_THAT(checkpoint, !tgcli::test::matches_json_schema("audit-checkpoint.schema.json"));

    auto logout_outcome = audit_identity("outcome", "logout", "main");
    logout_outcome.update({{"success", true},
                           {"mutation_state", "confirmed"},
                           {"completed_stages", json::array({"intent_synced", "logout_send_started",
                                                             "logout_closed_confirmed"})},
                           {"result", {{"account", "main"}, {"logged_out", true}}},
                           {"error", nullptr}});
    CHECK_THAT(logout_outcome, tgcli::test::matches_json_schema("audit-outcome.schema.json"));

    auto failure = logout_outcome;
    failure["success"] = false;
    failure["mutation_state"] = "possible";
    failure["completed_stages"] = json::array({"intent_synced", "logout_send_started"});
    failure["result"] = nullptr;
    failure["error"] = {{"code", "TIMEOUT"},
                        {"details", {{"operation", "logout"}, {"state", "ready"}}}};
    CHECK_THAT(failure, tgcli::test::matches_json_schema("audit-outcome.schema.json"));
    auto cross_command_error = failure;
    cross_command_error["error"]["details"]["operation"] = "account_remove";
    CHECK_THAT(cross_command_error, !tgcli::test::matches_json_schema("audit-outcome.schema.json"));

    auto success_with_error = logout_outcome;
    success_with_error["error"] = failure["error"];
    CHECK_THAT(success_with_error, !tgcli::test::matches_json_schema("audit-outcome.schema.json"));

    auto removal_outcome = audit_identity("outcome", "account_remove", "work");
    removal_outcome.update(
        {{"success", true},
         {"mutation_state", "possible"},
         {"completed_stages",
          json::array({"planned", "intent_synced", "remote_logout_send_started",
                       "remote_not_present", "client_close_started", "client_closed",
                       "config_remove_started", "config_removed", "data_remove_started",
                       "data_removed", "state_remove_started", "state_removed"})},
         {"result",
          {{"account", "work"},
           {"removed", true},
           {"remote_logout", "not_present"},
           {"default_account", "main"}}},
         {"error", nullptr}});
    CHECK_THAT(removal_outcome, tgcli::test::matches_json_schema("audit-outcome.schema.json"));

    auto wrong_remote = removal_outcome;
    wrong_remote["result"]["remote_logout"] = "confirmed";
    CHECK_THAT(wrong_remote, !tgcli::test::matches_json_schema("audit-outcome.schema.json"));
    auto wrong_removal_mutation = removal_outcome;
    wrong_removal_mutation["mutation_state"] = "confirmed";
    CHECK_THAT(wrong_removal_mutation,
               !tgcli::test::matches_json_schema("audit-outcome.schema.json"));

    auto kept_failure = removal_outcome;
    kept_failure["success"] = false;
    kept_failure["mutation_state"] = "none";
    kept_failure["completed_stages"] = json::array({"planned", "intent_synced", "remote_kept"});
    kept_failure["result"] = nullptr;
    kept_failure["error"] = {
        {"code", "REMOTE_LOGOUT_UNCONFIRMED"},
        {"details", {{"account", "work"}, {"state", "unknown"}, {"reason", "state_unproven"}}}};
    CHECK_THAT(kept_failure, !tgcli::test::matches_json_schema("audit-outcome.schema.json"));
    kept_failure["error"] = {
        {"code", "INTERNAL"},
        {"details", {{"operation", "account_remove"}, {"reason", "internal_error"}}}};
    CHECK_THAT(kept_failure, tgcli::test::matches_json_schema("audit-outcome.schema.json"));
}

TEST_CASE("removal tombstone schema binds policy stage prefix and next stage",
          "[schema][destructive][removal]") {
    auto tombstone = json{{"schema_version", 1},
                          {"invocation_id", "00112233445566778899aabbccddeeff"},
                          {"account", "work"},
                          {"stage", "intent_synced"},
                          {"completed_stages", json::array({"planned", "intent_synced"})},
                          {"next_stage", nullptr},
                          {"plan", removal_plan()},
                          {"config_snapshot", removal_plan()["config_snapshot"]},
                          {"data_root", removal_plan()["data_root"]},
                          {"state_root", nullptr}};
    tombstone["data_root"]["device"] = std::numeric_limits<std::uint64_t>::max();
    CHECK_THAT(tombstone, tgcli::test::matches_json_schema("removal-tombstone.schema.json"));

    auto wrong_next = tombstone;
    wrong_next["next_stage"] = "remote_logout_send_started";
    CHECK_THAT(wrong_next, !tgcli::test::matches_json_schema("removal-tombstone.schema.json"));

    auto wrong_policy = tombstone;
    wrong_policy["plan"] = removal_plan(true);
    CHECK_THAT(wrong_policy, !tgcli::test::matches_json_schema("removal-tombstone.schema.json"));

    auto overflow = tombstone;
    overflow["data_root"]["device"] = json::parse("18446744073709551616");
    CHECK_THAT(overflow, !tgcli::test::matches_json_schema("removal-tombstone.schema.json"));

    auto missing = tombstone;
    missing.erase("stage");
    CHECK_THAT(missing, !tgcli::test::matches_json_schema("removal-tombstone.schema.json"));

    auto outcome_without_intent = tombstone;
    outcome_without_intent["stage"] = "outcome_synced";
    outcome_without_intent["completed_stages"] = json::array({"planned", "outcome_synced"});
    outcome_without_intent["next_stage"] = nullptr;
    CHECK_THAT(outcome_without_intent,
               !tgcli::test::matches_json_schema("removal-tombstone.schema.json"));
}

TEST_CASE("auth function denial has a closed exact detail schema", "[schema][auth]") {
    const std::vector<std::string_view> functions{
        "getAuthorizationState",
        "setTdlibParameters",
        "setAuthenticationPhoneNumber",
        "requestQrCodeAuthentication",
        "checkAuthenticationBotToken",
        "setAuthenticationEmailAddress",
        "checkAuthenticationEmailCode",
        "checkAuthenticationCode",
        "registerUser",
        "checkAuthenticationPassword",
        "getMe",
        "logOut",
        "close",
        "other",
    };
    for (const auto function : functions) {
        const json error{
            {"error",
             {{"code", "AUTH_FUNCTION_DENIED"},
              {"message", "TDLib authorization function was denied"},
              {"details", {{"account", "main"}, {"state", "ready"}, {"function", function}}}}}};
        CHECK_THAT(error, tgcli::test::matches_json_schema("auth.error.schema.json"));
    }

    json invalid{
        {"error",
         {{"code", "AUTH_FUNCTION_DENIED"},
          {"message", "denied"},
          {"details", {{"account", "main"}, {"state", "ready"}, {"function", "getOption"}}}}}};
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("auth.error.schema.json"));

    invalid["error"]["details"]["function"] = "GETME";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("auth.error.schema.json"));

    invalid["error"]["details"]["function"] = "getMe";
    invalid["error"]["details"]["state"] = "Ready";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("auth.error.schema.json"));

    invalid["error"]["details"]["state"] = "ready";
    invalid["error"]["details"]["unexpected"] = true;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("auth.error.schema.json"));

    invalid["error"]["details"].erase("unexpected");
    invalid["error"]["details"].erase("function");
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("auth.error.schema.json"));
}

TEST_CASE("daemon control errors match their closed schema", "[schema][daemon-control]") {
    const std::vector<json> errors{
        {{"error", {{"code", "USAGE"}, {"message", "unsupported"}, {"details", json::object()}}}},
        {{"error",
          {{"code", "USAGE"},
           {"message", "invalid timeout"},
           {"details", {{"argument", "--timeout"}, {"reason", "invalid_argument"}}}}}},
        {{"error",
          {{"code", "DAEMON_NOT_RUNNING"},
           {"message", "daemon is not running"},
           {"details", {{"account", "main"}, {"socket", "/tmp/main.sock"}}}}}},
        {{"error",
          {{"code", "DAEMON_CONTROL_FAILED"},
           {"message", "cannot stop daemon"},
           {"details",
            {{"account", "main"}, {"operation", "stop"}, {"reason", "surface_invalid"}}}}}}};
    for (const auto& error : errors) {
        CHECK_THAT(error, tgcli::test::matches_json_schema("daemon.error.schema.json"));
    }

    auto unknown_reason = errors.back();
    unknown_reason["error"]["details"]["reason"] = "unsafe_guess";
    CHECK_THAT(unknown_reason, !tgcli::test::matches_json_schema("daemon.error.schema.json"));
}

TEST_CASE("result schemas reject missing required and unknown properties", "[schema]") {
    for (const auto& schema_case : schema_cases()) {
        INFO("schema: " << schema_case.filename);
        CHECK_THAT(schema_case.instance, tgcli::test::matches_json_schema(schema_case.filename));

        auto missing = schema_case.instance;
        missing.erase(schema_case.required_property);
        CHECK_THAT(missing, !tgcli::test::matches_json_schema(schema_case.filename));

        auto unknown = schema_case.instance;
        unknown["unexpected"] = true;
        CHECK_THAT(unknown, !tgcli::test::matches_json_schema(schema_case.filename));

        if (schema_case.has_nested_object) {
            auto nested_unknown = schema_case.instance;
            nested_unknown["daemon"]["unexpected"] = true;
            CHECK_THAT(nested_unknown, !tgcli::test::matches_json_schema(schema_case.filename));
        }
    }

    auto list_item_unknown = schema_cases().at(1).instance;
    list_item_unknown["items"][0]["unexpected"] = true;
    CHECK_THAT(list_item_unknown,
               !tgcli::test::matches_json_schema("account-list.result.schema.json"));
}

TEST_CASE("resolve result schema enforces kind-specific topic id bounds",
          "[schema][resolver][topic]") {
    const auto cases = schema_cases();
    const auto resolve =
        std::ranges::find(cases, std::string{"resolve.result.schema.json"}, &SchemaCase::filename);
    REQUIRE(resolve != cases.end());

    struct TopicCase {
        std::string_view kind;
        std::int64_t maximum;
    };
    for (const auto& topic :
         {TopicCase{"forum", 2147483647}, TopicCase{"thread", 9007199254740991},
          TopicCase{"direct", 9007199254740991}, TopicCase{"saved", 9007199254740991}}) {
        DYNAMIC_SECTION(topic.kind) {
            auto instance = resolve->instance;
            instance["topic"] = {{"kind", topic.kind}, {"id", 1}};
            CHECK_THAT(instance, tgcli::test::matches_json_schema("resolve.result.schema.json"));

            instance["topic"]["id"] = topic.maximum;
            CHECK_THAT(instance, tgcli::test::matches_json_schema("resolve.result.schema.json"));

            instance["topic"]["id"] = 0;
            CHECK_THAT(instance, !tgcli::test::matches_json_schema("resolve.result.schema.json"));

            instance["topic"]["id"] = topic.maximum + 1;
            CHECK_THAT(instance, !tgcli::test::matches_json_schema("resolve.result.schema.json"));
        }
    }
}

TEST_CASE("resolve result schema ties kind to message and topic presence",
          "[schema][resolver][discriminator]") {
    const auto cases = schema_cases();
    const auto resolve =
        std::ranges::find(cases, std::string{"resolve.result.schema.json"}, &SchemaCase::filename);
    REQUIRE(resolve != cases.end());

    auto message = resolve->instance;
    REQUIRE_THAT(message, tgcli::test::matches_json_schema("resolve.result.schema.json"));
    message["topic"] = nullptr;
    CHECK_THAT(message, tgcli::test::matches_json_schema("resolve.result.schema.json"));

    auto topic = resolve->instance;
    topic["kind"] = "topic";
    topic["message_id"] = nullptr;
    CHECK_THAT(topic, tgcli::test::matches_json_schema("resolve.result.schema.json"));

    auto chat = resolve->instance;
    chat["kind"] = "chat";
    chat["message_id"] = nullptr;
    chat["topic"] = nullptr;
    CHECK_THAT(chat, tgcli::test::matches_json_schema("resolve.result.schema.json"));

    for (const auto& invalid : {
             nlohmann::json{{"kind", "chat"}, {"message_id", 123}, {"topic", nullptr}},
             nlohmann::json{{"kind", "topic"}, {"message_id", 123}, {"topic", nullptr}},
             nlohmann::json{{"kind", "chat"},
                            {"message_id", nullptr},
                            {"topic", {{"kind", "forum"}, {"id", 7}}}},
             nlohmann::json{{"kind", "message"},
                            {"message_id", nullptr},
                            {"topic", {{"kind", "forum"}, {"id", 7}}}},
             nlohmann::json{{"kind", "message"}, {"message_id", nullptr}, {"topic", nullptr}},
             nlohmann::json{{"kind", "topic"}, {"message_id", nullptr}, {"topic", nullptr}},
         }) {
        auto mismatch = resolve->instance;
        mismatch.update(invalid);
        CHECK_THAT(mismatch, !tgcli::test::matches_json_schema("resolve.result.schema.json"));
    }
}

TEST_CASE("account show nested closure starts from its named valid fixture",
          "[schema][regression]") {
    const auto cases = schema_cases();
    const auto account_show = std::ranges::find(
        cases, std::string{"account-show.result.schema.json"}, &SchemaCase::filename);
    REQUIRE(account_show != cases.end());

    auto credentials_unknown = account_show->instance;
    REQUIRE_THAT(credentials_unknown,
                 tgcli::test::matches_json_schema("account-show.result.schema.json"));
    credentials_unknown["credentials"]["unexpected"] = true;
    CHECK_THAT(credentials_unknown,
               !tgcli::test::matches_json_schema("account-show.result.schema.json"));

    auto paths_unknown = account_show->instance;
    REQUIRE_THAT(paths_unknown,
                 tgcli::test::matches_json_schema("account-show.result.schema.json"));
    paths_unknown["paths"]["unexpected"] = true;
    CHECK_THAT(paths_unknown, !tgcli::test::matches_json_schema("account-show.result.schema.json"));
}

TEST_CASE("account removal schema accepts only the exact dry-run plan", "[schema][removal]") {
    const json dry{
        {"dry_run", true},
        {"plan",
         {{"operation", "account_remove"},
          {"account", "work"},
          {"remote_logout", true},
          {"keep_session", false},
          {"delete_paths", json::array({"/data/work", "/state/work"})},
          {"config_path", "/config/tgcli/config.toml"},
          {"config_snapshot",
           "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
           "dev:1;ino:2;size:3;ctime_ns:4"},
          {"data_root", nullptr},
          {"state_root", {{"path", "/state/work"}, {"device", 1}, {"inode", 2}, {"owner", 1000}}},
          {"reassign_default", "main"}}}};
    CHECK_THAT(dry, tgcli::test::matches_json_schema("account-remove.result.schema.json"));

    auto extra = dry;
    extra["plan"]["unexpected"] = true;
    CHECK_THAT(extra, !tgcli::test::matches_json_schema("account-remove.result.schema.json"));
    auto wrong_policy = dry;
    wrong_policy["plan"]["remote_logout"] = false;
    CHECK_THAT(wrong_policy,
               !tgcli::test::matches_json_schema("account-remove.result.schema.json"));

    auto keep_session = dry;
    keep_session["plan"]["remote_logout"] = false;
    keep_session["plan"]["keep_session"] = true;
    CHECK_THAT(keep_session, tgcli::test::matches_json_schema("account-remove.result.schema.json"));
}

TEST_CASE("account removal failures match the closed account error schema", "[schema][removal]") {
    const json plan{{"operation", "account_remove"},
                    {"account", "work"},
                    {"remote_logout", true},
                    {"keep_session", false},
                    {"delete_paths", json::array({"/data/work", "/state/work"})},
                    {"config_path", "/config/tgcli/config.toml"},
                    {"config_snapshot", "snapshot"},
                    {"data_root", nullptr},
                    {"state_root", nullptr},
                    {"reassign_default", "main"}};
    const std::vector<json> errors{
        {{"error",
          {{"code", "DEFAULT_REASSIGNMENT_REQUIRED"},
           {"message", "replacement required"},
           {"details", {{"account", "work"}, {"candidates", json::array({"main"})}}}}}},
        {{"error",
          {{"code", "ACCOUNT_MISMATCH"},
           {"message", "account mismatch"},
           {"details", {{"requested_account", "work"}, {"daemon_account", "main"}}}}}},
        {{"error",
          {{"code", "WRITE_DENIED"},
           {"message", "write denied"},
           {"details", {{"account", "work"}, {"reason", "no_grant"}}}}}},
        {{"error",
          {{"code", "CONFIRMATION_REQUIRED"},
           {"message", "confirmation required"},
           {"details", {{"account", "work"}, {"action", "account_remove"}, {"target", plan}}}}}},
        {{"error",
          {{"code", "AUDIT_UNAVAILABLE"},
           {"message", "audit unavailable"},
           {"details", {{"account", "work"}, {"path", "/audit"}, {"reason", "sync_failed"}}}}}},
        {{"error",
          {{"code", "REMOTE_LOGOUT_UNCONFIRMED"},
           {"message", "remote state unproven"},
           {"details", {{"account", "work"}, {"state", "closing"}, {"reason", "timeout"}}}}}},
        {{"error",
          {{"code", "LOCAL_CLEANUP_FAILED"},
           {"message", "cleanup failed"},
           {"details",
            {{"account", "work"},
             {"reason", "path_changed"},
             {"removed", json::array()},
             {"retained", json::array({"/data/work", "/state/work"})}}}}}},
        {{"error",
          {{"code", "REMOVAL_INCOMPLETE"},
           {"message", "retry required"},
           {"details",
            {{"account", "work"},
             {"path", "/removals/0011.json"},
             {"invocation_id", "00112233445566778899aabbccddeeff"},
             {"stage", "intent_synced"},
             {"completed_stages", json::array({"planned", "intent_synced"})},
             {"reason", "prior_crash"}}}}}},
        {{"error",
          {{"code", "RATE_LIMITED"},
           {"message", "rate limited"},
           {"details",
            {{"operation", "account_remove"}, {"tdlib_code", 429}, {"retry_after", 3}}}}}},
        {{"error",
          {{"code", "TDLIB_ERROR"},
           {"message", "tdlib error"},
           {"details", {{"operation", "account_remove"}, {"tdlib_code", 500}}}}}},
        {{"error",
          {{"code", "TIMEOUT"},
           {"message", "timed out"},
           {"details", {{"operation", "account_remove"}, {"state", "unknown"}}}}}},
        {{"error",
          {{"code", "DAEMON_SHUTDOWN"},
           {"message", "daemon shutdown"},
           {"details", {{"reason", "daemon_shutdown"}}}}}},
        {{"error",
          {{"code", "INTERNAL"},
           {"message", "internal"},
           {"details", {{"operation", "account_remove"}, {"reason", "internal_error"}}}}}},
    };

    for (const auto& error : errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("account.error.schema.json"));
    }

    auto wrong_plan_policy = errors.at(3);
    wrong_plan_policy["error"]["details"]["target"]["remote_logout"] = false;
    CHECK_THAT(wrong_plan_policy, !tgcli::test::matches_json_schema("account.error.schema.json"));
}
