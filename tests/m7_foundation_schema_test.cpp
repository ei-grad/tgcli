#include "schema_matcher.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

json terminal(std::string_view code, json details) {
    return {{"error", {{"code", code}, {"message", "contract error"}, {"details", details}}}};
}

json message() {
    return {{"id", 123},
            {"chat_id", -1001},
            {"date", "2026-08-28T10:00:00Z"},
            {"sender", {{"type", "user"}, {"id", 42}}},
            {"is_outgoing", false},
            {"topic", nullptr},
            {"type", "text"},
            {"text", "match"}};
}

} // namespace

TEST_CASE("future M2 and M4 result schemas are strict and remain independently valid",
          "[schema][m7-foundation]") {
    const json search{{"items", json::array({message()})}, {"next", "cursor"}};
    CHECK_THAT(search, tgcli::test::matches_json_schema("future/search.result.schema.json"));
    auto invalid_search = search;
    invalid_search["items"][0]["raw"] = json::object();
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/search.result.schema.json").match(invalid_search));

    const json private_info{{"id", 42},
                            {"title", "Ada"},
                            {"type", "private"},
                            {"is_bot", false},
                            {"usernames", json::array({"ada"})},
                            {"description", "bio"},
                            {"member_count", nullptr},
                            {"is_forum", false},
                            {"linked_chat_id", nullptr},
                            {"is_archived", false},
                            {"folder_ids", json::array({2})},
                            {"is_marked_unread", false},
                            {"unread_count", 0},
                            {"unread_mention_count", 0},
                            {"unread_reaction_count", 0},
                            {"unread_poll_vote_count", 0}};
    CHECK_THAT(private_info,
               tgcli::test::matches_json_schema("future/chat-info.result.schema.json"));
    auto invalid_info = private_info;
    invalid_info["member_count"] = 1;
    CHECK_FALSE(tgcli::test::matches_json_schema("future/chat-info.result.schema.json")
                    .match(invalid_info));
    auto basic_info = private_info;
    basic_info["type"] = "basic_group";
    basic_info["is_bot"] = false;
    basic_info["member_count"] = 7;
    CHECK_THAT(basic_info, tgcli::test::matches_json_schema("future/chat-info.result.schema.json"));
    basic_info["member_count"] = nullptr;
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/chat-info.result.schema.json").match(basic_info));
    for (const auto* type : {"supergroup", "channel"}) {
        auto aggregate_info = private_info;
        aggregate_info["type"] = type;
        aggregate_info["is_bot"] = false;
        aggregate_info["member_count"] = 7;
        CHECK_THAT(aggregate_info,
                   tgcli::test::matches_json_schema("future/chat-info.result.schema.json"));
        aggregate_info["member_count"] = nullptr;
        CHECK_FALSE(tgcli::test::matches_json_schema("future/chat-info.result.schema.json")
                        .match(aggregate_info));
        aggregate_info["member_count"] = -1;
        CHECK_FALSE(tgcli::test::matches_json_schema("future/chat-info.result.schema.json")
                        .match(aggregate_info));
    }

    const json chat_member{{"sender", {{"type", "chat"}, {"id", -1002}}},
                           {"display_name", "Linked channel"},
                           {"usernames", json::array({"linked"})},
                           {"is_bot", false},
                           {"status", "banned"},
                           {"tag", ""},
                           {"joined_at", nullptr}};
    const json members{{"items", json::array({chat_member})}, {"next", nullptr}};
    CHECK_THAT(members, tgcli::test::matches_json_schema("future/chat-members.result.schema.json"));
    auto invalid_members = members;
    invalid_members["items"][0]["sender"]["type"] = "unknown";
    CHECK_FALSE(tgcli::test::matches_json_schema("future/chat-members.result.schema.json")
                    .match(invalid_members));
    auto bot_chat_sender = members;
    bot_chat_sender["items"][0]["is_bot"] = true;
    CHECK_FALSE(tgcli::test::matches_json_schema("future/chat-members.result.schema.json")
                    .match(bot_chat_sender));

    const json download{{"chat_id", -1001},         {"message_id", 123},         {"file_id", 7},
                        {"media_type", "document"}, {"path", "/tmp/report.pdf"}, {"bytes", 4096}};
    CHECK_THAT(download, tgcli::test::matches_json_schema("future/download.result.schema.json"));
    CHECK_THAT(download, tgcli::test::matches_json_schema("download.result.schema.json"));
    auto relative = download;
    relative["path"] = "report.pdf";
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/download.result.schema.json").match(relative));
    auto maximum_bytes = download;
    maximum_bytes["bytes"] = std::numeric_limits<std::uint64_t>::max();
    CHECK_THAT(maximum_bytes,
               tgcli::test::matches_json_schema("future/download.result.schema.json"));
    auto overflow_bytes = download;
    overflow_bytes["bytes"] = json::parse("18446744073709551616");
    CHECK_FALSE(tgcli::test::matches_json_schema("future/download.result.schema.json")
                    .match(overflow_bytes));
}

