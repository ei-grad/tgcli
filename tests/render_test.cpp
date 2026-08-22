#include "cli/render.hpp"
#include "schema_matcher.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

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

TEST_CASE("public M3 write renderers preserve complete results and plans", "[m3][render][golden]") {
    const json chat{{"id", -1001},
                    {"title", "Project"},
                    {"type", "supergroup"},
                    {"is_bot", false},
                    {"usernames", json::array({"project"})}};
    const json send_result{{"id", 321},
                           {"chat_id", -1001},
                           {"date", "2026-08-05T10:00:00Z"},
                           {"sender", {{"type", "user"}, {"id", 42}}},
                           {"is_outgoing", true},
                           {"topic", {{"kind", "forum"}, {"id", 7}}},
                           {"type", "text"},
                           {"text", "hello"},
                           {"scheduled", false}};
    CHECK_THAT(send_result, tgcli::test::matches_json_schema("send.result.schema.json"));
    CHECK(tgcli::cli::render_human("send", send_result) == golden("send.txt"));
    const json send_plan{{"operation", "send"},
                         {"account", "main"},
                         {"tdlib_request", "sendMessage"},
                         {"chat", chat},
                         {"text", "hello"},
                         {"parse_mode", "markdown_v2"},
                         {"reply_to", 123},
                         {"requested_topic", {{"kind", "forum"}, {"id", 7}}},
                         {"effective_topic", {{"kind", "forum"}, {"id", 7}}},
                         {"silent", true},
                         {"schedule", {{"kind", "at"}, {"send_date", 1785924000}}},
                         {"observed_server_unix_time", 1785923900}};
    const json send_dry{{"dry_run", true}, {"plan", send_plan}};
    CHECK_THAT(send_dry, tgcli::test::matches_json_schema("send.result.schema.json"));
    CHECK(tgcli::cli::render_human("send", send_dry) == golden("send-dry-run.txt"));

    const json saved_chat{{"id", 42},
                          {"title", "Ada"},
                          {"type", "private"},
                          {"is_bot", false},
                          {"usernames", json::array({"ada"})}};
    const json saved_result{{"id", 322},
                            {"chat_id", 42},
                            {"date", "2026-08-05T10:00:01Z"},
                            {"sender", {{"type", "user"}, {"id", 42}}},
                            {"is_outgoing", true},
                            {"topic", {{"kind", "saved"}, {"id", 19}}},
                            {"type", "doc"},
                            {"text", "experiment result"},
                            {"scheduled", false}};
    CHECK_THAT(saved_result, tgcli::test::matches_json_schema("saved-attach.result.schema.json"));
    CHECK(tgcli::cli::render_human("saved attach", saved_result) == golden("saved-attach.txt"));
    auto invalid_saved = saved_result;
    invalid_saved["type"] = "text";
    CHECK_THAT(invalid_saved, !tgcli::test::matches_json_schema("saved-attach.result.schema.json"));
    invalid_saved = saved_result;
    invalid_saved["scheduled"] = true;
    invalid_saved["date"] = nullptr;
    CHECK_THAT(invalid_saved, !tgcli::test::matches_json_schema("saved-attach.result.schema.json"));
    invalid_saved = saved_result;
    invalid_saved["topic"] = {{"kind", "forum"}, {"id", 19}};
    CHECK_THAT(invalid_saved, !tgcli::test::matches_json_schema("saved-attach.result.schema.json"));
    const json saved_plan{{"operation", "saved_attach"},
                          {"account", "main"},
                          {"tdlib_request", "sendMessage"},
                          {"chat", saved_chat},
                          {"message_id", -77},
                          {"effective_topic", {{"kind", "saved"}, {"id", 19}}},
                          {"caption", "experiment result"},
                          {"file",
                           {{"path", "/private/input.bin"},
                            {"name", "input.bin"},
                            {"size", 17},
                            {"sha256", "sha256:" + std::string(64, 'a')},
                            {"device", 1},
                            {"inode", 2},
                            {"mtime_ns", 3},
                            {"ctime_ns", 4}}}};
    const json saved_dry{{"dry_run", true}, {"plan", saved_plan}};
    CHECK_THAT(saved_dry, tgcli::test::matches_json_schema("saved-attach.result.schema.json"));
    CHECK(tgcli::cli::render_human("saved attach", saved_dry) ==
          golden("saved-attach-dry-run.txt"));
    auto invalid_saved_dry = saved_dry;
    invalid_saved_dry["plan"]["file"]["sha256"] = std::string(64, 'a');
    CHECK_THAT(invalid_saved_dry,
               !tgcli::test::matches_json_schema("saved-attach.result.schema.json"));

    const json delete_result{{"chat_id", -1001},
                             {"message_ids", json::array({-5, 7})},
                             {"for_all", true},
                             {"deleted", true}};
    CHECK_THAT(delete_result, tgcli::test::matches_json_schema("msg-delete.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg delete", delete_result) == golden("msg-delete.txt"));
    const json delete_plan{{"operation", "msg_delete"},
                           {"account", "main"},
                           {"tdlib_request", "deleteMessages"},
                           {"chat", chat},
                           {"message_ids", json::array({-5, 7})},
                           {"requested_for_all", true},
                           {"effective_for_all", true}};
    const json delete_dry{{"dry_run", true}, {"plan", delete_plan}};
    CHECK_THAT(delete_dry, tgcli::test::matches_json_schema("msg-delete.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg delete", delete_dry) == golden("msg-delete-dry-run.txt"));

    const json forward_result{
        {"from_chat_id", -1001},
        {"to_chat_id", -1002},
        {"items", json::array({json{{"source_id", -5},
                                    {"status", "sent"},
                                    {"message", json{{"id", 322},
                                                     {"chat_id", -1002},
                                                     {"date", "2026-08-05T10:00:01Z"},
                                                     {"sender", {{"type", "user"}, {"id", 42}}},
                                                     {"is_outgoing", true},
                                                     {"topic", nullptr},
                                                     {"type", "text"},
                                                     {"text", "forwarded"},
                                                     {"scheduled", false}}}}})}};
    CHECK_THAT(forward_result, tgcli::test::matches_json_schema("msg-forward.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg forward", forward_result) == golden("msg-forward.txt"));
    auto forward_to = chat;
    forward_to["id"] = -1002;
    forward_to["title"] = "Destination";
    forward_to["usernames"] = json::array({"destination"});
    const json forward_plan{{"operation", "msg_forward"},
                            {"account", "main"},
                            {"tdlib_request", "forwardMessages"},
                            {"from", chat},
                            {"to", forward_to},
                            {"message_ids", json::array({-5, 7})},
                            {"drop_author", true}};
    const json forward_dry{{"dry_run", true}, {"plan", forward_plan}};
    CHECK_THAT(forward_dry, tgcli::test::matches_json_schema("msg-forward.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg forward", forward_dry) ==
          golden("msg-forward-dry-run.txt"));

    const json& edit_result = send_result;
    CHECK_THAT(edit_result, tgcli::test::matches_json_schema("msg-edit.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg edit", edit_result) == golden("msg-edit.txt"));
    const json edit_plan{{"operation", "msg_edit"},
                         {"account", "main"},
                         {"tdlib_request", "editMessageText"},
                         {"chat", chat},
                         {"message_id", 321},
                         {"text", "hello"}};
    const json edit_dry{{"dry_run", true}, {"plan", edit_plan}};
    CHECK_THAT(edit_dry, tgcli::test::matches_json_schema("msg-edit.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg edit", edit_dry) == golden("msg-edit-dry-run.txt"));

    const json react_result{{"chat_id", -1001},
                            {"message_id", 321},
                            {"reaction", "👍"},
                            {"removed", false},
                            {"big", true}};
    CHECK_THAT(react_result, tgcli::test::matches_json_schema("msg-react.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg react", react_result) == golden("msg-react.txt"));
    const json react_plan{{"operation", "msg_react"},
                          {"account", "main"},
                          {"tdlib_request", "addMessageReaction"},
                          {"chat", chat},
                          {"message_id", 321},
                          {"reaction", "👍"},
                          {"remove", false},
                          {"big", true}};
    const json react_dry{{"dry_run", true}, {"plan", react_plan}};
    CHECK_THAT(react_dry, tgcli::test::matches_json_schema("msg-react.result.schema.json"));
    CHECK(tgcli::cli::render_human("msg react", react_dry) == golden("msg-react-dry-run.txt"));

    for (const auto& [command, operation, tdlib_request, pinned, schema, result_golden,
                      dry_golden] :
         std::vector<std::tuple<std::string_view, std::string_view, std::string_view, bool,
                                std::string_view, std::string_view, std::string_view>>{
             {"msg pin", "msg_pin", "pinChatMessage", true, "msg-pin.result.schema.json",
              "msg-pin.txt", "msg-pin-dry-run.txt"},
             {"msg unpin", "msg_unpin", "unpinChatMessage", false, "msg-unpin.result.schema.json",
              "msg-unpin.txt", "msg-unpin-dry-run.txt"}}) {
        const json pin_result{{"chat_id", -1001}, {"message_id", 321}, {"pinned", pinned}};
        CHECK_THAT(pin_result, tgcli::test::matches_json_schema(std::string(schema)));
        CHECK(tgcli::cli::render_human(std::string(command), pin_result) ==
              golden(std::string(result_golden)));
        const json pin_plan{
            {"operation", operation}, {"account", "main"}, {"tdlib_request", tdlib_request},
            {"chat", chat},           {"message_id", 321}, {"pinned", pinned}};
        const json pin_dry{{"dry_run", true}, {"plan", pin_plan}};
        CHECK_THAT(pin_dry, tgcli::test::matches_json_schema(std::string(schema)));
        CHECK(tgcli::cli::render_human(std::string(command), pin_dry) ==
              golden(std::string(dry_golden)));
    }

    const json mark_result{
        {"chat_id", -1001}, {"last_read_message_id", 321}, {"marked_read", true}};
    const json mark_plan{{"operation", "chat_mark_read"},
                         {"account", "main"},
                         {"tdlib_request", "viewMessages"},
                         {"chat", chat},
                         {"last_message_id", 321}};
    CHECK_THAT(mark_result, tgcli::test::matches_json_schema("chat-mark-read.result.schema.json"));
    CHECK(tgcli::cli::render_human("chat mark-read", mark_result) == golden("chat-mark-read.txt"));
    CHECK(tgcli::cli::render_human("chat mark-read", {{"dry_run", true}, {"plan", mark_plan}}) ==
          golden("chat-mark-read-dry-run.txt"));

    for (const auto& [command, operation, muted, duration, schema, result_golden, dry_golden] :
         std::vector<std::tuple<std::string_view, std::string_view, bool, std::int32_t,
                                std::string_view, std::string_view, std::string_view>>{
             {"chat mute", "chat_mute", true, 3600, "chat-mute.result.schema.json", "chat-mute.txt",
              "chat-mute-dry-run.txt"},
             {"chat unmute", "chat_unmute", false, 0, "chat-unmute.result.schema.json",
              "chat-unmute.txt", "chat-unmute-dry-run.txt"}}) {
        const json result{{"chat_id", -1001}, {"muted", muted}, {"duration_seconds", duration}};
        const json plan{{"operation", operation},
                        {"account", "main"},
                        {"tdlib_request", "setChatNotificationSettings"},
                        {"chat", chat},
                        {"muted", muted},
                        {"duration_seconds", duration}};
        CHECK_THAT(result, tgcli::test::matches_json_schema(std::string(schema)));
        CHECK(tgcli::cli::render_human(std::string(command), result) ==
              golden(std::string(result_golden)));
        CHECK(tgcli::cli::render_human(std::string(command), {{"dry_run", true}, {"plan", plan}}) ==
              golden(std::string(dry_golden)));
    }

    for (const auto& [command, operation, pinned, schema, result_golden, dry_golden] :
         std::vector<std::tuple<std::string_view, std::string_view, bool, std::string_view,
                                std::string_view, std::string_view>>{
             {"chat pin", "chat_pin", true, "chat-pin.result.schema.json", "chat-pin.txt",
              "chat-pin-dry-run.txt"},
             {"chat unpin", "chat_unpin", false, "chat-unpin.result.schema.json", "chat-unpin.txt",
              "chat-unpin-dry-run.txt"}}) {
        const json result{{"chat_id", -1001}, {"chat_list", "archive"}, {"pinned", pinned}};
        const json plan{{"operation", operation},
                        {"account", "main"},
                        {"tdlib_request", "toggleChatIsPinned"},
                        {"chat", chat},
                        {"chat_list", "archive"},
                        {"pinned", pinned}};
        CHECK_THAT(result, tgcli::test::matches_json_schema(std::string(schema)));
        CHECK(tgcli::cli::render_human(std::string(command), result) ==
              golden(std::string(result_golden)));
        CHECK(tgcli::cli::render_human(std::string(command), {{"dry_run", true}, {"plan", plan}}) ==
              golden(std::string(dry_golden)));
    }

    for (const auto& [command, operation, archived, schema, result_golden, dry_golden] :
         std::vector<std::tuple<std::string_view, std::string_view, bool, std::string_view,
                                std::string_view, std::string_view>>{
             {"chat archive", "chat_archive", true, "chat-archive.result.schema.json",
              "chat-archive.txt", "chat-archive-dry-run.txt"},
             {"chat unarchive", "chat_unarchive", false, "chat-unarchive.result.schema.json",
              "chat-unarchive.txt", "chat-unarchive-dry-run.txt"}}) {
        const json result{{"chat_id", -1001}, {"archived", archived}};
        const json plan{{"operation", operation},
                        {"account", "main"},
                        {"tdlib_request", "addChatToList"},
                        {"chat", chat},
                        {"archived", archived}};
        CHECK_THAT(result, tgcli::test::matches_json_schema(std::string(schema)));
        CHECK(tgcli::cli::render_human(std::string(command), result) ==
              golden(std::string(result_golden)));
        CHECK(tgcli::cli::render_human(std::string(command), {{"dry_run", true}, {"plan", plan}}) ==
              golden(std::string(dry_golden)));
    }

    const json join_result{{"status", "joined"}, {"chat_id", -1001}};
    const json join_plan{
        {"operation", "chat_join"}, {"account", "main"}, {"tdlib_request", "joinChat"},
        {"source", "username"},     {"chat", chat},      {"invite_link_sha256", nullptr}};
    CHECK_THAT(join_result, tgcli::test::matches_json_schema("chat-join.result.schema.json"));
    CHECK(tgcli::cli::render_human("chat join", join_result) == golden("chat-join.txt"));
    CHECK(tgcli::cli::render_human("chat join", {{"dry_run", true}, {"plan", join_plan}}) ==
          golden("chat-join-dry-run.txt"));

    const json leave_result{{"chat_id", -1001}, {"left", true}};
    const json leave_plan{{"operation", "chat_leave"},
                          {"account", "main"},
                          {"tdlib_request", "leaveChat"},
                          {"chat", chat}};
    CHECK_THAT(leave_result, tgcli::test::matches_json_schema("chat-leave.result.schema.json"));
    CHECK(tgcli::cli::render_human("chat leave", leave_result) == golden("chat-leave.txt"));
    CHECK(tgcli::cli::render_human("chat leave", {{"dry_run", true}, {"plan", leave_plan}}) ==
          golden("chat-leave-dry-run.txt"));
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

TEST_CASE("fetch human renderer preserves the strict result in its reviewed golden",
          "[fetch][render][golden]") {
    const json result{{"chat_id", -1001},
                      {"cached_count", 250},
                      {"oldest_message_id", 123},
                      {"target", {{"limit", 1000}, {"all", false}, {"since", nullptr}}},
                      {"target_reached", false},
                      {"stop_reason", "tdlib_idle"},
                      {"resume_from_message_id", 123}};
    CHECK_THAT(result, tgcli::test::matches_json_schema("fetch.result.schema.json"));
    CHECK(tgcli::cli::render_human("fetch", result) == golden("fetch.txt"));
}
