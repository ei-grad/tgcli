#include "cli/schema_catalog.hpp"
#include "proto/operation.hpp"
#include "schema_matcher.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
using tgcli::cli::SchemaMappingView;
using tgcli::cli::SchemaPayloadKind;

std::string load_bytes(std::string_view filename) {
    const auto file = tgcli::test::schema_path(std::string(filename));
    std::ifstream input(file, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string_view kind_name(SchemaPayloadKind kind) {
    switch (kind) {
    case SchemaPayloadKind::Result:
        return "result";
    case SchemaPayloadKind::Item:
        return "item";
    case SchemaPayloadKind::Error:
        return "error";
    }
    return {};
}

std::map<std::pair<std::string, std::string>, std::string> source_mappings() {
    std::map<std::pair<std::string, std::string>, std::string> result;
    for (const auto* catalog_name :
         {"manifest.json", "stream-manifest.json", "error-manifest.json"}) {
        const auto catalog = tgcli::test::load_schema_document(catalog_name);
        for (const auto& [command, contracts] : catalog.at("commands").items()) {
            for (const auto& [kind, filename] : contracts.items()) {
                const auto key = std::pair{command, kind};
                const auto [found, inserted] = result.emplace(key, filename.get<std::string>());
                if (!inserted) {
                    REQUIRE(found->second == filename.get<std::string>());
                }
            }
        }
    }
    return result;
}

std::string
expected_all(std::initializer_list<std::pair<std::string_view, std::string_view>> schemas) {
    std::string output{"{"};
    bool first = true;
    for (const auto& [kind, filename] : schemas) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        output.push_back('"');
        output.append(kind);
        output += "\":";
        auto bytes = load_bytes(filename);
        REQUIRE(bytes.ends_with('\n'));
        bytes.pop_back();
        output += bytes;
    }
    output += "}\n";
    return output;
}

} // namespace

TEST_CASE("embedded schema catalogs and documents preserve exact source bytes",
          "[schema][catalog][embedded]") {
    const auto catalogs = tgcli::cli::embedded_schema_catalogs();
    REQUIRE(catalogs.size() == 3);
    CHECK(catalogs[0].filename == "manifest.json");
    CHECK(catalogs[1].filename == "stream-manifest.json");
    CHECK(catalogs[2].filename == "error-manifest.json");
    for (const auto& catalog : catalogs) {
        CHECK(catalog.bytes == load_bytes(catalog.filename));
    }

    const auto expected = source_mappings();
    const auto mappings = tgcli::cli::embedded_schema_mappings();
    REQUIRE(mappings.size() == expected.size());
    for (const auto& mapping : mappings) {
        const auto found =
            expected.find({std::string(mapping.command), std::string(kind_name(mapping.kind))});
        REQUIRE(found != expected.end());
        CHECK(mapping.filename == found->second);
        CHECK(mapping.bytes == load_bytes(mapping.filename));
    }

    CHECK(std::ranges::is_sorted(
        mappings, [](const SchemaMappingView& left, const SchemaMappingView& right) {
            return std::tuple(left.command, left.kind) < std::tuple(right.command, right.kind);
        }));
    CHECK(std::ranges::count_if(mappings, [](const SchemaMappingView& mapping) {
              return mapping.command == "wait-for" && mapping.kind == SchemaPayloadKind::Result;
          }) == 1);
}

