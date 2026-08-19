#include "schema_matcher.hpp"

#include <cstdint>
#include <filesystem>
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

json sender(std::string type = "user", std::int64_t id = 42) {
    return {{"type", std::move(type)}, {"id", id}};
}

json message_summary() {
    return {{"id", 123},
            {"chat_id", -1001},
            {"date", "2026-08-05T10:00:00Z"},
            {"sender", sender()},
            {"is_outgoing", false},
            {"topic", {{"kind", "forum"}, {"id", 7}}},
            {"type", "text"},
            {"text", "message or caption"}};
}

json chat_identity() {
    return {{"id", -1001},
            {"title", "Project"},
            {"type", "supergroup"},
            {"is_bot", false},
            {"usernames", json::array({"project"})}};
}

json private_chat_identity(bool is_bot) {
    auto result = chat_identity();
    result.update({{"id", 42},
                   {"title", is_bot ? "Build Bot" : "Ada"},
                   {"type", "private"},
                   {"is_bot", is_bot}});
    return result;
}

json chat_summary() {
    auto result = chat_identity();
    result.update({{"is_archived", false},
                   {"folder_ids", json::array({2})},
                   {"is_marked_unread", false},
                   {"unread_count", 3},
                   {"unread_mention_count", 1},
                   {"unread_reaction_count", 0},
                   {"unread_poll_vote_count", 0},
                   {"last_message", message_summary()}});
    return result;
}

json private_chat_summary(bool is_bot) {
    auto result = chat_summary();
    result.update(private_chat_identity(is_bot));
    return result;
}

json reaction(std::string_view type) {
    if (type == "emoji") {
        return {{"type", "emoji"}, {"emoji", "🧪"}};
    }
    if (type == "custom") {
        return {{"type", "custom"}, {"custom_emoji_id", "9223372036854775807"}};
    }
    return {{"type", "paid"}};
}

