#include "schema_matcher.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

using nlohmann::json;

namespace {

json session(std::string id = "0", std::string device_type = "unknown") {
    return {{"id", std::move(id)},
            {"is_current", true},
            {"is_password_pending", false},
            {"is_unconfirmed", false},
            {"can_accept_secret_chats", true},
            {"can_accept_calls", true},
            {"device_type", std::move(device_type)},
            {"api_id", 2040},
            {"application_name", "Telegram Desktop"},
            {"application_version", "5.1"},
            {"is_official_application", true},
            {"device_model", "Workstation"},
            {"platform", "Linux"},
            {"system_version", "6.10"},
            {"log_in_date", "1970-01-01T00:00:01Z"},
            {"last_active_date", "2038-01-19T03:14:07Z"},
            {"ip_address", "203.0.113.10"},
            {"location", "Athens, Greece"}};
}

json list_result(json items = json::array({session()})) {
    return {{"items", std::move(items)}, {"inactive_session_ttl_days", 366}, {"next", nullptr}};
}

json target(std::string id = "0", std::string device_type = "unknown") {
    return {{"id", std::move(id)},
            {"is_current", false},
            {"is_password_pending", false},
            {"is_unconfirmed", true},
            {"device_type", std::move(device_type)},
            {"application_name", "Telegram Desktop"},
            {"application_version", "5.1"},
            {"device_model", "Workstation"},
            {"platform", "Linux"},
            {"system_version", "6.10"},
            {"last_active_date", "2038-01-19T03:14:07Z"}};
}

json terminate_plan(std::string id = "0") {
    return {{"operation", "session_terminate"},
            {"account", "main"},
            {"tdlib_request", "terminateSession"},
            {"session", target(std::move(id))}};
}

json terminal_error(std::string code, json details, std::string message = "contract error") {
    return {{"error",
             {{"code", std::move(code)},
              {"message", std::move(message)},
              {"details", std::move(details)}}}};
}

json audit_incomplete(std::string mutation_state, json completed_stages) {
    return terminal_error("AUDIT_INCOMPLETE", {{"account", "main"},
                                               {"path", "/audit"},
                                               {"mutation_state", std::move(mutation_state)},
                                               {"completed_stages", std::move(completed_stages)}});
}

json spool_unavailable(std::string operation, std::string reason) {
    return terminal_error(
        "SPOOL_UNAVAILABLE",
        {{"operation", std::move(operation)}, {"path", "spool/"}, {"reason", std::move(reason)}},
        "attachment spool is unavailable");
}

json spool_audit_incomplete(json path = {{"kind", "bytes_hex"},
                                         {"value", "2f73746174652f73706f6f6c2fff"}}) {
    return terminal_error("AUDIT_INCOMPLETE",
                          {{"account", "main"},
                           {"path", std::move(path)},
                           {"mutation_state", "none"},
                           {"completed_stages", json::array()}},
                          "attachment spool recovery is incomplete");
}

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

} // namespace

