#include "schema_matcher.hpp"

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
    };
}

} // namespace

TEST_CASE("schema manifest is an exact command-to-result bijection", "[schema]") {
    const auto manifest = tgcli::test::load_schema_document("manifest.json");
    const json expected{{"schemaDialect", kDialect},
                        {"commands",
                         {{"account add", {{"result", "account-add.result.schema.json"}}},
                          {"account list", {{"result", "account-list.result.schema.json"}}},
                          {"account show", {{"result", "account-show.result.schema.json"}}},
                          {"account use", {{"result", "account-use.result.schema.json"}}},
                          {"daemon restart", {{"result", "daemon-restart.result.schema.json"}}},
                          {"daemon status", {{"result", "daemon-status.result.schema.json"}}},
                          {"daemon stop", {{"result", "daemon-stop.result.schema.json"}}},
                          {"doctor", {{"result", "doctor.result.schema.json"}}},
                          {"login", {{"result", "login.result.schema.json"}}},
                          {"logout", {{"result", "logout.result.schema.json"}}},
                          {"me", {{"result", "me.result.schema.json"}}},
                          {"version", {{"result", "version.result.schema.json"}}}}}};
    CHECK(manifest == expected);
    CHECK(manifest["commands"].size() == 12);

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

    auto show_credentials_unknown = schema_cases().at(2).instance;
    show_credentials_unknown["credentials"]["unexpected"] = true;
    CHECK_THAT(show_credentials_unknown,
               !tgcli::test::matches_json_schema("account-show.result.schema.json"));

    auto show_paths_unknown = schema_cases().at(2).instance;
    show_paths_unknown["paths"]["unexpected"] = true;
    CHECK_THAT(show_paths_unknown,
               !tgcli::test::matches_json_schema("account-show.result.schema.json"));
}