json terminal_error(std::string code, json details) {
    return {{"error",
             {{"code", std::move(code)},
              {"message", "contract error"},
              {"details", std::move(details)}}}};
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

std::vector<std::pair<std::string, json>> listen_items() {
    const json reaction_snapshot = {
        {"event", "reaction_snapshot"},
        {"chat_id", -1001},
        {"message_id", 123},
        {"reactions",
         {{"items", json::array({json{{"reaction", reaction("emoji")},
                                      {"total_count", 3},
                                      {"is_chosen", true},
                                      {"used_sender", sender()},
                                      {"recent_senders", json::array({sender("chat", -1002)})}},
                                 json{{"reaction", reaction("custom")},
                                      {"total_count", 1},
                                      {"is_chosen", false},
                                      {"used_sender", nullptr},
                                      {"recent_senders", json::array()}},
                                 json{{"reaction", reaction("paid")},
                                      {"total_count", 2},
                                      {"is_chosen", false},
                                      {"used_sender", nullptr},
                                      {"recent_senders", json::array()}}})},
          {"are_tags", false},
          {"can_get_added_reactions", true}}}};

    return {
        {"message", {{"event", "message"}, {"message", message_summary()}}},
        {"edit_content",
         {{"event", "edit_content"},
          {"chat_id", -1001},
          {"message_id", 123},
          {"content", {{"type", "photo"}, {"text", "replacement caption"}}}}},
        {"edit_metadata",
         {{"event", "edit_metadata"},
          {"chat_id", -1001},
          {"message_id", 123},
          {"edit_date", nullptr},
          {"has_reply_markup", false}}},
        {"reaction_snapshot", reaction_snapshot},
        {"reaction_snapshot_null",
         {{"event", "reaction_snapshot"},
          {"chat_id", -1001},
          {"message_id", 123},
          {"reactions", nullptr}}},
        {"bot_reaction_change",
         {{"event", "bot_reaction_change"},
          {"chat_id", -1001},
          {"message_id", 123},
          {"actor", sender("chat", -1002)},
          {"date", "2026-08-05T10:00:00Z"},
          {"old_reactions", json::array({reaction("emoji"), reaction("paid")})},
          {"new_reactions", json::array({reaction("custom")})}}},
        {"bot_reaction_snapshot",
         {{"event", "bot_reaction_snapshot"},
          {"chat_id", -1001},
          {"message_id", 123},
          {"date", "2026-08-05T10:00:00Z"},
          {"reactions", json::array({json{{"reaction", reaction("emoji")}, {"total_count", 3}},
                                     json{{"reaction", reaction("paid")}, {"total_count", 1}}})}}},
        {"delete_batch",
         {{"event", "delete_batch"},
          {"chat_id", -1001},
          {"message_ids", json::array({123, -124})},
          {"is_permanent", true},
          {"from_cache", false}}},
        {"chat_new", {{"event", "chat_change"}, {"change", "new"}, {"chat", chat_summary()}}},
        {"chat_identity",
         {{"event", "chat_change"}, {"change", "identity"}, {"chat", chat_identity()}}},
        {"chat_title",
         {{"event", "chat_change"},
          {"change", "title"},
          {"chat_id", -1001},
          {"title", "New title"}}},
        {"chat_last_message",
         {{"event", "chat_change"},
          {"change", "last_message"},
          {"chat_id", -1001},
          {"last_message", nullptr}}},
        {"chat_list_added",
         {{"event", "chat_change"},
          {"change", "list_added"},
          {"chat_id", -1001},
          {"list", {{"type", "folder"}, {"folder_id", 2}}}}},
        {"chat_list_removed",
         {{"event", "chat_change"},
          {"change", "list_removed"},
          {"chat_id", -1001},
          {"list", {{"type", "archive"}}}}},
        {"chat_read_inbox",
         {{"event", "chat_change"},
          {"change", "read_inbox"},
          {"chat_id", -1001},
          {"last_read_inbox_message_id", 0},
          {"unread_count", 2}}},
        {"chat_unread_mention_count",
         {{"event", "chat_change"},
          {"change", "unread_mention_count"},
          {"chat_id", -1001},
          {"unread_mention_count", 1}}},
        {"chat_unread_reaction_count",
         {{"event", "chat_change"},
          {"change", "unread_reaction_count"},
          {"chat_id", -1001},
          {"unread_reaction_count", 1}}},
        {"chat_unread_poll_vote_count",
         {{"event", "chat_change"},
          {"change", "unread_poll_vote_count"},
          {"chat_id", -1001},
          {"unread_poll_vote_count", 1}}},
        {"chat_marked_unread",
         {{"event", "chat_change"},
          {"change", "marked_unread"},
          {"chat_id", -1001},
          {"is_marked_unread", true}}},
        {"chat_new_private_user",
         {{"event", "chat_change"}, {"change", "new"}, {"chat", private_chat_summary(false)}}},
        {"chat_new_private_bot",
         {{"event", "chat_change"}, {"change", "new"}, {"chat", private_chat_summary(true)}}},
        {"chat_identity_private_user",
         {{"event", "chat_change"},
          {"change", "identity"},
          {"chat", private_chat_identity(false)}}},
        {"chat_identity_private_bot",
         {{"event", "chat_change"}, {"change", "identity"}, {"chat", private_chat_identity(true)}}},
    };
}

std::vector<std::pair<std::string, json>> inherited_stream_errors() {
    return {
        {"usage",
         terminal_error("USAGE", {{"argument", "--types"}, {"reason", "invalid_argument"}})},
        {"not_authed", terminal_error("NOT_AUTHED", {{"account", "main"},
                                                     {"state", "closing"},
                                                     {"reason", "authorization_lost"}})},
        {"bot_wait_for", terminal_error("BOT_UNSUPPORTED", {{"operation", "wait_for"}})},
        {"bot_resolve", terminal_error("BOT_UNSUPPORTED", {{"operation", "resolve"}})},
        {"not_found", terminal_error("NOT_FOUND", {{"selector", "missing"}})},
        {"not_found_local",
         terminal_error("NOT_FOUND", {{"selector", "@missing"}, {"scope", "local_materialized"}})},
        {"ambiguous_chat",
         terminal_error("AMBIGUOUS", {{"selector", "dev"},
                                      {"scope", "active_dialogs"},
                                      {"candidates", json::array({chat_identity()})},
                                      {"truncated", false}})},
        {"ambiguous_private_user",
         terminal_error("AMBIGUOUS", {{"selector", "Ada"},
                                      {"scope", "active_dialogs"},
                                      {"candidates", json::array({private_chat_identity(false)})},
                                      {"truncated", false}})},
        {"ambiguous_private_bot",
         terminal_error("AMBIGUOUS", {{"selector", "Build Bot"},
                                      {"scope", "active_dialogs"},
                                      {"candidates", json::array({private_chat_identity(true)})},
                                      {"truncated", false}})},
        {"ambiguous_user",
         terminal_error("AMBIGUOUS",
                        {{"selector", "Ada"},
                         {"candidates", json::array({json{{"id", 42},
                                                          {"display_name", "Ada Lovelace"},
                                                          {"usernames", json::array({"ada"})},
                                                          {"is_bot", false}}})},
                         {"truncated", false}})},
        {"rate_limited",
         terminal_error("RATE_LIMITED",
                        {{"operation", "resolve"}, {"tdlib_code", 429}, {"retry_after", 5}})},
        {"tdlib_error",
         terminal_error("TDLIB_ERROR", {{"operation", "listen"}, {"tdlib_code", 400}})},
        {"timeout", terminal_error("TIMEOUT", {{"operation", "wait_for"}, {"state", nullptr}})},
        {"pagination_invalid",
         terminal_error("PAGINATION_INVALID",
                        {{"operation", "wait_for"}, {"reason", "non_advancing_upstream"}})},
        {"daemon_shutdown", terminal_error("DAEMON_SHUTDOWN", {{"reason", "daemon_shutdown"}})},
        {"internal",
         terminal_error("INTERNAL", {{"operation", "listen"}, {"reason", "internal_error"}})},
    };
}

std::vector<std::pair<std::string, json>> overflow_errors() {
    return {
        {"queue_items", terminal_error("STREAM_OVERFLOW", {{"operation", "listen"},
                                                           {"cause", "queue_items"},
                                                           {"limit_items", 1024},
                                                           {"limit_bytes", 8388608},
                                                           {"queued_items", 1024},
                                                           {"queued_bytes", 8000},
                                                           {"incoming_bytes", 64}})},
        {"queue_bytes", terminal_error("STREAM_OVERFLOW", {{"operation", "wait_for"},
                                                           {"cause", "queue_bytes"},
                                                           {"limit_items", 1024},
                                                           {"limit_bytes", 8388608},
                                                           {"queued_items", 800},
                                                           {"queued_bytes", 8380000},
                                                           {"incoming_bytes", 12000}})},
        {"item_bytes", terminal_error("STREAM_OVERFLOW", {{"operation", "listen"},
                                                          {"cause", "item_bytes"},
                                                          {"limit_bytes", 262144},
                                                          {"incoming_bytes", 262145}})},
        {"history_overlap", terminal_error("STREAM_OVERFLOW", {{"operation", "wait_for"},
                                                               {"cause", "history_overlap"},
                                                               {"limit_items", 1024},
                                                               {"limit_bytes", 8388608},
                                                               {"queued_items", 1024},
                                                               {"queued_bytes", 8388608},
                                                               {"incoming_bytes", 64}})},
        {"counter_exhausted", terminal_error("STREAM_OVERFLOW", {{"operation", "wait_for"},
                                                                 {"cause", "counter_exhausted"}})},
    };
}

std::vector<std::pair<std::string, json>> capacity_errors() {
    std::vector<std::pair<std::string, json>> result{
        {"subscriber_slots", terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                                                {"phase", "admission"},
                                                                {"resource", "subscriber_slots"},
                                                                {"limit", 32}})},
        {"lock_free_ingress", terminal_error("STREAM_CAPACITY", {{"operation", "wait_for"},
                                                                 {"phase", "admission"},
                                                                 {"resource", "lock_free_ingress"},
                                                                 {"atomic", "terminal_cause"}})},
        {"metadata_bootstrap_items",
         terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                            {"phase", "bootstrap"},
                                            {"resource", "metadata_bootstrap_items"},
                                            {"limit_items", 4096},
                                            {"used_items", 4096},
                                            {"incoming_items", 1}})},
        {"metadata_bootstrap_bytes",
         terminal_error("STREAM_CAPACITY", {{"operation", "wait_for"},
                                            {"phase", "bootstrap"},
                                            {"resource", "metadata_bootstrap_bytes"},
                                            {"limit_bytes", 16777216},
                                            {"would_use_bytes", 16777217}})},
        {"metadata_order_items",
         terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                            {"phase", "active"},
                                            {"resource", "metadata_order_items"},
                                            {"limit_items", 4096},
                                            {"used_items", 4096},
                                            {"incoming_items", 1}})},
        {"metadata_order_bytes",
         terminal_error("STREAM_CAPACITY", {{"operation", "wait_for"},
                                            {"phase", "active"},
                                            {"resource", "metadata_order_bytes"},
                                            {"limit_bytes", 16777216},
                                            {"would_use_bytes", 16777217}})},
        {"metadata_item_bytes",
         terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                            {"phase", "active"},
                                            {"resource", "metadata_item_bytes"},
                                            {"limit_bytes", 262144},
                                            {"incoming_bytes", 262145}})},
    };

    for (const auto& phase : {"bootstrap", "active"}) {
        result.emplace_back(std::string("metadata_chats_") + phase,
                            terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                                               {"phase", phase},
                                                               {"resource", "metadata_chats"},
                                                               {"limit", 65536},
                                                               {"used", 65536},
                                                               {"incoming", 1}}));
        result.emplace_back(std::string("metadata_entities_") + phase,
                            terminal_error("STREAM_CAPACITY", {{"operation", "wait_for"},
                                                               {"phase", phase},
                                                               {"resource", "metadata_entities"},
                                                               {"limit", 131072},
                                                               {"used", 131072},
                                                               {"incoming", 1}}));
        result.emplace_back(std::string("metadata_bytes_") + phase,
                            terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                                               {"phase", phase},
                                                               {"resource", "metadata_bytes"},
                                                               {"limit_bytes", 67108864},
                                                               {"would_use_bytes", 67108865}}));
    }
    for (const auto& atomic :
         {"slot_pointer", "publisher_count", "descriptor_index", "byte_index"}) {
        result.emplace_back(std::string("lock_free_ingress_") + atomic,
                            terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                                               {"phase", "admission"},
                                                               {"resource", "lock_free_ingress"},
                                                               {"atomic", atomic}}));
    }
    constexpr auto kMaximumBytes = std::numeric_limits<std::uint64_t>::max();
    result.emplace_back("metadata_bootstrap_bytes_max",
                        terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                                           {"phase", "bootstrap"},
                                                           {"resource", "metadata_bootstrap_bytes"},
                                                           {"limit_bytes", 16777216},
                                                           {"would_use_bytes", kMaximumBytes}}));
    result.emplace_back("metadata_order_bytes_max",
                        terminal_error("STREAM_CAPACITY", {{"operation", "wait_for"},
                                                           {"phase", "active"},
                                                           {"resource", "metadata_order_bytes"},
                                                           {"limit_bytes", 16777216},
                                                           {"would_use_bytes", kMaximumBytes}}));
    result.emplace_back("metadata_item_bytes_max",
                        terminal_error("STREAM_CAPACITY", {{"operation", "listen"},
                                                           {"phase", "active"},
                                                           {"resource", "metadata_item_bytes"},
                                                           {"limit_bytes", 262144},
                                                           {"incoming_bytes", kMaximumBytes}}));
    for (const auto& phase : {"bootstrap", "active"}) {
        result.emplace_back(
            std::string("metadata_bytes_") + phase + "_max",
            terminal_error("STREAM_CAPACITY", {{"operation", "wait_for"},
                                               {"phase", phase},
                                               {"resource", "metadata_bytes"},
                                               {"limit_bytes", 67108864},
                                               {"would_use_bytes", kMaximumBytes}}));
    }
    return result;
}

} // namespace