TEST_CASE("session schemas are self-contained strict Draft 2020-12 documents",
          "[schema][session]") {
    for (const auto* filename :
         {"session-list.result.schema.json", "session-terminate.result.schema.json",
          "session.error.schema.json"}) {
        const auto schema = tgcli::test::load_schema_document(filename);
        INFO(filename);
        REQUIRE(schema["$schema"] == "https://json-schema.org/draft/2020-12/schema");
        check_schema_node(schema);
    }

    const auto list_schema = tgcli::test::load_schema_document("session-list.result.schema.json");
    CHECK(list_schema["$defs"]["session"]["required"].size() == 18);
    CHECK(list_schema["$defs"]["deviceType"]["enum"].size() == 17);
    CHECK(list_schema["properties"]["items"]["type"] == "array");

    const auto error_schema = tgcli::test::load_schema_document("session.error.schema.json");
    json references = json::array();
    for (const auto& branch : error_schema["properties"]["error"]["oneOf"]) {
        references.push_back(branch["$ref"]);
    }
    CHECK(references == json::array({"#/$defs/usageError",
                                     "#/$defs/accountNotFoundError",
                                     "#/$defs/accountMismatchError",
                                     "#/$defs/configInvalidError",
                                     "#/$defs/configConflictError",
                                     "#/$defs/hookFailedError",
                                     "#/$defs/notAuthedError",
                                     "#/$defs/writeDeniedError",
                                     "#/$defs/confirmationRequiredError",
                                     "#/$defs/auditUnavailableError",
                                     "#/$defs/spoolUnavailableError",
                                     "#/$defs/auditIncompleteError",
                                     "#/$defs/spoolAuditIncompleteError",
                                     "#/$defs/protocolAnswerInvalidError",
                                     "#/$defs/daemonNotRunningError",
                                     "#/$defs/daemonControlFailedError",
                                     "#/$defs/daemonShutdownError",
                                     "#/$defs/botUnsupportedError",
                                     "#/$defs/notFoundError",
                                     "#/$defs/preconditionFailedError",
                                     "#/$defs/tdlibError",
                                     "#/$defs/rateLimitedError",
                                     "#/$defs/internalError",
                                     "#/$defs/dispatcherInternalError",
                                     "#/$defs/timeoutError"}));
    CHECK(error_schema["$defs"]["filesystemDiagnosticPath"]["properties"]["value"]["pattern"] ==
          "^2f(?:0[1-9a-f]|[1-9a-f][0-9a-f])+$");
    CHECK(error_schema["$defs"]["auditIncompleteError"]["properties"]["details"]["oneOf"].size() ==
          6);
    CHECK(error_schema["$defs"]["auditUnavailableError"]["properties"]["details"]["properties"]
                      ["reason"]["enum"] ==
          json::array({"path_invalid", "wrong_owner", "wrong_type", "wrong_mode",
                       "wrong_link_count", "too_large", "capacity_exhausted", "open_failed",
                       "lock_failed", "read_failed", "write_failed", "sync_failed", "rename_failed",
                       "directory_sync_failed", "parse_error", "schema_error", "contradiction"}));
}

