#include "proto/operation.hpp"
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

void check_schema_node(const json& schema, bool allow_raw_live_object = false) {
    std::vector<const json*> pending{&schema};
    std::size_t open_object_count = 0;
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
            if ((*node)["additionalProperties"] == true) {
                ++open_object_count;
                REQUIRE(allow_raw_live_object);
                REQUIRE(node->contains("type"));
                CHECK((*node)["type"] == "object");
                CHECK((*node)["required"] == json::array({"@type"}));
                REQUIRE((*node)["properties"].is_object());
                CHECK((*node)["properties"].size() == 3);
                CHECK((*node)["properties"]["@type"] ==
                      json{{"type", "string"}, {"pattern", "^[A-Za-z][A-Za-z0-9]{0,127}$"}});
                CHECK((*node)["properties"]["@extra"] == false);
                CHECK((*node)["properties"]["@client_id"] == false);
            } else {
                CHECK((*node)["additionalProperties"] == false);
            }
        }
        for (const auto& [name, child] : node->items()) {
            static_cast<void>(name);
            pending.push_back(&child);
        }
    }
    CHECK(open_object_count == static_cast<std::size_t>(allow_raw_live_object));
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
        {"chats.result.schema.json",
         {{"items", json::array({json{{"id", -1001},
                                      {"title", "Project"},
                                      {"type", "supergroup"},
                                      {"is_bot", false},
                                      {"usernames", json::array({"project"})},
                                      {"is_archived", false},
                                      {"folder_ids", json::array({2})},
                                      {"is_marked_unread", false},
                                      {"unread_count", 3},
                                      {"unread_mention_count", 1},
                                      {"unread_reaction_count", 0},
                                      {"unread_poll_vote_count", 0},
                                      {"last_message",
                                       {{"id", 123},
                                        {"chat_id", -1001},
                                        {"date", "2026-08-05T10:00:00Z"},
                                        {"sender", {{"type", "user"}, {"id", 42}}},
                                        {"is_outgoing", false},
                                        {"topic", {{"kind", "forum"}, {"id", 7}}},
                                        {"type", "text"},
                                        {"text", "experiment result"}}}}})},
          {"next", "cursor-2"}},
         "items",
         true},
        {"unread.result.schema.json",
         {{"items", json::array({json{{"id", -1001},
                                      {"title", "Project"},
                                      {"type", "supergroup"},
                                      {"is_bot", false},
                                      {"is_archived", false},
                                      {"is_marked_unread", false},
                                      {"unread_count", 3},
                                      {"unread_mention_count", 1},
                                      {"unread_reaction_count", 0},
                                      {"unread_poll_vote_count", 0}}})},
          {"next", nullptr}},
         "items",
         true},
        {"version.result.schema.json",
         {{"version", "1.0.0"}, {"protocol", 2}, {"tdlib", "1.8.65"}},
         "version"},
        {"version.result.schema.json",
         {{"version", "1.0.0"}, {"protocol", 2}, {"tdlib", "1.8.65"}, {"commit", "4d7ca6e"}},
         "version"},
        {"daemon-stop.result.schema.json", {{"stopping", true}}, "stopping"},
        {"daemon-status.result.schema.json",
         {{"account", "main"}, {"running", false}, {"socket", "/tmp/tgcli.sock"}},
         "account"},
        {"daemon-status.result.schema.json",
         {{"account", "main"},
          {"running", true},
          {"pid", 123},
          {"version", "1.0.0"},
          {"protocol", 1},
          {"socket", "/tmp/tgcli.sock"}},
         "account"},
        {"daemon-restart.result.schema.json",
         {{"account", "main"},
          {"restarted", true},
          {"pid", 124},
          {"version", "1.0.0"},
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
        {"msg-get.result.schema.json",
         {{"items", json::array({json{{"id", 123},
                                      {"chat_id", -1001},
                                      {"date", "2026-08-05T10:00:00Z"},
                                      {"sender", {{"type", "user"}, {"id", 42}}},
                                      {"is_outgoing", false},
                                      {"topic", {{"kind", "forum"}, {"id", 7}}},
                                      {"type", "text"},
                                      {"text", "message or caption"}}})},
          {"next", nullptr}},
         "items",
         true},
        {"msg-link.result.schema.json",
         {{"chat_id", -1001},
          {"message_id", 123},
          {"link", "urn:telegram:message"},
          {"is_public", false}},
         "link"},
        {"read.result.schema.json",
         {{"items", json::array({json{{"id", 123},
                                      {"chat_id", -1001},
                                      {"date", "2026-08-05T10:00:00Z"},
                                      {"sender", {{"type", "user"}, {"id", 42}}},
                                      {"is_outgoing", false},
                                      {"topic", {{"kind", "forum"}, {"id", 7}}},
                                      {"type", "text"},
                                      {"text", "message or caption"}}})},
          {"next", "cursor-2"},
          {"boundary", "page"}},
         "items",
         true},
        {"fetch.result.schema.json",
         {{"chat_id", -1001},
          {"cached_count", 250},
          {"oldest_message_id", 123},
          {"target", {{"limit", 1000}, {"all", false}, {"since", nullptr}}},
          {"target_reached", false},
          {"stop_reason", "tdlib_idle"},
          {"resume_from_message_id", 123}},
         "chat_id",
         true},
        {"doctor.result.schema.json",
         {{"account", "main"},
          {"daemon",
           {{"running", true},
            {"in_process", false},
            {"pid", 123},
            {"version", "1.0.0"},
            {"socket", "/tmp/tgcli.sock"}}},
          {"tdlib", {{"version", "1.8.65"}}},
          {"auth", {{"state", "unknown"}}}},
         "account",
         true},
        {"doctor.result.schema.json",
         {{"account", "main"},
          {"daemon",
           {{"running", false}, {"in_process", true}, {"pid", 123}, {"version", "1.0.0"}}},
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
    json expected{{"schemaDialect", kDialect},
                  {"commands",
                   {{"account add", {{"result", "account-add.result.schema.json"}}},
                    {"account list", {{"result", "account-list.result.schema.json"}}},
                    {"account remove", {{"result", "account-remove.result.schema.json"}}},
                    {"account show", {{"result", "account-show.result.schema.json"}}},
                    {"account use", {{"result", "account-use.result.schema.json"}}},
                    {"chat archive", {{"result", "chat-archive.result.schema.json"}}},
                    {"chat info", {{"result", "chat-info.result.schema.json"}}},
                    {"chat join", {{"result", "chat-join.result.schema.json"}}},
                    {"chat leave", {{"result", "chat-leave.result.schema.json"}}},
                    {"chat mark-read", {{"result", "chat-mark-read.result.schema.json"}}},
                    {"chat members", {{"result", "chat-members.result.schema.json"}}},
                    {"chat mute", {{"result", "chat-mute.result.schema.json"}}},
                    {"chat pin", {{"result", "chat-pin.result.schema.json"}}},
                    {"chat unarchive", {{"result", "chat-unarchive.result.schema.json"}}},
                    {"chat unmute", {{"result", "chat-unmute.result.schema.json"}}},
                    {"chat unpin", {{"result", "chat-unpin.result.schema.json"}}},
                    {"chats", {{"result", "chats.result.schema.json"}}},
                    {"daemon restart", {{"result", "daemon-restart.result.schema.json"}}},
                    {"daemon status", {{"result", "daemon-status.result.schema.json"}}},
                    {"daemon stop", {{"result", "daemon-stop.result.schema.json"}}},
                    {"doctor", {{"result", "doctor.result.schema.json"}}},
                    {"download", {{"result", "download.result.schema.json"}}},
                    {"fetch", {{"result", "fetch.result.schema.json"}}},
                    {"login", {{"result", "login.result.schema.json"}}},
                    {"logout", {{"result", "logout.result.schema.json"}}},
                    {"me", {{"result", "me.result.schema.json"}}},
                    {"msg delete", {{"result", "msg-delete.result.schema.json"}}},
                    {"msg edit", {{"result", "msg-edit.result.schema.json"}}},
                    {"msg forward", {{"result", "msg-forward.result.schema.json"}}},
                    {"msg get", {{"result", "msg-get.result.schema.json"}}},
                    {"msg link", {{"result", "msg-link.result.schema.json"}}},
                    {"msg pin", {{"result", "msg-pin.result.schema.json"}}},
                    {"msg react", {{"result", "msg-react.result.schema.json"}}},
                    {"msg unpin", {{"result", "msg-unpin.result.schema.json"}}},
                    {"raw", {{"result", "raw.result.schema.json"}}},
                    {"read", {{"result", "read.result.schema.json"}}},
                    {"resolve", {{"result", "resolve.result.schema.json"}}},
                    {"saved attach", {{"result", "saved-attach.result.schema.json"}}},
                    {"saved search", {{"result", "saved-search.result.schema.json"}}},
                    {"saved tags", {{"result", "saved-tags.result.schema.json"}}},
                    {"search", {{"result", "search.result.schema.json"}}},
                    {"send", {{"result", "send.result.schema.json"}}},
                    {"session list", {{"result", "session-list.result.schema.json"}}},
                    {"session terminate", {{"result", "session-terminate.result.schema.json"}}},
                    {"unread", {{"result", "unread.result.schema.json"}}},
                    {"version", {{"result", "version.result.schema.json"}}},
                    {"wait-for", {{"result", "wait-for.result.schema.json"}}}}}};
    for (const auto& identity : tgcli::proto::m6_operation_identities()) {
        std::string filename(identity.command_path);
        std::ranges::replace(filename, ' ', '-');
        expected["commands"][identity.command_path] = {
            {"result", filename + ".result.schema.json"}};
    }
    CHECK(manifest == expected);
    CHECK(manifest["commands"].size() == 77);

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
        check_schema_node(schema, command == "raw");
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
    const auto m3_write_error = tgcli::test::load_schema_document("m3-write.error.schema.json");
    check_schema_node(m3_write_error);
    for (const auto* filename : {"logout.error.schema.json", "account-remove.error.schema.json",
                                 "audit-intent.schema.json", "audit-checkpoint.schema.json",
                                 "audit-outcome.schema.json", "removal-tombstone.schema.json"}) {
        const auto schema = tgcli::test::load_schema_document(filename);
        REQUIRE(schema.contains("$schema"));
        CHECK(schema["$schema"] == kDialect);
        check_schema_node(schema);
    }
}

TEST_CASE("chats schema keeps identity folder and tagged-topic bounds exact", "[schema][chats]") {
    const auto cases = schema_cases();
    const auto found = std::ranges::find_if(cases, [](const SchemaCase& test_case) {
        return test_case.filename == "chats.result.schema.json";
    });
    REQUIRE(found != cases.end());
    auto result = found->instance;
    CHECK_THAT(result, tgcli::test::matches_json_schema("chats.result.schema.json"));

    result["items"][0]["folder_ids"] = json::array({2, 2});
    CHECK_THAT(result, !tgcli::test::matches_json_schema("chats.result.schema.json"));

    result = found->instance;
    result["items"][0]["last_message"]["topic"] = json{{"kind", "forum"}, {"id", 2147483648LL}};
    CHECK_THAT(result, !tgcli::test::matches_json_schema("chats.result.schema.json"));

    result["items"][0]["last_message"]["topic"]["kind"] = "thread";
    CHECK_THAT(result, tgcli::test::matches_json_schema("chats.result.schema.json"));

    for (const bool is_bot : {false, true}) {
        result = found->instance;
        result["items"][0].update({{"id", 42}, {"type", "private"}, {"is_bot", is_bot}});
        CHECK_THAT(result, tgcli::test::matches_json_schema("chats.result.schema.json"));
    }

    for (const std::string_view type : {"basic_group", "supergroup", "channel"}) {
        result = found->instance;
        result["items"][0]["type"] = type;
        result["items"][0]["is_bot"] = false;
        CHECK_THAT(result, tgcli::test::matches_json_schema("chats.result.schema.json"));

        result["items"][0]["is_bot"] = true;
        CHECK_THAT(result, !tgcli::test::matches_json_schema("chats.result.schema.json"));
    }
}

TEST_CASE("unread schema is unpaginated unbounded and keeps private bot identity exact",
          "[schema][unread]") {
    const auto cases = schema_cases();
    const auto found = std::ranges::find_if(cases, [](const SchemaCase& test_case) {
        return test_case.filename == "unread.result.schema.json";
    });
    REQUIRE(found != cases.end());
    auto result = found->instance;
    CHECK_THAT(result, tgcli::test::matches_json_schema("unread.result.schema.json"));

    for (const bool is_bot : {false, true}) {
        result = found->instance;
        result["items"][0].update({{"id", 42}, {"type", "private"}, {"is_bot", is_bot}});
        CHECK_THAT(result, tgcli::test::matches_json_schema("unread.result.schema.json"));
    }
    for (const std::string_view type : {"basic_group", "supergroup", "channel"}) {
        result = found->instance;
        result["items"][0]["type"] = type;
        result["items"][0]["is_bot"] = true;
        CHECK_THAT(result, !tgcli::test::matches_json_schema("unread.result.schema.json"));
    }

    result = found->instance;
    result["next"] = "cursor";
    CHECK_THAT(result, !tgcli::test::matches_json_schema("unread.result.schema.json"));
    result = found->instance;
    result["items"][0]["unread_count"] = -1;
    CHECK_THAT(result, !tgcli::test::matches_json_schema("unread.result.schema.json"));
    result = found->instance;
    result["items"][0]["usernames"] = json::array();
    CHECK_THAT(result, !tgcli::test::matches_json_schema("unread.result.schema.json"));

    result = found->instance;
    const auto item = result["items"][0];
    result["items"] = json::array();
    for (std::size_t index = 0; index < 101; ++index) {
        auto next = item;
        next["id"] = static_cast<std::int64_t>(index + 1);
        result["items"].push_back(std::move(next));
    }
    CHECK_THAT(result, tgcli::test::matches_json_schema("unread.result.schema.json"));
}

TEST_CASE("read schema binds page cursors to progress and terminal boundaries to null",
          "[schema][read]") {
    const auto cases = schema_cases();
    const auto found = std::ranges::find_if(cases, [](const SchemaCase& test_case) {
        return test_case.filename == "read.result.schema.json";
    });
    REQUIRE(found != cases.end());
    auto result = found->instance;
    CHECK_THAT(result, tgcli::test::matches_json_schema("read.result.schema.json"));

    result["next"] = nullptr;
    CHECK_THAT(result, !tgcli::test::matches_json_schema("read.result.schema.json"));
    result = found->instance;
    result["boundary"] = "tdlib_idle";
    CHECK_THAT(result, !tgcli::test::matches_json_schema("read.result.schema.json"));
    result["next"] = nullptr;
    CHECK_THAT(result, tgcli::test::matches_json_schema("read.result.schema.json"));
    result["items"] = json::array();
    CHECK_THAT(result, tgcli::test::matches_json_schema("read.result.schema.json"));
    result["extra"] = true;
    CHECK_THAT(result, !tgcli::test::matches_json_schema("read.result.schema.json"));

    result = found->instance;
    const auto item = result["items"][0];
    result["items"] = json::array();
    for (std::size_t index = 0; index < 101; ++index) {
        auto next = item;
        next["id"] = static_cast<std::int64_t>(index + 1);
        result["items"].push_back(std::move(next));
    }
    CHECK_THAT(result, !tgcli::test::matches_json_schema("read.result.schema.json"));
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

TEST_CASE("public M3 write errors close resolver durability and timeout branches",
          "[schema][m3][send][delete][error]") {
    const auto hash = std::string("sha256:") + std::string(64, 'a');
    const auto other_hash = std::string("sha256:") + std::string(64, 'b');
    const std::vector<json> errors{
        terminal_error("USAGE", {{"argument", "--topic"}, {"reason", "unsupported_topic_kind"}}),
        terminal_error("BOT_UNSUPPORTED", {{"operation", "send"}}),
        terminal_error("NOT_FOUND", {{"chat_id", -1001}, {"message_id", -7}}),
        terminal_error("AMBIGUOUS",
                       {{"selector", "Project"},
                        {"scope", "active_dialogs"},
                        {"candidates", json::array({json{{"id", -1001},
                                                         {"title", "Project"},
                                                         {"type", "basic_group"},
                                                         {"is_bot", false},
                                                         {"usernames", json::array()}}})},
                        {"truncated", false}}),
        terminal_error("RATE_LIMITED",
                       {{"operation", "resolve"}, {"tdlib_code", 429}, {"retry_after", 7}}),
        terminal_error("TDLIB_ERROR", {{"operation", "send"}, {"tdlib_code", 400}}),
        terminal_error("PRECONDITION_FAILED", {{"operation", "send"},
                                               {"chat_id", -1001},
                                               {"message_id", nullptr},
                                               {"reason", "schedule_window_elapsed"}}),
        terminal_error("IDEMPOTENCY_CONFLICT", {{"operation", "send"},
                                                {"key_hash", hash},
                                                {"expected_fingerprint", hash},
                                                {"actual_fingerprint", other_hash}}),
        terminal_error("IDEMPOTENCY_PENDING",
                       {{"operation", "send"},
                        {"key_hash", hash},
                        {"fingerprint", other_hash},
                        {"invocation_id", "0123456789abcdef0123456789abcdef"},
                        {"temporary_message_ids", json::array({-7})}}),
        terminal_error(
            "AUDIT_UNAVAILABLE",
            {{"account", "main"}, {"path", "/state/main/audit.log"}, {"reason", "sync_failed"}}),
        terminal_error("AUDIT_INCOMPLETE",
                       {{"account", "main"},
                        {"path", "/state/main/audit.log"},
                        {"mutation_state", "possible"},
                        {"completed_stages", json::array({"idempotency_pending", "dispatch_started",
                                                          "temporary_ids_observed"})}}),
        json{{"error",
              {{"code", "AUDIT_INCOMPLETE"},
               {"message", "attachment spool recovery is incomplete"},
               {"details",
                {{"account", "main"},
                 {"path", {{"kind", "bytes_hex"}, {"value", "2f73746174652fff"}}},
                 {"mutation_state", "none"},
                 {"completed_stages", json::array()}}}}}},
        terminal_error("TIMEOUT", {{"operation", "send"},
                                   {"phase", "preflight"},
                                   {"state", "ready"},
                                   {"outcome", "not_started"},
                                   {"idempotency", "removed"}}),
        terminal_error("TIMEOUT", {{"operation", "send"},
                                   {"phase", "confirmation"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "pending"},
                                   {"temporary_message_id", -7}}),
        terminal_error("TIMEOUT", {{"operation", "msg_delete"},
                                   {"phase", "dispatch"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "not_requested"}}),
        terminal_error("TIMEOUT", {{"operation", "config_admission"}, {"state", nullptr}}),
    };
    for (const auto& error : errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    }

    auto invalid = errors[4];
    invalid["error"]["details"].erase("retry_after");
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[5];
    invalid["error"]["details"]["retry_after"] = 1;
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[12];
    invalid["error"]["details"]["outcome"] = "unknown";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[13];
    invalid["error"]["details"].erase("temporary_message_id");
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    auto saved_incomplete = errors[10];
    saved_incomplete["error"]["details"]["completed_stages"] =
        json::array({"idempotency_pending", "spool_ready", "dispatch_started"});
    CHECK_THAT(saved_incomplete, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = saved_incomplete;
    invalid["error"]["details"]["completed_stages"].push_back("spool_ready");
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[11];
    invalid["error"]["details"]["path"]["value"] = "2f00";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[6];
    invalid["error"]["details"]["reason"] = "not_deletable_for_self";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
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

TEST_CASE("saved attachment errors are strict public M4 branches",
          "[schema][saved-attach][error]") {
    const auto exact_error = [](std::string code, std::string message, json details) {
        return json{{"error",
                     {{"code", std::move(code)},
                      {"message", std::move(message)},
                      {"details", std::move(details)}}}};
    };
    const std::vector<json> errors{
        exact_error("BOT_UNSUPPORTED", "saved commands require a user account", json::object()),
        exact_error(
            "NOT_FOUND", "input file is unavailable",
            {{"operation", "saved_attach"}, {"path", "/private/input.bin"}, {"reason", "missing"}}),
        exact_error("INPUT_CHANGED", "input file changed while being read",
                    {{"operation", "saved_attach"}, {"path", "/private/input.bin"}}),
        exact_error("SPOOL_UNAVAILABLE", "attachment spool is unavailable",
                    {{"operation", "saved_attach"},
                     {"path", "/private/input.bin"},
                     {"reason", "sync_failed"}}),
        terminal_error("PRECONDITION_FAILED", {{"operation", "saved_attach"},
                                               {"chat_id", 42},
                                               {"message_id", -77},
                                               {"reason", "wrong_topic"}}),
        terminal_error("PRECONDITION_FAILED", {{"operation", "saved_attach"},
                                               {"chat_id", 42},
                                               {"message_id", -77},
                                               {"reason", "not_replyable"}}),
        terminal_error("TIMEOUT", {{"operation", "saved_attach"},
                                   {"phase", "confirmation"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "pending"},
                                   {"temporary_message_id", -7}}),
        terminal_error("SEND_FAILED", {{"operation", "saved_attach"},
                                       {"chat_id", 42},
                                       {"temporary_message_id", -7},
                                       {"reason", "deleted_before_confirmation"}}),
        terminal_error("TDLIB_ERROR", {{"operation", "saved_attach"}, {"tdlib_code", 400}}),
    };
    for (const auto& error : errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    }

    auto invalid = errors[1];
    invalid["error"]["message"] = "message was not found";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[2];
    invalid["error"]["details"]["path"] = "relative";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[3];
    invalid["error"]["details"]["reason"] = "unreadable";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[4];
    invalid["error"]["details"]["reason"] = "not_forwardable";
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid = errors[6];
    invalid["error"]["details"].erase("temporary_message_id");
    CHECK_THAT(invalid, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
}

TEST_CASE("direct message write schemas pair exact result plans and operation errors",
          "[schema][m3][message-write]") {
    const json chat{{"id", -1001},
                    {"title", "Project"},
                    {"type", "supergroup"},
                    {"is_bot", false},
                    {"usernames", json::array({"project"})}};
    const json message{{"id", 101},
                       {"chat_id", -1001},
                       {"date", "2026-08-05T10:00:00Z"},
                       {"sender", {{"type", "user"}, {"id", 42}}},
                       {"is_outgoing", true},
                       {"topic", nullptr},
                       {"type", "text"},
                       {"text", "revised"},
                       {"scheduled", false}};
    CHECK_THAT(message, tgcli::test::matches_json_schema("msg-edit.result.schema.json"));
    auto invalid_message = message;
    invalid_message["scheduled"] = true;
    CHECK_THAT(invalid_message, !tgcli::test::matches_json_schema("msg-edit.result.schema.json"));
    invalid_message = message;
    invalid_message["type"] = "photo";
    CHECK_THAT(invalid_message, !tgcli::test::matches_json_schema("msg-edit.result.schema.json"));

    const json edit_dry{{"dry_run", true},
                        {"plan",
                         {{"operation", "msg_edit"},
                          {"account", "main"},
                          {"tdlib_request", "editMessageText"},
                          {"chat", chat},
                          {"message_id", 101},
                          {"text", "revised"}}}};
    CHECK_THAT(edit_dry, tgcli::test::matches_json_schema("msg-edit.result.schema.json"));
    auto invalid_dry = edit_dry;
    invalid_dry["plan"]["tdlib_request"] = "sendMessage";
    CHECK_THAT(invalid_dry, !tgcli::test::matches_json_schema("msg-edit.result.schema.json"));

    const json reaction{{"chat_id", -1001},
                        {"message_id", 101},
                        {"reaction", "👍"},
                        {"removed", false},
                        {"big", true}};
    CHECK_THAT(reaction, tgcli::test::matches_json_schema("msg-react.result.schema.json"));
    auto invalid_reaction = reaction;
    invalid_reaction["removed"] = true;
    CHECK_THAT(invalid_reaction, !tgcli::test::matches_json_schema("msg-react.result.schema.json"));

    const json pinned_result{{"chat_id", -1001}, {"message_id", 101}, {"pinned", true}};
    const json unpinned_result{{"chat_id", -1001}, {"message_id", 101}, {"pinned", false}};
    CHECK_THAT(pinned_result, tgcli::test::matches_json_schema("msg-pin.result.schema.json"));
    CHECK_THAT(unpinned_result, tgcli::test::matches_json_schema("msg-unpin.result.schema.json"));
    CHECK_THAT(unpinned_result, !tgcli::test::matches_json_schema("msg-pin.result.schema.json"));

    const std::vector<json> errors{
        terminal_error("PRECONDITION_FAILED", {{"operation", "msg_edit"},
                                               {"chat_id", -1001},
                                               {"message_id", 101},
                                               {"reason", "not_editable"}}),
        terminal_error("PRECONDITION_FAILED", {{"operation", "msg_react"},
                                               {"chat_id", -1001},
                                               {"message_id", 101},
                                               {"reason", "reaction_unavailable"}}),
        terminal_error("PRECONDITION_FAILED", {{"operation", "msg_pin"},
                                               {"chat_id", -1001},
                                               {"message_id", 101},
                                               {"reason", "not_pinnable"}}),
        terminal_error("TIMEOUT", {{"operation", "msg_unpin"},
                                   {"phase", "dispatch"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "pending"}}),
        terminal_error("BOT_UNSUPPORTED", {{"operation", "msg_react"}})};
    for (const auto& error : errors) {
        CHECK_THAT(error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    }
    auto invalid_error = errors.front();
    invalid_error["error"]["details"]["reason"] = "reaction_unavailable";
    CHECK_THAT(invalid_error, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
}

TEST_CASE("msg forward schemas share one strict ordered item contract", "[schema][m3][forward]") {
    const json chat{{"id", -1001},
                    {"title", "Project"},
                    {"type", "supergroup"},
                    {"is_bot", false},
                    {"usernames", json::array({"project"})}};
    const json destination{{"id", -1002},
                           {"title", "Destination"},
                           {"type", "supergroup"},
                           {"is_bot", false},
                           {"usernames", json::array({"destination"})}};
    const json message{{"id", 101},
                       {"chat_id", -1002},
                       {"date", "2026-08-05T10:00:00Z"},
                       {"sender", {{"type", "user"}, {"id", 42}}},
                       {"is_outgoing", true},
                       {"topic", nullptr},
                       {"type", "text"},
                       {"text", "forwarded"},
                       {"scheduled", false}};
    const json sent{{"source_id", 1}, {"status", "sent"}, {"message", message}};
    const json pending{{"source_id", 2}, {"status", "pending"}, {"temporary_message_id", -2}};
    const json failed{{"source_id", 2},
                      {"status", "failed"},
                      {"failure_reason", "upstream_null"},
                      {"tdlib_code", nullptr},
                      {"retry_after", nullptr}};
    const json rate_failed{{"source_id", 2},
                           {"status", "failed"},
                           {"failure_reason", "tdlib_error"},
                           {"tdlib_code", 429},
                           {"retry_after", 4}};
    const json result{
        {"from_chat_id", -1001}, {"to_chat_id", -1002}, {"items", json::array({sent})}};
    CHECK_THAT(result, tgcli::test::matches_json_schema("msg-forward.result.schema.json"));
    auto invalid_result = result;
    invalid_result["items"][0] = failed;
    CHECK_THAT(invalid_result, !tgcli::test::matches_json_schema("msg-forward.result.schema.json"));

    const json dry{{"dry_run", true},
                   {"plan",
                    {{"operation", "msg_forward"},
                     {"account", "main"},
                     {"tdlib_request", "forwardMessages"},
                     {"from", chat},
                     {"to", destination},
                     {"message_ids", json::array({1, 2})},
                     {"drop_author", false}}}};
    CHECK_THAT(dry, tgcli::test::matches_json_schema("msg-forward.result.schema.json"));

    const std::vector<json> errors{
        terminal_error("PRECONDITION_FAILED", {{"operation", "msg_forward"},
                                               {"chat_id", -1001},
                                               {"message_id", 1},
                                               {"reason", "not_forwardable"}}),
        terminal_error("FORWARD_PARTIAL", {{"operation", "msg_forward"},
                                           {"from_chat_id", -1001},
                                           {"to_chat_id", -1002},
                                           {"items", json::array({sent, failed})}}),
        terminal_error("FORWARD_FAILED", {{"operation", "msg_forward"},
                                          {"from_chat_id", -1001},
                                          {"to_chat_id", -1002},
                                          {"items", json::array({failed})}}),
        terminal_error("RATE_LIMITED", {{"operation", "msg_forward"},
                                        {"tdlib_code", 429},
                                        {"retry_after", 4},
                                        {"items", json::array({rate_failed})}}),
        terminal_error("RATE_LIMITED", {{"operation", "msg_forward"},
                                        {"tdlib_code", 429},
                                        {"retry_after", 4},
                                        {"items", json::array()}}),
        terminal_error("TIMEOUT", {{"operation", "msg_forward"},
                                   {"phase", "confirmation"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "pending"},
                                   {"items", json::array()}}),
        terminal_error("TIMEOUT", {{"operation", "msg_forward"},
                                   {"phase", "confirmation"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "pending"},
                                   {"items", json::array({pending})}}),
        terminal_error("TIMEOUT", {{"operation", "msg_forward"},
                                   {"phase", "confirmation"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "pending"},
                                   {"items", json::array({sent, pending})}})};
    for (const auto& error : errors) {
        INFO(error.dump());
        CHECK_THAT(error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    }
    auto invalid_error = errors[1];
    invalid_error["error"]["details"]["items"] = json::array({sent});
    CHECK_THAT(invalid_error, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    invalid_error = errors[3];
    invalid_error["error"]["details"]["items"] = json::array({sent});
    CHECK_THAT(invalid_error, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    const auto complete_timeout =
        terminal_error("TIMEOUT", {{"operation", "msg_forward"},
                                   {"phase", "confirmation"},
                                   {"state", "ready"},
                                   {"outcome", "unknown"},
                                   {"idempotency", "pending"},
                                   {"items", json::array({sent, failed})}});
    CHECK_THAT(complete_timeout, !tgcli::test::matches_json_schema("m3-write.error.schema.json"));
}

TEST_CASE("chat write schemas pair exact real and dry-run relations", "[schema][m3][chat-write]") {
    const json chat{{"id", -1001},
                    {"title", "Project"},
                    {"type", "supergroup"},
                    {"is_bot", false},
                    {"usernames", json::array({"project"})}};
    struct Case {
        std::string schema;
        json result;
        json plan;
    };
    const std::vector<Case> cases{
        {"chat-mark-read.result.schema.json",
         {{"chat_id", -1001}, {"last_read_message_id", nullptr}, {"marked_read", true}},
         {{"operation", "chat_mark_read"},
          {"account", "main"},
          {"tdlib_request", nullptr},
          {"chat", chat},
          {"last_message_id", nullptr}}},
        {"chat-mute.result.schema.json",
         {{"chat_id", -1001}, {"muted", true}, {"duration_seconds", 3600}},
         {{"operation", "chat_mute"},
          {"account", "main"},
          {"tdlib_request", "setChatNotificationSettings"},
          {"chat", chat},
          {"muted", true},
          {"duration_seconds", 3600}}},
        {"chat-unmute.result.schema.json",
         {{"chat_id", -1001}, {"muted", false}, {"duration_seconds", 0}},
         {{"operation", "chat_unmute"},
          {"account", "main"},
          {"tdlib_request", "setChatNotificationSettings"},
          {"chat", chat},
          {"muted", false},
          {"duration_seconds", 0}}},
        {"chat-pin.result.schema.json",
         {{"chat_id", -1001}, {"chat_list", "archive"}, {"pinned", true}},
         {{"operation", "chat_pin"},
          {"account", "main"},
          {"tdlib_request", "toggleChatIsPinned"},
          {"chat", chat},
          {"chat_list", "archive"},
          {"pinned", true}}},
        {"chat-unpin.result.schema.json",
         {{"chat_id", -1001}, {"chat_list", "main"}, {"pinned", false}},
         {{"operation", "chat_unpin"},
          {"account", "main"},
          {"tdlib_request", "toggleChatIsPinned"},
          {"chat", chat},
          {"chat_list", "main"},
          {"pinned", false}}},
        {"chat-archive.result.schema.json",
         {{"chat_id", -1001}, {"archived", true}},
         {{"operation", "chat_archive"},
          {"account", "main"},
          {"tdlib_request", "addChatToList"},
          {"chat", chat},
          {"archived", true}}},
        {"chat-unarchive.result.schema.json",
         {{"chat_id", -1001}, {"archived", false}},
         {{"operation", "chat_unarchive"},
          {"account", "main"},
          {"tdlib_request", "addChatToList"},
          {"chat", chat},
          {"archived", false}}},
        {"chat-join.result.schema.json",
         {{"status", "request_sent"}, {"chat_id", nullptr}},
         {{"operation", "chat_join"},
          {"account", "main"},
          {"tdlib_request", "joinChatByInviteLink"},
          {"source", "invite_link"},
          {"chat", nullptr},
          {"invite_link_sha256",
           "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}},
        {"chat-leave.result.schema.json",
         {{"chat_id", -1001}, {"left", true}},
         {{"operation", "chat_leave"},
          {"account", "main"},
          {"tdlib_request", "leaveChat"},
          {"chat", chat}}},
    };
    for (const auto& test_case : cases) {
        INFO(test_case.schema);
        CHECK_THAT(test_case.result, tgcli::test::matches_json_schema(test_case.schema));
        const json dry{{"dry_run", true}, {"plan", test_case.plan}};
        CHECK_THAT(dry, tgcli::test::matches_json_schema(test_case.schema));
        auto unknown = test_case.result;
        unknown["extra"] = true;
        CHECK_THAT(unknown, !tgcli::test::matches_json_schema(test_case.schema));
    }

    auto wrong_pin = cases[3].result;
    wrong_pin["pinned"] = false;
    CHECK_THAT(wrong_pin, !tgcli::test::matches_json_schema("chat-pin.result.schema.json"));
    auto wrong_join = cases[7].plan;
    wrong_join["source"] = "username";
    const json wrong_join_dry{{"dry_run", true}, {"plan", wrong_join}};
    CHECK_THAT(wrong_join_dry, !tgcli::test::matches_json_schema("chat-join.result.schema.json"));

    const auto saved =
        terminal_error("PRECONDITION_FAILED", {{"operation", "chat_unmute"},
                                               {"chat_id", 42},
                                               {"message_id", nullptr},
                                               {"reason", "saved_notifications_unsupported"}});
    const auto listed = terminal_error("PRECONDITION_FAILED", {{"operation", "chat_pin"},
                                                               {"chat_id", -1001},
                                                               {"message_id", nullptr},
                                                               {"reason", "chat_not_listed"}});
    const auto leave = terminal_error(
        "CONFIRMATION_REQUIRED",
        {{"account", "main"}, {"action", "chat_leave"}, {"target", cases.back().plan}});
    const auto guard =
        terminal_error("JOIN_APPROVAL_REQUIRED",
                       {{"operation", "chat_join"}, {"bot_user_id", 77}, {"query_id", 88}});
    const auto declined = terminal_error("JOIN_DECLINED", {{"operation", "chat_join"}});
    for (const auto& error : {saved, listed, leave, guard, declined}) {
        CHECK_THAT(error, tgcli::test::matches_json_schema("m3-write.error.schema.json"));
    }
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
        "getCurrentState",
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
            {{"account", "main"}, {"operation", "stop"}, {"reason", "surface_invalid"}}}}}},
        {{"error",
          {{"code", "INTERNAL"},
           {"message", "command handler failed"},
           {"details", {{"operation", "stop"}, {"reason", "internal_error"}}}}}}};
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

TEST_CASE("version result schema bounds the optional build revision", "[schema][version]") {
    const json base{{"version", "1.0.0"}, {"protocol", 3}, {"tdlib", "1.8.65"}};
    CHECK_THAT(base, tgcli::test::matches_json_schema("version.result.schema.json"));

    const std::vector<std::string> accepted{
        "4d7ca6e",
        "4d7ca6e-dirty",
        "4d7ca6ed9b8a",
        "4d7ca6ed9b8a5c1f0e3d2c4b6a8f9e7d1c3b5a70",
        std::string(64, 'a'),
        std::string(64, 'a') + "-dirty",
    };
    for (const auto& value : accepted) {
        auto instance = base;
        instance["commit"] = value;
        INFO("commit: " << value);
        CHECK_THAT(instance, tgcli::test::matches_json_schema("version.result.schema.json"));
    }

    for (const auto* rejected : {"", "4d7ca6", "4D7CA6E", "4d7ca6e-DIRTY", "4d7ca6e-modified",
                                 "g4d7ca6e", "-dirty", " 4d7ca6e", "4d7ca6e ", "4d7ca6e\n"}) {
        auto instance = base;
        instance["commit"] = rejected;
        INFO("commit: " << rejected);
        CHECK_THAT(instance, !tgcli::test::matches_json_schema("version.result.schema.json"));
    }

    auto null_commit = base;
    null_commit["commit"] = nullptr;
    CHECK_THAT(null_commit, !tgcli::test::matches_json_schema("version.result.schema.json"));

    for (const auto& wrong_type :
         std::vector<json>{json(7), json(true), json::array(), json::object()}) {
        auto instance = base;
        instance["commit"] = wrong_type;
        CHECK_THAT(instance, !tgcli::test::matches_json_schema("version.result.schema.json"));
    }
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