TEST_CASE("stream catalog has an exact packaged-schema bijection", "[schema][stream][catalog]") {
    const auto catalog = tgcli::test::load_schema_document("stream-manifest.json");
    const json expected{
        {"schemaDialect", kDialect},
        {"commands",
         {{"listen", {{"item", "listen.item.schema.json"}, {"error", "stream.error.schema.json"}}},
          {"wait-for",
           {{"result", "wait-for.result.schema.json"}, {"error", "stream.error.schema.json"}}}}}};
    CHECK(catalog == expected);

    std::set<std::string> referenced;
    for (const auto& [command, contracts] : catalog.at("commands").items()) {
        CAPTURE(command);
        REQUIRE(contracts.is_object());
        for (const auto& [kind, filename] : contracts.items()) {
            CAPTURE(kind);
            REQUIRE(filename.is_string());
            referenced.insert(filename.get<std::string>());
        }
    }
    CHECK(referenced == std::set<std::string>{"listen.item.schema.json", "stream.error.schema.json",
                                              "wait-for.result.schema.json"});

    const auto schema_directory = tgcli::test::schema_path("stream-manifest.json").parent_path();
    for (const auto& filename : referenced) {
        const auto file = schema_directory / filename;
        CHECK(std::filesystem::is_regular_file(file));
        CHECK_FALSE(std::filesystem::is_symlink(file));
    }

    std::set<std::string> stream_files;
    for (const auto& entry : std::filesystem::directory_iterator(schema_directory)) {
        const auto filename = entry.path().filename().string();
        if (filename == "stream-manifest.json" || filename.starts_with("listen.") ||
            filename.starts_with("wait-for.") || filename == "stream.error.schema.json") {
            stream_files.insert(filename);
        }
    }
    auto expected_files = referenced;
    expected_files.insert("stream-manifest.json");
    CHECK(stream_files == expected_files);

    const auto result_manifest = tgcli::test::load_schema_document("manifest.json");
    CHECK(result_manifest["commands"]["wait-for"] ==
          json{{"result", "wait-for.result.schema.json"}});
    CHECK_FALSE(result_manifest["commands"].contains("listen"));
}