TEST_CASE("raw result schema separates live TD objects from exact dry-run plans",
          "[schema][m7-foundation][raw]") {
    const json live{{"@type", "ok"}, {"value", true}};
    CHECK_THAT(live, tgcli::test::matches_json_schema("future/raw.result.schema.json"));
    auto transport = live;
    transport["@extra"] = "leak";
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw.result.schema.json").match(transport));

    const json dry{{"dry_run", true},
                   {"plan",
                    {{"operation", "raw"},
                     {"function", "deleteMessages"},
                     {"tier", "destructive"},
                     {"tdlib_sha", "a17f87c4cff7b90b278d12b91ba0614383aaee82"},
                     {"request_sha256",
                      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
                     {"request_bytes", 128}}}};
    CHECK_THAT(dry, tgcli::test::matches_json_schema("future/raw.result.schema.json"));
    auto lowered = dry;
    lowered["plan"]["tier"] = "read";
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw.result.schema.json").match(lowered));
}

TEST_CASE("raw audit v3 schemas retain hashes and reject request or response bodies",
          "[schema][m7-foundation][raw][audit-v3]") {
    constexpr std::string_view invocation = "00112233445566778899aabbccddeeff";
    constexpr std::string_view token = "ffeeddccbbaa99887766554433221100";
    constexpr std::string_view hash =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const json intent{
        {"schema_version", 3},         {"record_type", "raw_intent"},
        {"invocation_id", invocation}, {"function", "deleteMessages"},
        {"tier", "destructive"},       {"tdlib_sha", "a17f87c4cff7b90b278d12b91ba0614383aaee82"},
        {"request_sha256", hash},      {"request_bytes", 128}};
    CHECK_THAT(intent, tgcli::test::matches_json_schema("future/raw-audit-intent.v3.schema.json"));
    auto leaked_request = intent;
    leaked_request["request"] = {{"@type", "deleteMessages"}};
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-intent.v3.schema.json")
                    .match(leaked_request));

    const json response{{"schema_version", 3},
                        {"record_type", "raw_checkpoint"},
                        {"invocation_id", invocation},
                        {"stage", "raw_response_received"},
                        {"data",
                         {{"dispatch_token", token},
                          {"generation", "18446744073709551615"},
                          {"kind", "result"},
                          {"response_type", "ok"},
                          {"td_error_code", nullptr},
                          {"response_sha256", hash},
                          {"response_bytes", 32}}}};
    CHECK_THAT(response,
               tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json"));
    auto generation_overflow = response;
    generation_overflow["data"]["generation"] = "18446744073709551616";
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json")
                    .match(generation_overflow));
    auto zero_generation = response;
    zero_generation["data"]["generation"] = "0";
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json")
                    .match(zero_generation));
    auto error_as_result = response;
    error_as_result["data"]["response_type"] = "error";
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json")
                    .match(error_as_result));
    auto leaked_response = response;
    leaked_response["data"]["response"] = {{"@type", "ok"}};
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json")
                    .match(leaked_response));
    auto malformed_response = response;
    malformed_response["data"] = {{"dispatch_token", token},  {"generation", "7"},
                                  {"kind", "malformed"},      {"response_type", "updateNewChat"},
                                  {"td_error_code", nullptr}, {"response_sha256", hash},
                                  {"response_bytes", 32}};
    CHECK_THAT(malformed_response,
               tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json"));
    malformed_response["data"]["response_sha256"] = nullptr;
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json")
                    .match(malformed_response));
    auto oversized_response = response;
    oversized_response["data"] = {{"dispatch_token", token},    {"generation", "7"},
                                  {"kind", "result_too_large"}, {"response_type", "ok"},
                                  {"td_error_code", nullptr},   {"response_sha256", nullptr},
                                  {"response_bytes", nullptr}};
    CHECK_THAT(oversized_response,
               tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json"));
    oversized_response["data"]["response_bytes"] = 16'777'216;
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-checkpoint.v3.schema.json")
                    .match(oversized_response));

    const json outcome{{"schema_version", 3},
                       {"record_type", "raw_outcome"},
                       {"invocation_id", invocation},
                       {"mutation_state", "confirmed"},
                       {"terminal",
                        {{"kind", "result_digest"},
                         {"response_type", "ok"},
                         {"response_sha256", hash},
                         {"response_bytes", 32}}}};
    CHECK_THAT(outcome,
               tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json"));
    const json none_outcome{{"schema_version", 3},
                            {"record_type", "raw_outcome"},
                            {"invocation_id", invocation},
                            {"mutation_state", "none"},
                            {"terminal", nullptr}};
    CHECK_THAT(none_outcome,
               tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json"));
    auto nonnull_none = none_outcome;
    nonnull_none["terminal"] = json::object();
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json")
                    .match(nonnull_none));
    auto unconfirmed = outcome;
    unconfirmed["mutation_state"] = "possible";
    unconfirmed["terminal"] = {
        {"kind", "error_summary"}, {"code", "RAW_OUTCOME_UNCONFIRMED"}, {"td_error_code", nullptr}};
    CHECK_THAT(unconfirmed,
               tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json"));
    auto internal_surrogate = unconfirmed;
    internal_surrogate["terminal"]["code"] = "INTERNAL";
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json")
                    .match(internal_surrogate));
    auto malformed_outcome = unconfirmed;
    malformed_outcome["terminal"] = {{"kind", "error_summary"},
                                     {"code", "INTERNAL"},
                                     {"reason", "unexpected_response"},
                                     {"td_error_code", nullptr}};
    CHECK_THAT(malformed_outcome,
               tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json"));
    malformed_outcome["terminal"]["reason"] = "result_too_large";
    CHECK_THAT(malformed_outcome,
               tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json"));
    auto leaked_terminal = outcome;
    leaked_terminal["terminal"]["body"] = {{"@type", "ok"}};
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw-audit-outcome.v3.schema.json")
                    .match(leaked_terminal));
}

TEST_CASE("future family error schemas reject cross-operation and secret-bearing fields",
          "[schema][m7-foundation][error]") {
    const auto search = terminal("PAGINATION_INVALID",
                                 {{"operation", "search"}, {"reason", "marker_not_advancing"}});
    CHECK_THAT(search, tgcli::test::matches_json_schema("future/search.error.schema.json"));
    auto wrong_search = search;
    wrong_search["error"]["details"]["operation"] = "chat_members";
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/search.error.schema.json").match(wrong_search));
    for (const auto* filename : {"search.error.schema.json", "future/search.error.schema.json"}) {
        for (const auto* resource : {"raw_scanned_items", "cursor_marker_bytes"}) {
            const auto capacity = terminal(
                "RESOURCE_LIMIT", {{"operation", "search"}, {"resource", resource}, {"limit", 1}});
            CHECK_THAT(capacity, tgcli::test::matches_json_schema(filename));
        }
        const auto stale_capacity = terminal(
            "RESOURCE_LIMIT", {{"operation", "search"}, {"resource", "items"}, {"limit", 1}});
        CHECK_FALSE(tgcli::test::matches_json_schema(filename).match(stale_capacity));
    }

    for (const auto* filename :
         {"future/search.error.schema.json", "future/chat-read.error.schema.json",
          "future/download.error.schema.json", "future/raw.error.schema.json"}) {
        const auto config_timeout =
            terminal("TIMEOUT", {{"operation", "config_admission"}, {"state", nullptr}});
        CHECK_THAT(config_timeout, tgcli::test::matches_json_schema(filename));
        const auto shutdown = terminal("DAEMON_SHUTDOWN", {{"reason", "daemon_shutdown"}});
        CHECK_THAT(shutdown, tgcli::test::matches_json_schema(filename));
    }

    const auto chat_read =
        terminal("PAGINATION_INVALID", {{"operation", "chat_members"}, {"reason", "page_invalid"}});
    CHECK_THAT(chat_read, tgcli::test::matches_json_schema("future/chat-read.error.schema.json"));
    const auto members_bot = terminal("BOT_UNSUPPORTED", {{"operation", "chat_members"}});
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/chat-read.error.schema.json").match(members_bot));
    const auto info_bot = terminal("BOT_UNSUPPORTED", {{"operation", "chat_info"}});
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/chat-read.error.schema.json").match(info_bot));
    const auto resolver_bot = terminal("BOT_UNSUPPORTED", {{"operation", "resolve"}});
    CHECK_THAT(resolver_bot,
               tgcli::test::matches_json_schema("future/chat-read.error.schema.json"));
    CHECK_THAT(resolver_bot, tgcli::test::matches_json_schema("future/download.error.schema.json"));

    const auto download = terminal("PRECONDITION_FAILED", {{"operation", "download"},
                                                           {"chat_id", -1001},
                                                           {"message_id", 123},
                                                           {"reason", "album_unsupported"}});
    CHECK_THAT(download, tgcli::test::matches_json_schema("future/download.error.schema.json"));
    CHECK_THAT(download, tgcli::test::matches_json_schema("download.error.schema.json"));
    const auto missing_download = terminal("NOT_FOUND", {{"chat_id", -1001}, {"message_id", 123}});
    CHECK_THAT(missing_download,
               tgcli::test::matches_json_schema("future/download.error.schema.json"));
    const auto unavailable = terminal(
        "OUTPUT_UNAVAILABLE",
        {{"operation", "download"}, {"path", "/tmp/report.pdf"}, {"reason", "cleanup_failed"}});
    CHECK_THAT(unavailable, tgcli::test::matches_json_schema("future/download.error.schema.json"));
    auto no_path = unavailable;
    no_path["error"]["details"].erase("path");
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/download.error.schema.json").match(no_path));

    const auto raw = terminal(
        "DENIED", {{"operation", "raw"}, {"function", "getChat"}, {"reason", "function_denied"}});
    CHECK_THAT(raw, tgcli::test::matches_json_schema("future/raw.error.schema.json"));
    auto unconfirmed_raw =
        terminal("RAW_OUTCOME_UNCONFIRMED",
                 {{"operation", "raw"},
                  {"function", "deleteMessages"},
                  {"request_sha256",
                   "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
                  {"mutation_state", "possible"}});
    unconfirmed_raw["error"]["message"] = "raw request outcome is unconfirmed";
    CHECK_THAT(unconfirmed_raw, tgcli::test::matches_json_schema("future/raw.error.schema.json"));
    auto wrong_unconfirmed = unconfirmed_raw;
    wrong_unconfirmed["error"]["details"]["account"] = "main";
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/raw.error.schema.json").match(wrong_unconfirmed));
    auto leaked = raw;
    leaked["error"]["details"]["request"] = {{"@type", "getChat"}};
    CHECK_FALSE(tgcli::test::matches_json_schema("future/raw.error.schema.json").match(leaked));
}

TEST_CASE("active M2 error schemas are command-local and cataloged",
          "[schema][m2][error][catalog]") {
    const auto catalog = tgcli::test::load_schema_document("error-manifest.json");
    const std::array mappings{
        std::pair{"chats", "chats.error.schema.json"},
        std::pair{"unread", "unread.error.schema.json"},
        std::pair{"read", "read.error.schema.json"},
        std::pair{"msg get", "msg-get.error.schema.json"},
        std::pair{"msg link", "msg-link.error.schema.json"},
        std::pair{"fetch", "fetch.error.schema.json"},
    };
    for (const auto& [command, filename] : mappings) {
        REQUIRE(catalog["commands"].contains(command));
        CHECK(catalog["commands"][command] == json{{"error", filename}});
    }

    const std::array cases{
        std::pair{"chats.error.schema.json",
                  terminal("TIMEOUT", {{"operation", "chats"}, {"state", "ready"}})},
        std::pair{"unread.error.schema.json",
                  terminal("INTERNAL", {{"operation", "unread"}, {"reason", "internal_error"}})},
        std::pair{"read.error.schema.json",
                  terminal("PAGINATION_INVALID",
                           {{"operation", "read"}, {"reason", "non_advancing_upstream"}})},
        std::pair{
            "msg-get.error.schema.json",
            terminal("NOT_FOUND", {{"chat_id", -1001}, {"missing_ids", json::array({1, 2})}})},
        std::pair{"msg-link.error.schema.json",
                  terminal("NOT_FOUND", {{"chat_id", -1001}, {"message_id", 1}})},
        std::pair{"fetch.error.schema.json", terminal("TIMEOUT", {{"operation", "fetch"},
                                                                  {"chat_id", -1001},
                                                                  {"phase", "network_fill"},
                                                                  {"state", "ready"},
                                                                  {"cached_count", 2},
                                                                  {"oldest_message_id", 1},
                                                                  {"resume_from_message_id", 1}})},
    };
    for (const auto& [filename, value] : cases) {
        CHECK_THAT(value, tgcli::test::matches_json_schema(filename));
    }

    for (const auto& [command, filename] : mappings) {
        static_cast<void>(command);
        const auto timeout =
            terminal("TIMEOUT", {{"operation", "config_admission"}, {"state", nullptr}});
        CHECK_THAT(timeout, tgcli::test::matches_json_schema(filename));
    }

    const auto resolver_bot = terminal("BOT_UNSUPPORTED", {{"operation", "resolve"}});
    CHECK_THAT(resolver_bot, tgcli::test::matches_json_schema("msg-get.error.schema.json"));
    CHECK_THAT(resolver_bot, tgcli::test::matches_json_schema("msg-link.error.schema.json"));
    CHECK_FALSE(tgcli::test::matches_json_schema("read.error.schema.json").match(resolver_bot));
    CHECK_FALSE(tgcli::test::matches_json_schema("fetch.error.schema.json").match(resolver_bot));

    auto cross_operation = cases.front().second;
    cross_operation["error"]["details"]["operation"] = "unread";
    CHECK_FALSE(tgcli::test::matches_json_schema(cases.front().first).match(cross_operation));
}

TEST_CASE("generated protocol error schemas preserve the exact uint64 request-id ceiling",
          "[schema][uint64][generator]") {
    for (const auto* filename :
         {"contact.error.schema.json", "folder.error.schema.json", "topic.error.schema.json",
          "chat-admin.error.schema.json", "storage.error.schema.json",
          "future/search.error.schema.json", "future/chat-read.error.schema.json",
          "future/download.error.schema.json", "future/raw.error.schema.json"}) {
        const auto endpoint = terminal(
            "PROTOCOL_ANSWER_INVALID",
            {{"request_id", std::numeric_limits<std::uint64_t>::max()}, {"reason", "malformed"}});
        CHECK_THAT(endpoint, tgcli::test::matches_json_schema(filename));
        auto overflow = endpoint;
        overflow["error"]["details"]["request_id"] = json::parse("18446744073709551616");
        CHECK_FALSE(tgcli::test::matches_json_schema(filename).match(overflow));
    }
}