TEST_CASE("non-stream error manifest is the exact accepted command authority",
          "[schema][catalog][error]") {
    json expected{{"schemaDialect", "https://json-schema.org/draft/2020-12/schema"},
                  {"commands",
                   {{"account add", {{"error", "account.error.schema.json"}}},
                    {"account list", {{"error", "account.error.schema.json"}}},
                    {"account remove", {{"error", "account-remove.error.schema.json"}}},
                    {"account show", {{"error", "account.error.schema.json"}}},
                    {"account use", {{"error", "account.error.schema.json"}}},
                    {"chat archive", {{"error", "m3-write.error.schema.json"}}},
                    {"chat info", {{"error", "chat-read.error.schema.json"}}},
                    {"chat join", {{"error", "m3-write.error.schema.json"}}},
                    {"chat leave", {{"error", "m3-write.error.schema.json"}}},
                    {"chat mark-read", {{"error", "m3-write.error.schema.json"}}},
                    {"chat members", {{"error", "chat-read.error.schema.json"}}},
                    {"chat mute", {{"error", "m3-write.error.schema.json"}}},
                    {"chat pin", {{"error", "m3-write.error.schema.json"}}},
                    {"chat unarchive", {{"error", "m3-write.error.schema.json"}}},
                    {"chat unmute", {{"error", "m3-write.error.schema.json"}}},
                    {"chat unpin", {{"error", "m3-write.error.schema.json"}}},
                    {"chats", {{"error", "chats.error.schema.json"}}},
                    {"daemon restart", {{"error", "daemon.error.schema.json"}}},
                    {"daemon status", {{"error", "daemon.error.schema.json"}}},
                    {"daemon stop", {{"error", "daemon.error.schema.json"}}},
                    {"doctor", {{"error", "meta.error.schema.json"}}},
                    {"download", {{"error", "download.error.schema.json"}}},
                    {"fetch", {{"error", "fetch.error.schema.json"}}},
                    {"login", {{"error", "auth.error.schema.json"}}},
                    {"logout", {{"error", "logout.error.schema.json"}}},
                    {"me", {{"error", "auth.error.schema.json"}}},
                    {"msg delete", {{"error", "m3-write.error.schema.json"}}},
                    {"msg edit", {{"error", "m3-write.error.schema.json"}}},
                    {"msg forward", {{"error", "m3-write.error.schema.json"}}},
                    {"msg get", {{"error", "msg-get.error.schema.json"}}},
                    {"msg link", {{"error", "msg-link.error.schema.json"}}},
                    {"msg pin", {{"error", "m3-write.error.schema.json"}}},
                    {"msg react", {{"error", "m3-write.error.schema.json"}}},
                    {"msg unpin", {{"error", "m3-write.error.schema.json"}}},
                    {"resolve", {{"error", "resolve.error.schema.json"}}},
                    {"read", {{"error", "read.error.schema.json"}}},
                    {"saved attach", {{"error", "m3-write.error.schema.json"}}},
                    {"saved search", {{"error", "saved.error.schema.json"}}},
                    {"saved tags", {{"error", "saved.error.schema.json"}}},
                    {"search", {{"error", "search.error.schema.json"}}},
                    {"send", {{"error", "m3-write.error.schema.json"}}},
                    {"session list", {{"error", "session.error.schema.json"}}},
                    {"session terminate", {{"error", "session.error.schema.json"}}},
                    {"unread", {{"error", "unread.error.schema.json"}}},
                    {"version", {{"error", "meta.error.schema.json"}}}}}};
    const auto m6_error_schema = [](std::string_view operation) {
        if (operation.starts_with("contact_")) {
            return "contact.error.schema.json";
        }
        if (operation.starts_with("folder_")) {
            return "folder.error.schema.json";
        }
        if (operation.starts_with("topic_")) {
            return "topic.error.schema.json";
        }
        return operation.starts_with("chat_") ? "chat-admin.error.schema.json"
                                              : "storage.error.schema.json";
    };
    for (const auto& identity : tgcli::proto::m6_operation_identities()) {
        const auto* const family = m6_error_schema(identity.canonical_name);
        expected["commands"][identity.command_path] = {{"error", family}};
    }
    CHECK(tgcli::test::load_schema_document("error-manifest.json") == expected);
    CHECK_FALSE(expected.at("commands").contains("listen"));
    CHECK_FALSE(expected.at("commands").contains("wait-for"));
}

TEST_CASE("schema target normalization is ASCII-only and history is the sole alias",
          "[schema][catalog][normalization]") {
    const std::vector<std::string_view> tokens{" account", "\tlist\r\n"};
    CHECK(tgcli::cli::normalize_schema_target(tokens) == "account list");

    const std::vector<std::string_view> whitespace{" \t\n\r\f\v"};
    CHECK_FALSE(tgcli::cli::normalize_schema_target(whitespace));
    CHECK_FALSE(tgcli::cli::normalize_schema_target({}));

    const std::vector<std::string_view> non_ascii{"account\xC2\xA0list"};
    CHECK(tgcli::cli::normalize_schema_target(non_ascii) == "account\xC2\xA0list");
    CHECK(tgcli::cli::canonicalize_schema_target("history") == "read");
    CHECK(tgcli::cli::canonicalize_schema_target("History") == "History");
    CHECK(tgcli::cli::canonicalize_schema_target("read") == "read");
}

