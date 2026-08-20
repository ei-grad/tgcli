#include "cli/render.hpp"
#include "schema_matcher.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

std::string golden(std::string_view filename) {
    const std::ifstream input(std::string(TGCLI_GOLDEN_DIR) + "/" + std::string(filename));
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

} // namespace

TEST_CASE("account human renderers match reviewed goldens", "[account][render][golden]") {
    CHECK(tgcli::cli::render_human("account add",
                                   {{"account", "work"}, {"created", true}, {"default", false}}) ==
          golden("account-add.txt"));
    CHECK(tgcli::cli::render_human(
              "account list", {{"items", json::array({json{{"name", "main"}, {"default", true}},
                                                      json{{"name", "work"}, {"default", false}}})},
                               {"next", nullptr}}) == golden("account-list.txt"));
    CHECK(tgcli::cli::render_human("account list", {{"items", json::array()}, {"next", nullptr}}) ==
          golden("account-list-empty.txt"));
    CHECK(tgcli::cli::render_human("account show", {{"account", "main"},
                                                    {"default", true},
                                                    {"allow_write", false},
                                                    {"idle_exit", nullptr},
                                                    {"credentials",
                                                     {{"api_id", "value"},
                                                      {"api_hash", "command"},
                                                      {"db_key", "none"},
                                                      {"password", "interactive"},
                                                      {"bot_token", "command"}}},
                                                    {"paths",
                                                     {{"data", "/data/tgcli/accounts/main"},
                                                      {"state", "/state/tgcli/accounts/main"},
                                                      {"socket", "/run/tgcli/main.sock"}}}}) ==
          golden("account-show.txt"));
    CHECK(tgcli::cli::render_human("account use",
                                   {{"default_account", "work"}, {"previous_default", "main"}}) ==
          golden("account-use.txt"));
    CHECK(tgcli::cli::render_human("account remove", {{"account", "work"},
                                                      {"removed", true},
                                                      {"remote_logout", "kept"},
                                                      {"default_account", nullptr}}) ==
          golden("account-remove.txt"));
}

TEST_CASE("login and me human renderers match reviewed goldens", "[auth][render][golden]") {
    const json user{{"id", 123456},
                    {"first_name", "Ada"},
                    {"last_name", "Lovelace"},
                    {"usernames", json::array({"ada", "analytical_engine"})},
                    {"phone_number", "12025550123"},
                    {"is_bot", false},
                    {"is_premium", true}};
    CHECK(tgcli::cli::render_human("me", user) == golden("me.txt"));
    CHECK(tgcli::cli::render_human(
              "login", {{"account", "main"}, {"auth_state", "ready"}, {"user", user}}) ==
          golden("login.txt"));
}

TEST_CASE("logout human renderers match reviewed goldens", "[logout][render][golden]") {
    CHECK(tgcli::cli::render_human("logout", {{"account", "main"}, {"logged_out", true}}) ==
          golden("logout.txt"));
    CHECK(tgcli::cli::render_human("logout", {{"dry_run", true},
                                              {"plan",
                                               {{"operation", "logout"},
                                                {"account", "main"},
                                                {"remote_logout", true},
                                                {"tdlib_request", "logOut"}}}}) ==
          golden("logout-dry-run.txt"));
}

TEST_CASE("version human output matches reviewed revision goldens", "[version][render][golden]") {
    CHECK(tgcli::cli::render_human("version",
                                   {{"version", "0.1.0"}, {"protocol", 3}, {"tdlib", "1.8.65"}}) ==
          golden("version.txt"));
    CHECK(
        tgcli::cli::render_human(
            "version",
            {{"version", "0.1.0"}, {"protocol", 3}, {"tdlib", "1.8.65"}, {"commit", "4d7ca6e"}}) ==
        golden("version-commit.txt"));
    CHECK(tgcli::cli::render_human("version", {{"version", "0.1.0"},
                                               {"protocol", 3},
                                               {"tdlib", "1.8.65"},
                                               {"commit", "4d7ca6e-dirty"}}) ==
          golden("version-commit-dirty.txt"));
}