TEST_CASE("stream schemas use the strict local Draft 2020-12 subset", "[schema][stream]") {
    for (const auto* filename :
         {"listen.item.schema.json", "wait-for.result.schema.json", "stream.error.schema.json"}) {
        CAPTURE(filename);
        const auto schema = tgcli::test::load_schema_document(filename);
        REQUIRE(schema.contains("$schema"));
        CHECK(schema["$schema"] == kDialect);
        check_schema_node(schema);
    }
}

TEST_CASE("wait-for result is exactly the shared MessageSummary", "[schema][stream][wait-for]") {
    std::vector<std::pair<std::string, json>> cases;
    for (const auto& topic :
         {json{{"kind", "forum"}, {"id", 1}}, json{{"kind", "thread"}, {"id", 2}},
          json{{"kind", "direct"}, {"id", 3}}, json{{"kind", "saved"}, {"id", 4}}, json(nullptr)}) {
        auto message = message_summary();
        message["topic"] = topic;
        cases.emplace_back(topic.is_null() ? "null" : topic["kind"].get<std::string>(),
                           std::move(message));
    }
    for (const auto& type : {"text", "photo", "video", "doc", "voice", "other"}) {
        auto message = message_summary();
        message["type"] = type;
        cases.emplace_back(type, std::move(message));
    }
    auto chat_sender = message_summary();
    chat_sender["sender"] = sender("chat", -1002);
    chat_sender["date"] = nullptr;
    chat_sender["id"] = -5;
    cases.emplace_back("chat_sender_null_date", std::move(chat_sender));
    auto int53_boundaries = message_summary();
    int53_boundaries["id"] = -9007199254740991LL;
    int53_boundaries["chat_id"] = 9007199254740991LL;
    int53_boundaries["sender"]["id"] = 9007199254740991LL;
    cases.emplace_back("int53_boundaries", std::move(int53_boundaries));

    for (const auto& [name, message] : cases) {
        CAPTURE(name);
        CHECK_THAT(message, tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    }

    auto missing = message_summary();
    missing.erase("sender");
    CHECK_THAT(missing, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    auto unknown = message_summary();
    unknown["receive_sequence"] = 10;
    CHECK_THAT(unknown, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    auto nested_unknown = message_summary();
    nested_unknown["sender"]["name"] = "Ada";
    CHECK_THAT(nested_unknown, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    auto invalid_topic = message_summary();
    invalid_topic["topic"] = {{"kind", "forum"}, {"id", 2147483648LL}};
    CHECK_THAT(invalid_topic, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    auto invalid_date = message_summary();
    invalid_date["date"] = "2026-02-29T10:00:00Z";
    CHECK_THAT(invalid_date, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    auto zero_id = message_summary();
    zero_id["id"] = 0;
    CHECK_THAT(zero_id, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    auto invalid_user = message_summary();
    invalid_user["sender"]["id"] = -1;
    CHECK_THAT(invalid_user, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
    const json saved_only{{"id", 1},
                          {"chat_id", 2},
                          {"date", "2026-08-05T10:00:00Z"},
                          {"text", "narrow Saved summary"}};
    CHECK_THAT(saved_only, !tgcli::test::matches_json_schema("wait-for.result.schema.json"));
}

TEST_CASE("listen item schema covers every event and chat-change branch",
          "[schema][stream][listen]") {
    const auto cases = listen_items();
    for (const auto& [name, item] : cases) {
        CAPTURE(name);
        CHECK_THAT(item, tgcli::test::matches_json_schema("listen.item.schema.json"));

        auto missing_discriminator = item;
        missing_discriminator.erase("event");
        CHECK_THAT(missing_discriminator,
                   !tgcli::test::matches_json_schema("listen.item.schema.json"));

        auto unknown = item;
        unknown["receive_sequence"] = 7;
        CHECK_THAT(unknown, !tgcli::test::matches_json_schema("listen.item.schema.json"));
    }

    auto missing_payload = cases.front().second;
    missing_payload.erase("message");
    CHECK_THAT(missing_payload, !tgcli::test::matches_json_schema("listen.item.schema.json"));
    auto nested_unknown = cases.front().second;
    nested_unknown["message"]["raw"] = json::object();
    CHECK_THAT(nested_unknown, !tgcli::test::matches_json_schema("listen.item.schema.json"));
    auto cross_event = cases.front().second;
    cross_event["event"] = "edit_content";
    CHECK_THAT(cross_event, !tgcli::test::matches_json_schema("listen.item.schema.json"));
    auto cross_change = cases.at(8).second;
    cross_change["change"] = "identity";
    CHECK_THAT(cross_change, !tgcli::test::matches_json_schema("listen.item.schema.json"));
    auto invalid_reaction = cases.at(3).second;
    invalid_reaction["reactions"]["items"][0]["reaction"] =
        json{{"type", "custom"}, {"custom_emoji_id", "9223372036854775808"}};
    CHECK_THAT(invalid_reaction, !tgcli::test::matches_json_schema("listen.item.schema.json"));
    for (const auto& invalid_id : {json("0"), json("01"), json("-1"), json(1)}) {
        auto invalid_custom = cases.at(3).second;
        invalid_custom["reactions"]["items"][0]["reaction"] =
            json{{"type", "custom"}, {"custom_emoji_id", invalid_id}};
        CHECK_THAT(invalid_custom, !tgcli::test::matches_json_schema("listen.item.schema.json"));
    }
    auto wrong_chat_list = cases.at(13).second;
    wrong_chat_list["list"]["folder_id"] = 2;
    CHECK_THAT(wrong_chat_list, !tgcli::test::matches_json_schema("listen.item.schema.json"));

    auto non_private_bot_identity = cases.at(9).second;
    non_private_bot_identity["chat"]["is_bot"] = true;
    CHECK_THAT(non_private_bot_identity,
               !tgcli::test::matches_json_schema("listen.item.schema.json"));
    auto non_private_bot_summary = cases.at(8).second;
    non_private_bot_summary["chat"]["is_bot"] = true;
    CHECK_THAT(non_private_bot_summary,
               !tgcli::test::matches_json_schema("listen.item.schema.json"));
}

TEST_CASE("stream error schema accepts every inherited and overflow branch",
          "[schema][stream][error]") {
    for (const auto& [name, error] : inherited_stream_errors()) {
        CAPTURE(name);
        CHECK_THAT(error, tgcli::test::matches_json_schema("stream.error.schema.json"));
    }
    for (const auto& [name, error] : overflow_errors()) {
        CAPTURE(name);
        CHECK_THAT(error, tgcli::test::matches_json_schema("stream.error.schema.json"));

        auto unknown = error;
        unknown["error"]["details"]["unexpected"] = 1;
        CHECK_THAT(unknown, !tgcli::test::matches_json_schema("stream.error.schema.json"));

        auto missing = error;
        missing["error"]["details"].erase("cause");
        CHECK_THAT(missing, !tgcli::test::matches_json_schema("stream.error.schema.json"));

        for (const auto* limit : {"limit_items", "limit_bytes"}) {
            if (!error["error"]["details"].contains(limit)) {
                continue;
            }
            auto wrong_constant = error;
            ++wrong_constant["error"]["details"][limit].get_ref<json::number_integer_t&>();
            CHECK_THAT(wrong_constant,
                       !tgcli::test::matches_json_schema("stream.error.schema.json"));
        }
    }

    auto cross_history = overflow_errors().at(3).second;
    cross_history["error"]["details"]["operation"] = "listen";
    CHECK_THAT(cross_history, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto cross_item = overflow_errors().at(2).second;
    cross_item["error"]["details"]["cause"] = "queue_bytes";
    CHECK_THAT(cross_item, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto unknown_code = terminal_error("STREAM_DROPPED", {{"operation", "listen"}});
    CHECK_THAT(unknown_code, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto inherited_unknown = inherited_stream_errors().front().second;
    inherited_unknown["error"]["details"]["unexpected"] = true;
    CHECK_THAT(inherited_unknown, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto inherited_missing = inherited_stream_errors().front().second;
    inherited_missing["error"].erase("details");
    CHECK_THAT(inherited_missing, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto unknown_envelope = inherited_stream_errors().front().second;
    unknown_envelope["request_id"] = 1;
    CHECK_THAT(unknown_envelope, !tgcli::test::matches_json_schema("stream.error.schema.json"));

    auto non_private_bot_candidate =
        terminal_error("AMBIGUOUS", {{"selector", "dev"},
                                     {"scope", "active_dialogs"},
                                     {"candidates", json::array({chat_identity()})},
                                     {"truncated", false}});
    non_private_bot_candidate["error"]["details"]["candidates"][0]["is_bot"] = true;
    CHECK_THAT(non_private_bot_candidate,
               !tgcli::test::matches_json_schema("stream.error.schema.json"));
}

TEST_CASE("stream capacity schema covers every exact resource and phase",
          "[schema][stream][capacity]") {
    for (const auto& [name, error] : capacity_errors()) {
        CAPTURE(name);
        CHECK_THAT(error, tgcli::test::matches_json_schema("stream.error.schema.json"));

        auto unknown = error;
        unknown["error"]["details"]["unexpected"] = 1;
        CHECK_THAT(unknown, !tgcli::test::matches_json_schema("stream.error.schema.json"));

        auto missing = error;
        missing["error"]["details"].erase("resource");
        CHECK_THAT(missing, !tgcli::test::matches_json_schema("stream.error.schema.json"));

        const auto& details = error["error"]["details"];
        if (details.contains("used_items")) {
            auto non_exhausting = error;
            non_exhausting["error"]["details"]["used_items"] =
                details["limit_items"].get<std::int64_t>() - 1;
            CHECK_THAT(non_exhausting,
                       !tgcli::test::matches_json_schema("stream.error.schema.json"));
        }
        if (details.contains("used")) {
            auto non_exhausting = error;
            non_exhausting["error"]["details"]["used"] = details["limit"].get<std::int64_t>() - 1;
            CHECK_THAT(non_exhausting,
                       !tgcli::test::matches_json_schema("stream.error.schema.json"));
        }
        if (details.contains("would_use_bytes")) {
            auto non_overflow = error;
            non_overflow["error"]["details"]["would_use_bytes"] = details["limit_bytes"];
            CHECK_THAT(non_overflow, !tgcli::test::matches_json_schema("stream.error.schema.json"));

            auto zero = error;
            zero["error"]["details"]["would_use_bytes"] = 0;
            CHECK_THAT(zero, !tgcli::test::matches_json_schema("stream.error.schema.json"));

            auto legacy_tuple = error;
            legacy_tuple["error"]["details"]["used_bytes"] = 1;
            legacy_tuple["error"]["details"]["incoming_bytes"] = 1;
            CHECK_THAT(legacy_tuple, !tgcli::test::matches_json_schema("stream.error.schema.json"));
        }
        if (details["resource"] == "metadata_item_bytes") {
            auto non_overflow = error;
            non_overflow["error"]["details"]["incoming_bytes"] = details["limit_bytes"];
            CHECK_THAT(non_overflow, !tgcli::test::matches_json_schema("stream.error.schema.json"));

            auto zero = error;
            zero["error"]["details"]["incoming_bytes"] = 0;
            CHECK_THAT(zero, !tgcli::test::matches_json_schema("stream.error.schema.json"));
        }

        for (const auto* limit : {"limit", "limit_items", "limit_bytes"}) {
            if (!error["error"]["details"].contains(limit)) {
                continue;
            }
            auto wrong_constant = error;
            ++wrong_constant["error"]["details"][limit].get_ref<json::number_integer_t&>();
            CHECK_THAT(wrong_constant,
                       !tgcli::test::matches_json_schema("stream.error.schema.json"));
        }
    }

    auto wrong_limit = capacity_errors().front().second;
    wrong_limit["error"]["details"]["limit"] = 31;
    CHECK_THAT(wrong_limit, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto cross_admission = capacity_errors().at(1).second;
    cross_admission["error"]["details"]["limit"] = 32;
    CHECK_THAT(cross_admission, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto cross_phase = capacity_errors().at(4).second;
    cross_phase["error"]["details"]["phase"] = "bootstrap";
    CHECK_THAT(cross_phase, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto invalid_atomic = capacity_errors().at(1).second;
    invalid_atomic["error"]["details"]["atomic"] = "mutex";
    CHECK_THAT(invalid_atomic, !tgcli::test::matches_json_schema("stream.error.schema.json"));
    auto invalid_item_size = capacity_errors().at(6).second;
    invalid_item_size["error"]["details"]["incoming_bytes"] = 262144;
    CHECK_THAT(invalid_item_size, !tgcli::test::matches_json_schema("stream.error.schema.json"));
}
