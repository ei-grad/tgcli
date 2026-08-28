#include "schema_matcher.hpp"

#include <array>
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
    auto relative = download;
    relative["path"] = "report.pdf";
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/download.result.schema.json").match(relative));
}

TEST_CASE("future raw result schema separates live TD objects from exact dry-run plans",
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

TEST_CASE("dormant raw audit v3 schemas retain hashes and reject request or response bodies",
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

    const auto chat_read =
        terminal("PAGINATION_INVALID", {{"operation", "chat_members"}, {"reason", "page_invalid"}});
    CHECK_THAT(chat_read, tgcli::test::matches_json_schema("future/chat-read.error.schema.json"));
    const auto members_bot = terminal("BOT_UNSUPPORTED", {{"operation", "chat_members"}});
    CHECK_THAT(members_bot, tgcli::test::matches_json_schema("future/chat-read.error.schema.json"));
    const auto info_bot = terminal("BOT_UNSUPPORTED", {{"operation", "chat_info"}});
    CHECK_FALSE(
        tgcli::test::matches_json_schema("future/chat-read.error.schema.json").match(info_bot));

    const auto download = terminal("PRECONDITION_FAILED", {{"operation", "download"},
                                                           {"chat_id", -1001},
                                                           {"message_id", 123},
                                                           {"reason", "album_unsupported"}});
    CHECK_THAT(download, tgcli::test::matches_json_schema("future/download.error.schema.json"));

    const auto raw = terminal(
        "DENIED", {{"operation", "raw"}, {"function", "getChat"}, {"reason", "function_denied"}});
    CHECK_THAT(raw, tgcli::test::matches_json_schema("future/raw.error.schema.json"));
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

    auto cross_operation = cases.front().second;
    cross_operation["error"]["details"]["operation"] = "unread";
    CHECK_FALSE(tgcli::test::matches_json_schema(cases.front().first).match(cross_operation));
}