TEST_CASE("daemon control human renderers match reviewed goldens",
          "[daemon-control][render][golden]") {
    CHECK(tgcli::cli::render_human("daemon status", {{"account", "main"},
                                                     {"running", true},
                                                     {"pid", 123},
                                                     {"version", "0.1.0"},
                                                     {"protocol", 1},
                                                     {"socket", "/run/tgcli/main.sock"}}) ==
          golden("daemon-status-running.txt"));
    CHECK(tgcli::cli::render_human(
              "daemon status",
              {{"account", "main"}, {"running", false}, {"socket", "/run/tgcli/main.sock"}}) ==
          golden("daemon-status-absent.txt"));
    CHECK(tgcli::cli::render_human("daemon stop", {{"stopping", true}}) ==
          golden("daemon-stop.txt"));
    CHECK(tgcli::cli::render_human("daemon restart", {{"account", "main"},
                                                      {"restarted", true},
                                                      {"pid", 124},
                                                      {"version", "0.1.0"},
                                                      {"protocol", 1},
                                                      {"socket", "/run/tgcli/main.sock"}}) ==
          golden("daemon-restart.txt"));
}

TEST_CASE("Saved Messages human renderers match reviewed goldens", "[saved][render][golden]") {
    CHECK(tgcli::cli::render_human(
              "saved tags",
              {{"items",
                json::array({json{{"tag", "🧪"}, {"label", "experiments"}, {"count", 7}},
                             json{{"tag", "custom:123456789"}, {"label", ""}, {"count", 2}}})},
               {"next", nullptr}}) == golden("saved-tags.txt"));
    CHECK(tgcli::cli::render_human("saved search",
                                   {{"items", json::array({json{{"id", 200},
                                                                {"chat_id", 42},
                                                                {"date", "2026-07-02T12:00:00Z"},
                                                                {"text", "experiment result"}},
                                                           json{{"id", 199},
                                                                {"chat_id", 42},
                                                                {"date", "2026-07-02T11:59:00Z"},
                                                                {"text", ""}}})},
                                    {"next", "cursor-2"}}) == golden("saved-search.txt"));
}

TEST_CASE("session list human renderer preserves every curated field as TSV",
          "[session][render][golden]") {
    const json first{{"id", "0"},
                     {"is_current", true},
                     {"is_password_pending", false},
                     {"is_unconfirmed", true},
                     {"can_accept_secret_chats", true},
                     {"can_accept_calls", false},
                     {"device_type", "linux"},
                     {"api_id", 2040},
                     {"application_name", "Desktop\tBeta"},
                     {"application_version", "5.1\nnightly"},
                     {"is_official_application", true},
                     {"device_model", "ThinkPad \"P1\""},
                     {"platform", "Linux"},
                     {"system_version", "6.10\\custom"},
                     {"log_in_date", nullptr},
                     {"last_active_date", "2038-01-19T03:14:07Z"},
                     {"ip_address", "203.0.113.10\tvpn"},
                     {"location", "Athens\nGreece"}};
    const json second{{"id", "9007199254740992"},
                      {"is_current", false},
                      {"is_password_pending", true},
                      {"is_unconfirmed", false},
                      {"can_accept_secret_chats", false},
                      {"can_accept_calls", true},
                      {"device_type", "iphone"},
                      {"api_id", -2147483648},
                      {"application_name", "Telegram iOS"},
                      {"application_version", "11.0"},
                      {"is_official_application", true},
                      {"device_model", "iPhone"},
                      {"platform", "iOS"},
                      {"system_version", "18.0"},
                      {"log_in_date", "1970-01-01T00:00:01Z"},
                      {"last_active_date", nullptr},
                      {"ip_address", "2001:db8::1"},
                      {"location", ""}};
    const json result{{"items", json::array({first, second})},
                      {"inactive_session_ttl_days", 30},
                      {"next", nullptr}};
    CHECK_THAT(result, tgcli::test::matches_json_schema("session-list.result.schema.json"));
    const auto rendered = tgcli::cli::render_human("session list", result);
    CHECK(rendered == golden("session-list.txt"));
    CHECK(rendered.find(first["ip_address"].dump()) != std::string::npos);
    CHECK(rendered.find(first["location"].dump()) != std::string::npos);

    const json empty{{"items", json::array()}, {"inactive_session_ttl_days", 1}, {"next", nullptr}};
    CHECK_THAT(empty, tgcli::test::matches_json_schema("session-list.result.schema.json"));
    CHECK(tgcli::cli::render_human("session list", empty) == golden("session-list-empty.txt"));
}