TEST_CASE("session id schemas accept exactly canonical signed int64 strings", "[schema][session]") {
    const std::vector<std::string> valid_ids{
        "-9223372036854775808", "-9007199254740992",  "-1", "0", "1",
        "9007199254740992",     "9223372036854775807"};
    for (const auto& id : valid_ids) {
        INFO(id);
        auto list = list_result();
        list["items"][0]["id"] = id;
        CHECK_THAT(list, tgcli::test::matches_json_schema("session-list.result.schema.json"));
        CHECK_THAT(json({{"session_id", id}, {"terminated", true}}),
                   tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
        CHECK_THAT(terminal_error("NOT_FOUND",
                                  {{"operation", "session_terminate"}, {"session_id", id}},
                                  "session not found"),
                   tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    const std::vector<json> invalid_ids{"-9223372036854775809",
                                        "9223372036854775808",
                                        "-0",
                                        "+1",
                                        "00",
                                        "01",
                                        "-01",
                                        " 1",
                                        "1 ",
                                        "",
                                        0,
                                        1,
                                        nullptr};
    for (const auto& id : invalid_ids) {
        INFO(id.dump());
        auto list = list_result();
        list["items"][0]["id"] = id;
        CHECK_THAT(list, !tgcli::test::matches_json_schema("session-list.result.schema.json"));

        const json real{{"session_id", id}, {"terminated", true}};
        CHECK_THAT(real, !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

        const auto error =
            terminal_error("NOT_FOUND", {{"operation", "session_terminate"}, {"session_id", id}},
                           "session not found");
        CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
}

TEST_CASE("session timestamp schemas enforce the emitted calendar and Unix range",
          "[schema][session]") {
    const std::vector<json> valid_timestamps{nullptr, "1970-01-01T00:00:01Z",
                                             "1972-02-29T12:34:56Z", "2000-02-29T23:59:59Z",
                                             "2038-01-19T03:14:07Z"};
    for (const auto& timestamp : valid_timestamps) {
        INFO(timestamp.dump());
        auto list = list_result();
        list["items"][0]["log_in_date"] = timestamp;
        list["items"][0]["last_active_date"] = timestamp;
        CHECK_THAT(list, tgcli::test::matches_json_schema("session-list.result.schema.json"));

        const json dry_run{{"dry_run", true}, {"plan", terminate_plan()}};
        auto terminate = dry_run;
        terminate["plan"]["session"]["last_active_date"] = timestamp;
        CHECK_THAT(terminate,
                   tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

        auto confirmation = terminal_error(
            "CONFIRMATION_REQUIRED",
            {{"account", "main"}, {"action", "session_terminate"}, {"target", terminate_plan()}});
        confirmation["error"]["details"]["target"]["session"]["last_active_date"] = timestamp;
        CHECK_THAT(confirmation, tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    const std::vector<json> invalid_timestamps{"1969-12-31T23:59:59Z",
                                               "1970-01-01T00:00:00Z",
                                               "2001-02-29T00:00:00Z",
                                               "2000-13-01T00:00:00Z",
                                               "2000-04-31T00:00:00Z",
                                               "2000-01-01T24:00:00Z",
                                               "2000-01-01T00:60:00Z",
                                               "2000-01-01T00:00:60Z",
                                               "2000-01-01T00:00:00.1Z",
                                               "2000-01-01T00:00:00+00:00",
                                               "2038-01-19T03:14:08Z",
                                               1,
                                               false};
    for (const auto& timestamp : invalid_timestamps) {
        INFO(timestamp.dump());
        auto list = list_result();
        list["items"][0]["log_in_date"] = timestamp;
        CHECK_THAT(list, !tgcli::test::matches_json_schema("session-list.result.schema.json"));

        json terminate{{"dry_run", true}, {"plan", terminate_plan()}};
        terminate["plan"]["session"]["last_active_date"] = timestamp;
        CHECK_THAT(terminate,
                   !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

        auto confirmation = terminal_error(
            "CONFIRMATION_REQUIRED",
            {{"account", "main"}, {"action", "session_terminate"}, {"target", terminate_plan()}});
        confirmation["error"]["details"]["target"]["session"]["last_active_date"] = timestamp;
        CHECK_THAT(confirmation, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
}

TEST_CASE("session list schema closes fields types TTL and device variants", "[schema][session]") {
    const std::vector<std::string> device_types{
        "android", "apple", "brave",  "chrome", "edge",    "firefox", "ipad",    "iphone", "linux",
        "mac",     "opera", "safari", "ubuntu", "unknown", "vivaldi", "windows", "xbox"};
    for (const auto& device_type : device_types) {
        INFO(device_type);
        CHECK_THAT(list_result(json::array({session("0", device_type)})),
                   tgcli::test::matches_json_schema("session-list.result.schema.json"));
    }

    auto ordered = list_result(json::array({session("9", "linux"), session("-4", "iphone")}));
    ordered["items"][0]["is_current"] = false;
    CHECK_THAT(ordered, tgcli::test::matches_json_schema("session-list.result.schema.json"));
    CHECK(ordered["items"][0]["id"] == "9");
    CHECK(ordered["items"][1]["id"] == "-4");

    auto api_minimum = list_result();
    api_minimum["items"][0]["api_id"] = std::numeric_limits<std::int32_t>::min();
    CHECK_THAT(api_minimum, tgcli::test::matches_json_schema("session-list.result.schema.json"));
    auto api_maximum = list_result();
    api_maximum["items"][0]["api_id"] = std::numeric_limits<std::int32_t>::max();
    CHECK_THAT(api_maximum, tgcli::test::matches_json_schema("session-list.result.schema.json"));

    std::vector<json> invalid;
    auto value = list_result();
    value["unexpected"] = true;
    invalid.push_back(value);
    value = list_result();
    value.erase("next");
    invalid.push_back(value);
    value = list_result();
    value["next"] = false;
    invalid.push_back(value);
    value = list_result();
    value["inactive_session_ttl_days"] = 0;
    invalid.push_back(value);
    value = list_result();
    value["inactive_session_ttl_days"] = 367;
    invalid.push_back(value);
    value = list_result();
    value["inactive_session_ttl_days"] = "30";
    invalid.push_back(value);
    value = list_result();
    value["items"] = json::object();
    invalid.push_back(value);
    value = list_result();
    value["items"][0]["unexpected"] = true;
    invalid.push_back(value);
    value = list_result();
    value["items"][0].erase("location");
    invalid.push_back(value);
    value = list_result();
    value["items"][0]["is_current"] = "true";
    invalid.push_back(value);
    value = list_result();
    value["items"][0]["api_id"] = std::int64_t{2147483648};
    invalid.push_back(value);
    value = list_result();
    value["items"][0]["device_type"] = "future_device";
    invalid.push_back(value);

    for (const auto& candidate : invalid) {
        INFO(candidate.dump());
        CHECK_THAT(candidate, !tgcli::test::matches_json_schema("session-list.result.schema.json"));
    }
}

TEST_CASE("session terminate result is one exact real or redacted dry-run branch",
          "[schema][session]") {
    const json real{{"session_id", "0"}, {"terminated", true}};
    const json dry_run{{"dry_run", true}, {"plan", terminate_plan("9007199254740992")}};
    CHECK_THAT(real, tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    CHECK_THAT(dry_run, tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

    auto cross_branch = real;
    cross_branch["dry_run"] = true;
    CHECK_THAT(cross_branch,
               !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    cross_branch = dry_run;
    cross_branch["session_id"] = "0";
    CHECK_THAT(cross_branch,
               !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

    auto invalid = real;
    invalid["terminated"] = false;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    invalid = dry_run;
    invalid["dry_run"] = false;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    invalid = dry_run;
    invalid["plan"]["unexpected"] = true;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    invalid = dry_run;
    invalid["plan"]["session"]["is_current"] = true;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    invalid = dry_run;
    invalid["plan"]["session"]["device_type"] = "future_device";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

    for (const auto* personal_field : {"ip_address", "location"}) {
        invalid = dry_run;
        invalid["plan"]["session"][personal_field] = "must not be in a termination plan";
        INFO(personal_field);
        CHECK_THAT(invalid,
                   !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    }
}

TEST_CASE("session terminate plans enforce the local account-name language", "[schema][session]") {
    const std::vector<std::string> valid_accounts{"a", std::string(32, 'Z')};
    for (const auto& account : valid_accounts) {
        INFO(account);
        json dry_run{{"dry_run", true}, {"plan", terminate_plan()}};
        dry_run["plan"]["account"] = account;
        CHECK_THAT(dry_run,
                   tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

        auto confirmation = terminal_error(
            "CONFIRMATION_REQUIRED",
            {{"account", "main"}, {"action", "session_terminate"}, {"target", terminate_plan()}});
        confirmation["error"]["details"]["target"]["account"] = account;
        CHECK_THAT(confirmation, tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    const std::vector<std::string> invalid_accounts{"", std::string(33, 'a'), "work.name",
                                                    "work/name", "máin"};
    for (const auto& account : invalid_accounts) {
        INFO(account);
        json dry_run{{"dry_run", true}, {"plan", terminate_plan()}};
        dry_run["plan"]["account"] = account;
        CHECK_THAT(dry_run,
                   !tgcli::test::matches_json_schema("session-terminate.result.schema.json"));

        auto confirmation = terminal_error(
            "CONFIRMATION_REQUIRED",
            {{"account", "main"}, {"action", "session_terminate"}, {"target", terminate_plan()}});
        confirmation["error"]["details"]["target"]["account"] = account;
        CHECK_THAT(confirmation, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    const auto result_schema =
        tgcli::test::load_schema_document("session-terminate.result.schema.json");
    CHECK(result_schema["$defs"]["plan"]["properties"]["account"]["$ref"] == "#/$defs/account");
    const auto error_schema = tgcli::test::load_schema_document("session.error.schema.json");
    CHECK(error_schema["$defs"]["plan"]["properties"]["account"]["$ref"] == "#/$defs/account");
}

TEST_CASE("session AUDIT_INCOMPLETE admits every legal v1 and v2 history-state pair",
          "[schema][session][error]") {
    struct HistoryCase {
        std::string mutation_state;
        json completed_stages;
    };

    const std::vector<HistoryCase> legal{
        {"none", json::array({"intent_synced"})},
        {"possible", json::array({"intent_synced", "logout_send_started"})},
        {"confirmed",
         json::array({"intent_synced", "logout_send_started", "logout_closed_confirmed"})},

        {"none", json::array()},
        {"none", json::array({"idempotency_pending"})},
        {"none", json::array({"spool_ready"})},
        {"none", json::array({"idempotency_pending", "spool_ready"})},
        {"none", json::array({"dispatch_started", "forward_progress"})},
        {"none", json::array({"dispatch_started", "temporary_ids_observed", "forward_progress"})},
        {"none", json::array({"idempotency_pending", "dispatch_started", "forward_progress"})},
        {"none", json::array({"idempotency_pending", "dispatch_started", "temporary_ids_observed",
                              "forward_progress"})},

        {"possible", json::array({"dispatch_started"})},
        {"possible", json::array({"dispatch_started", "temporary_ids_observed"})},
        {"possible", json::array({"idempotency_pending", "dispatch_started"})},
        {"possible",
         json::array({"idempotency_pending", "dispatch_started", "temporary_ids_observed"})},
        {"possible", json::array({"spool_ready", "dispatch_started"})},
        {"possible", json::array({"spool_ready", "dispatch_started", "temporary_ids_observed"})},
        {"possible", json::array({"idempotency_pending", "spool_ready", "dispatch_started"})},
        {"possible", json::array({"idempotency_pending", "spool_ready", "dispatch_started",
                                  "temporary_ids_observed"})},
        {"possible", json::array({"dispatch_started", "forward_progress"})},
        {"possible",
         json::array({"dispatch_started", "temporary_ids_observed", "forward_progress"})},
        {"possible", json::array({"idempotency_pending", "dispatch_started", "forward_progress"})},
        {"possible", json::array({"idempotency_pending", "dispatch_started",
                                  "temporary_ids_observed", "forward_progress"})},

        {"confirmed", json::array({"dispatch_started", "mutation_confirmed"})},
        {"confirmed",
         json::array({"dispatch_started", "temporary_ids_observed", "mutation_confirmed"})},
        {"confirmed",
         json::array({"idempotency_pending", "dispatch_started", "mutation_confirmed"})},
        {"confirmed", json::array({"idempotency_pending", "dispatch_started",
                                   "temporary_ids_observed", "mutation_confirmed"})},
        {"confirmed", json::array({"spool_ready", "dispatch_started", "mutation_confirmed"})},
        {"confirmed", json::array({"spool_ready", "dispatch_started", "temporary_ids_observed",
                                   "mutation_confirmed"})},
        {"confirmed", json::array({"idempotency_pending", "spool_ready", "dispatch_started",
                                   "mutation_confirmed"})},
        {"confirmed", json::array({"idempotency_pending", "spool_ready", "dispatch_started",
                                   "temporary_ids_observed", "mutation_confirmed"})},
        {"confirmed", json::array({"dispatch_started", "forward_progress"})},
        {"confirmed",
         json::array({"dispatch_started", "temporary_ids_observed", "forward_progress"})},
        {"confirmed", json::array({"idempotency_pending", "dispatch_started", "forward_progress"})},
        {"confirmed", json::array({"idempotency_pending", "dispatch_started",
                                   "temporary_ids_observed", "forward_progress"})},
        {"confirmed", json::array({"dispatch_started", "forward_progress", "mutation_confirmed"})},
        {"confirmed", json::array({"dispatch_started", "temporary_ids_observed", "forward_progress",
                                   "mutation_confirmed"})},
        {"confirmed", json::array({"idempotency_pending", "dispatch_started", "forward_progress",
                                   "mutation_confirmed"})},
        {"confirmed",
         json::array({"idempotency_pending", "dispatch_started", "temporary_ids_observed",
                      "forward_progress", "mutation_confirmed"})},
    };

    for (const auto& history : legal) {
        INFO(history.mutation_state << " " << history.completed_stages.dump());
        CHECK_THAT(audit_incomplete(history.mutation_state, history.completed_stages),
                   tgcli::test::matches_json_schema("session.error.schema.json"));
    }
}

TEST_CASE("session AUDIT_INCOMPLETE rejects illegal histories and inconsistent states",
          "[schema][session][error]") {
    struct HistoryCase {
        std::string mutation_state;
        json completed_stages;
    };
    const std::vector<HistoryCase> invalid{
        {"none", json::array({"dispatch_started"})},
        {"possible", json::array()},
        {"confirmed", json::array()},
        {"confirmed", json::array({"mutation_confirmed", "dispatch_started"})},
        {"possible", json::array({"logout_send_started", "dispatch_started"})},
        {"possible", json::array({"dispatch_started", "dispatch_started"})},
        {"possible", json::array({"temporary_ids_observed"})},
        {"possible", json::array({"planned"})},
        {"confirmed", json::array({"intent_synced"})},
        {"possible", json::array({"intent_synced", "logout_closed_confirmed"})},
        {"none", json::array({"spool_ready", "idempotency_pending"})},
        {"possible", json::array({"spool_ready", "temporary_ids_observed"})},
        {"confirmed",
         json::array({"dispatch_started", "forward_progress", "temporary_ids_observed"})},
        {"confirmed", json::array({"dispatch_started", "mutation_confirmed", "forward_progress"})},
        {"possible", json::array({"dispatch_started", "idempotency_pending"})},
        {"none", json::array({"dispatch_started", "mutation_confirmed"})},
        {"possible", json::array({"dispatch_started", "mutation_confirmed"})},
    };

    for (const auto& history : invalid) {
        INFO(history.mutation_state << " " << history.completed_stages.dump());
        CHECK_THAT(audit_incomplete(history.mutation_state, history.completed_stages),
                   !tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    auto additional = audit_incomplete("possible", json::array({"dispatch_started"}));
    additional["error"]["details"]["unexpected"] = true;
    CHECK_THAT(additional, !tgcli::test::matches_json_schema("session.error.schema.json"));
}

TEST_CASE("session SPOOL_UNAVAILABLE admits only the operation and root-reason cross-product",
          "[schema][session][error]") {
    const std::vector<std::string> operations{"session_list", "session_terminate"};
    const std::vector<std::string> reasons{"path_invalid", "wrong_type",  "wrong_owner",
                                           "wrong_mode",   "open_failed", "read_failed"};
    for (const auto& operation : operations) {
        for (const auto& reason : reasons) {
            INFO(operation << " " << reason);
            CHECK_THAT(spool_unavailable(operation, reason),
                       tgcli::test::matches_json_schema("session.error.schema.json"));
        }
    }

    for (const auto* operation : {"send", "msg_forward", "future_session_operation"}) {
        INFO(operation);
        CHECK_THAT(spool_unavailable(operation, "wrong_mode"),
                   !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
    for (const auto* reason :
         {"sync_failed", "capacity_exhausted", "contradiction", "future_reason"}) {
        INFO(reason);
        CHECK_THAT(spool_unavailable("session_list", reason),
                   !tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    for (const auto& invalid_path :
         std::vector<json>{"spool",
                           "/spool",
                           "/state/spool/",
                           {{"kind", "bytes_hex"}, {"value", "2f73706f6f6c"}},
                           nullptr}) {
        INFO(invalid_path.dump());
        auto error = spool_unavailable("session_list", "wrong_mode");
        error["error"]["details"]["path"] = invalid_path;
        CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    auto invalid = spool_unavailable("session_list", "wrong_mode");
    invalid["error"]["message"] = "spool failed";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    for (const auto* field : {"code", "message", "details"}) {
        INFO(field);
        invalid = spool_unavailable("session_list", "wrong_mode");
        invalid["error"].erase(field);
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
    for (const auto* field : {"operation", "path", "reason"}) {
        INFO(field);
        invalid = spool_unavailable("session_list", "wrong_mode");
        invalid["error"]["details"].erase(field);
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
    for (const auto* field : {"operation", "reason"}) {
        INFO(field);
        invalid = spool_unavailable("session_list", "wrong_mode");
        invalid["error"]["details"][field] = 1;
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
    invalid = spool_unavailable("session_list", "wrong_mode");
    invalid["error"]["details"]["unexpected"] = true;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = spool_unavailable("session_list", "wrong_mode");
    invalid["error"]["unexpected"] = true;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = terminal_error(
        "SPOOL_FUTURE",
        {{"operation", "session_list"}, {"path", "spool/"}, {"reason", "wrong_mode"}},
        "attachment spool is unavailable");
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
}

TEST_CASE("session spool AUDIT_INCOMPLETE requires a reversible absolute byte path",
          "[schema][session][error]") {
    CHECK_THAT(spool_audit_incomplete(),
               tgcli::test::matches_json_schema("session.error.schema.json"));
    CHECK_THAT(spool_audit_incomplete({{"kind", "bytes_hex"}, {"value", "2fff"}}),
               tgcli::test::matches_json_schema("session.error.schema.json"));

    for (const auto* encoding : {"2f", "61", "2f6", "2fgg", "2fFF", "2f6100", "2f00"}) {
        INFO(encoding);
        CHECK_THAT(spool_audit_incomplete({{"kind", "bytes_hex"}, {"value", encoding}}),
                   !tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    auto invalid = spool_audit_incomplete({{"kind", "path"}, {"value", "2f61"}});
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = spool_audit_incomplete({{"value", "2f61"}});
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = spool_audit_incomplete({{"kind", "bytes_hex"}});
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid =
        spool_audit_incomplete({{"kind", "bytes_hex"}, {"value", "2f61"}, {"unexpected", true}});
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    for (const auto* mutation_state : {"possible", "confirmed", "future"}) {
        INFO(mutation_state);
        invalid = spool_audit_incomplete();
        invalid["error"]["details"]["mutation_state"] = mutation_state;
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
    for (const auto& completed_stages :
         std::vector<json>{json::array({"dispatch_started"}), json::array({nullptr}), "none"}) {
        INFO(completed_stages.dump());
        invalid = spool_audit_incomplete();
        invalid["error"]["details"]["completed_stages"] = completed_stages;
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    invalid = spool_audit_incomplete();
    invalid["error"]["message"] = "audit recovery failed";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    for (const auto* field : {"code", "message", "details"}) {
        INFO(field);
        invalid = spool_audit_incomplete();
        invalid["error"].erase(field);
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
    for (const auto* field : {"account", "path", "mutation_state", "completed_stages"}) {
        INFO(field);
        invalid = spool_audit_incomplete();
        invalid["error"]["details"].erase(field);
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    }
    invalid = spool_audit_incomplete();
    invalid["error"]["details"]["account"] = 1;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = spool_audit_incomplete();
    invalid["error"]["details"]["unexpected"] = true;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
}

TEST_CASE("session error schema admits the exact common and session branches",
          "[schema][session][error]") {
    const std::string snapshot =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
        "dev:1;ino:2;size:3;ctime_ns:4";
    const std::vector<json> common_errors{
        terminal_error("USAGE", json::object()),
        terminal_error("USAGE", {{"argument", "session-id"}, {"reason", "invalid_argument"}}),
        terminal_error("ACCOUNT_NOT_FOUND", {{"account", "main"}}),
        terminal_error("ACCOUNT_MISMATCH",
                       {{"requested_account", "main"}, {"daemon_account", "work"}}),
        terminal_error("CONFIG_INVALID", {{"path", "/config"}, {"reason", "parse_error"}}),
        terminal_error("CONFIG_CONFLICT",
                       {{"path", "/config"}, {"expected", snapshot}, {"current", "missing"}}),
        terminal_error("HOOK_FAILED", {{"hook", "db_key_cmd"}, {"reason", "exit"}, {"status", 1}}),
        terminal_error("NOT_AUTHED",
                       {{"account", "main"}, {"state", "closed"}, {"reason", "not_ready"}}),
        terminal_error("WRITE_DENIED", {{"account", "main"}, {"reason", "no_grant"}}),
        terminal_error(
            "CONFIRMATION_REQUIRED",
            {{"account", "main"}, {"action", "session_terminate"}, {"target", terminate_plan()}}),
        terminal_error("AUDIT_UNAVAILABLE",
                       {{"account", "main"}, {"path", "/audit"}, {"reason", "capacity_exhausted"}}),
        terminal_error("AUDIT_INCOMPLETE",
                       {{"account", "main"},
                        {"path", "/audit"},
                        {"mutation_state", "possible"},
                        {"completed_stages", json::array({"dispatch_started"})}}),
        terminal_error(
            "PROTOCOL_ANSWER_INVALID",
            {{"request_id", std::numeric_limits<std::uint64_t>::max()}, {"reason", "malformed"}}),
        terminal_error("DAEMON_NOT_RUNNING", {{"account", "main"}, {"socket", "/socket"}}),
        terminal_error(
            "DAEMON_CONTROL_FAILED",
            {{"account", "main"}, {"operation", "restart"}, {"reason", "replacement_failed"}}),
        terminal_error("DAEMON_SHUTDOWN", {{"reason", "daemon_shutdown"}}),
    };
    for (const auto& error : common_errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    const std::vector<json> session_errors{
        terminal_error("BOT_UNSUPPORTED", {{"operation", "session_list"}},
                       "session commands require a user account"),
        terminal_error("NOT_FOUND", {{"operation", "session_terminate"}, {"session_id", "0"}},
                       "session not found"),
        terminal_error("PRECONDITION_FAILED",
                       {{"operation", "session_terminate"},
                        {"session_id", "-9223372036854775808"},
                        {"reason", "current_session"}},
                       "the current session cannot be terminated; use tgcli logout"),
        terminal_error("TDLIB_ERROR", {{"operation", "session_list"}, {"tdlib_code", 500}}),
        terminal_error(
            "RATE_LIMITED",
            {{"operation", "session_terminate"}, {"tdlib_code", 429}, {"retry_after", 0}}),
        terminal_error("INTERNAL", {{"operation", "session_list"},
                                    {"reason", "malformed_tdlib_response"},
                                    {"tdlib_type_id", nullptr}}),
        terminal_error("INTERNAL", {{"operation", "session_terminate"},
                                    {"reason", "malformed_tdlib_response"},
                                    {"tdlib_type_id", -123}}),
    };
    for (const auto& error : session_errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("session.error.schema.json"));
    }
}

TEST_CASE("session TIMEOUT branches reject cross-phase states and idempotency claims",
          "[schema][session][error]") {
    const std::vector<json> timeouts{
        terminal_error("TIMEOUT", {{"operation", "session_list"}, {"state", nullptr}}),
        terminal_error("TIMEOUT", {{"operation", "session_terminate"},
                                   {"phase", "preflight"},
                                   {"state", "ready"},
                                   {"outcome", "not_started"},
                                   {"idempotency", "not_requested"}}),
        terminal_error("TIMEOUT", {{"operation", "session_terminate"},
                                   {"phase", "dispatch"},
                                   {"state", "closed"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "not_requested"}}),
    };
    for (const auto& error : timeouts) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("session.error.schema.json"));
    }

    auto invalid = timeouts.at(1);
    invalid["error"]["details"]["outcome"] = "unknown";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = timeouts.at(2);
    invalid["error"]["details"]["outcome"] = "not_started";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = timeouts.at(2);
    invalid["error"]["details"]["idempotency"] = "pending";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
    invalid = timeouts.at(0);
    invalid["error"]["details"]["state"] = "future_state";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("session.error.schema.json"));
}

TEST_CASE("session error branches reject undeclared fields types and future variants",
          "[schema][session][error]") {
    auto error =
        terminal_error("NOT_FOUND", {{"operation", "session_terminate"}, {"session_id", "0"}},
                       "session not found");
    error["unexpected"] = true;
    CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));

    error = terminal_error("NOT_FOUND", {{"operation", "session_terminate"}, {"session_id", "0"}},
                           "different message");
    CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));

    error = terminal_error(
        "RATE_LIMITED", {{"operation", "session_list"}, {"tdlib_code", 429}, {"retry_after", -1}});
    CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));

    error = terminal_error("INTERNAL", {{"operation", "session_list"},
                                        {"reason", "malformed_tdlib_response"},
                                        {"tdlib_type_id", "future"}});
    CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));

    error =
        terminal_error("AUDIT_INCOMPLETE", {{"account", "main"},
                                            {"path", "/audit"},
                                            {"mutation_state", "possible"},
                                            {"completed_stages", json::array({"future_stage"})}});
    CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));

    error = terminal_error(
        "CONFIRMATION_REQUIRED",
        {{"account", "main"}, {"action", "session_terminate"}, {"target", terminate_plan()}});
    error["error"]["details"]["target"]["session"]["ip_address"] = "203.0.113.10";
    CHECK_THAT(error, !tgcli::test::matches_json_schema("session.error.schema.json"));
}