TEST_CASE("schema lookup selects result then item and renders fixed all-kind order",
          "[schema][catalog][lookup]") {
    const auto version = tgcli::cli::find_schema_set("version");
    REQUIRE(version);
    REQUIRE(version->result != nullptr);
    CHECK(version->item == nullptr);
    REQUIRE(version->error != nullptr);
    CHECK(tgcli::cli::primary_schema(*version) == version->result);
    CHECK(tgcli::cli::render_all_schemas(*version) ==
          expected_all(
              {{"result", "version.result.schema.json"}, {"error", "meta.error.schema.json"}}));

    const auto listen = tgcli::cli::find_schema_set("listen");
    REQUIRE(listen);
    CHECK(listen->result == nullptr);
    REQUIRE(listen->item != nullptr);
    REQUIRE(listen->error != nullptr);
    CHECK(tgcli::cli::primary_schema(*listen) == listen->item);
    CHECK(
        tgcli::cli::render_all_schemas(*listen) ==
        expected_all({{"item", "listen.item.schema.json"}, {"error", "stream.error.schema.json"}}));

    const auto wait_for = tgcli::cli::find_schema_set("wait-for");
    REQUIRE(wait_for);
    CHECK(tgcli::cli::render_all_schemas(*wait_for) ==
          expected_all(
              {{"result", "wait-for.result.schema.json"}, {"error", "stream.error.schema.json"}}));

    const auto removal = tgcli::cli::find_schema_set("account remove");
    REQUIRE(removal);
    CHECK(tgcli::cli::render_all_schemas(*removal) ==
          expected_all({{"result", "account-remove.result.schema.json"},
                        {"error", "account-remove.error.schema.json"}}));

    const auto chats = tgcli::cli::find_schema_set("chats");
    REQUIRE(chats);
    REQUIRE(chats->result != nullptr);
    REQUIRE(chats->error != nullptr);
    CHECK(chats->error->filename == "chats.error.schema.json");

    const auto resolve = tgcli::cli::find_schema_set("resolve");
    REQUIRE(resolve);
    REQUIRE(resolve->result != nullptr);
    REQUIRE(resolve->error != nullptr);

    const auto unread = tgcli::cli::find_schema_set("unread");
    REQUIRE(unread);
    REQUIRE(unread->result != nullptr);
    CHECK(unread->result->filename == "unread.result.schema.json");
    CHECK(unread->item == nullptr);
    REQUIRE(unread->error != nullptr);
    CHECK(unread->error->filename == "unread.error.schema.json");

    const auto read = tgcli::cli::find_schema_set("read");
    REQUIRE(read);
    REQUIRE(read->result != nullptr);
    CHECK(read->result->filename == "read.result.schema.json");
    CHECK(read->item == nullptr);
    REQUIRE(read->error != nullptr);
    CHECK(read->error->filename == "read.error.schema.json");

    const auto fetch = tgcli::cli::find_schema_set("fetch");
    REQUIRE(fetch);
    REQUIRE(fetch->result != nullptr);
    CHECK(fetch->result->filename == "fetch.result.schema.json");
    CHECK(fetch->item == nullptr);
    REQUIRE(fetch->error != nullptr);
    CHECK(fetch->error->filename == "fetch.error.schema.json");

    CHECK_FALSE(tgcli::cli::find_schema_set("schema"));
    CHECK_FALSE(tgcli::cli::find_schema_set("daemon run"));
    CHECK_FALSE(tgcli::cli::find_schema_set("history"));
    for (const auto* audit_target : {"audit intent", "audit checkpoint", "audit outcome"}) {
        CHECK_FALSE(tgcli::cli::find_schema_set(audit_target));
    }
    for (const auto& mapping : tgcli::cli::embedded_schema_mappings()) {
        CHECK(mapping.filename != "audit-intent.schema.json");
        CHECK(mapping.filename != "audit-checkpoint.schema.json");
        CHECK(mapping.filename != "audit-outcome.schema.json");
    }
}

TEST_CASE("schema target completion keys expose only cataloged lookup operands",
          "[schema][catalog][completion]") {
    const auto keys = tgcli::cli::schema_target_completion_keys();
    CHECK(std::ranges::is_sorted(keys));
    CHECK(std::ranges::adjacent_find(keys) == keys.end());

    std::set<std::string_view> expected;
    for (const auto& mapping : tgcli::cli::embedded_schema_mappings()) {
        expected.insert(mapping.command);
    }
    if (expected.contains("read")) {
        expected.insert("history");
    }
    CHECK(std::vector<std::string_view>(keys.begin(), keys.end()) ==
          std::vector<std::string_view>(expected.begin(), expected.end()));
    CHECK(std::ranges::find(keys, "schema") == keys.end());
    CHECK(std::ranges::find(keys, "daemon run") == keys.end());
}