TEST_CASE("session terminate human renderers match real and dry-run goldens",
          "[session][render][golden]") {
    const json real{{"session_id", "-9223372036854775808"}, {"terminated", true}};
    CHECK_THAT(real, tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    CHECK(tgcli::cli::render_human("session terminate", real) == golden("session-terminate.txt"));

    const json dry_run{{"dry_run", true},
                       {"plan",
                        {{"operation", "session_terminate"},
                         {"account", "main"},
                         {"tdlib_request", "terminateSession"},
                         {"session",
                          {{"id", "9007199254740992"},
                           {"is_current", false},
                           {"is_password_pending", true},
                           {"is_unconfirmed", false},
                           {"device_type", "iphone"},
                           {"application_name", "Telegram\tDesktop"},
                           {"application_version", "5.1\nnightly"},
                           {"device_model", "ThinkPad \"P1\""},
                           {"platform", "Linux"},
                           {"system_version", "6.10\\custom"},
                           {"last_active_date", "2038-01-19T03:14:07Z"}}}}}};
    CHECK_THAT(dry_run, tgcli::test::matches_json_schema("session-terminate.result.schema.json"));
    const auto rendered = tgcli::cli::render_human("session terminate", dry_run);
    CHECK(rendered == golden("session-terminate-dry-run.txt"));
    CHECK(rendered.find("ip_address") == std::string::npos);
    CHECK(rendered.find("location") == std::string::npos);
}

TEST_CASE("chats human renderer matches its reviewed golden", "[chats][render][golden]") {
    CHECK(tgcli::cli::render_human(
              "chats", {{"items", json::array({json{{"id", -1001},
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
                        {"next", "cursor-2"}}) == golden("chats.txt"));
}

TEST_CASE("unread human renderer preserves every field and explicit empty state",
          "[unread][render][golden]") {
    const json first{{"id", -1001},
                     {"title", "Project\nAlpha"},
                     {"type", "supergroup"},
                     {"is_bot", false},
                     {"is_archived", false},
                     {"is_marked_unread", false},
                     {"unread_count", 3},
                     {"unread_mention_count", 1},
                     {"unread_reaction_count", 0},
                     {"unread_poll_vote_count", 0}};
    const json second{{"id", 42},
                      {"title", "Build Bot"},
                      {"type", "private"},
                      {"is_bot", true},
                      {"is_archived", true},
                      {"is_marked_unread", true},
                      {"unread_count", 0},
                      {"unread_mention_count", 0},
                      {"unread_reaction_count", 2},
                      {"unread_poll_vote_count", 1}};
    CHECK(tgcli::cli::render_human("unread", {{"items", json::array({first, second})},
                                              {"next", nullptr}}) == golden("unread.txt"));
    CHECK(tgcli::cli::render_human("unread", {{"items", json::array()}, {"next", nullptr}}) ==
          golden("unread-empty.txt"));
}

TEST_CASE("resolve human renderer matches its reviewed golden", "[resolver][render][golden]") {
    CHECK(tgcli::cli::render_human("resolve", {{"kind", "message"},
                                               {"chat",
                                                {{"id", -1001},
                                                 {"title", "Project"},
                                                 {"type", "supergroup"},
                                                 {"is_bot", false},
                                                 {"usernames", json::array({"project"})}}},
                                               {"message_id", 123},
                                               {"topic", {{"kind", "forum"}, {"id", 7}}},
                                               {"link_type", "message"},
                                               {"is_public", true}}) == golden("resolve.txt"));
}

TEST_CASE("message read human renderers match their reviewed exact TSV goldens",
          "[msg][render][golden]") {
    const json message{{"id", 123},
                       {"chat_id", -1001},
                       {"date", "2026-08-05T10:00:00Z"},
                       {"sender", {{"type", "user"}, {"id", 42}}},
                       {"is_outgoing", false},
                       {"topic", {{"kind", "forum"}, {"id", 7}}},
                       {"type", "text"},
                       {"text", "message or caption"}};
    CHECK(tgcli::cli::render_human("msg get", {{"items", json::array({message})},
                                               {"next", nullptr}}) == golden("msg-get.txt"));
    CHECK(tgcli::cli::render_human("msg link", {{"chat_id", -1001},
                                                {"message_id", 123},
                                                {"link", "https://t.me/example/7"},
                                                {"is_public", true}}) == golden("msg-link.txt"));
}

TEST_CASE("read human renderer matches its reviewed exact TSV golden", "[read][render][golden]") {
    const json message{{"id", 123},
                       {"chat_id", -1001},
                       {"date", "2026-08-05T10:00:00Z"},
                       {"sender", {{"type", "user"}, {"id", 42}}},
                       {"is_outgoing", false},
                       {"topic", {{"kind", "forum"}, {"id", 7}}},
                       {"type", "text"},
                       {"text", "message or caption"}};
    const json result{
        {"items", json::array({message})}, {"next", "cursor-2"}, {"boundary", "page"}};
    CHECK_THAT(result, tgcli::test::matches_json_schema("read.result.schema.json"));
    CHECK(tgcli::cli::render_human("read", result) == golden("read.txt"));
}
