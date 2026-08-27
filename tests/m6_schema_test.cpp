#include "proto/operation.hpp"
#include "schema_matcher.hpp"

#include <algorithm>
#include <array>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
using tgcli::test::matches_json_schema;

json chat() {
    return {{"id", -1001},
            {"title", "Project"},
            {"type", "supergroup"},
            {"is_bot", false},
            {"usernames", json::array({"project"})}};
}

json user() {
    return {{"id", 77},
            {"display_name", "Ada"},
            {"usernames", json::array({"ada"})},
            {"is_bot", false}};
}

std::string_view member_status(tgcli::proto::M6Operation operation) {
    using O = tgcli::proto::M6Operation;
    if (operation == O::ChatDemote) {
        return "member";
    }
    if (operation == O::ChatBan) {
        return "banned";
    }
    return "left";
}

json folder_name() {
    return {
        {"text", "Work"}, {"animate_custom_emoji", true}, {"custom_emoji_entities", json::array()}};
}

json folder(bool snapshot) {
    json value{
        {"id", 7},       {"name", folder_name()}, {"icon", snapshot ? json(nullptr) : json("work")},
        {"color_id", 2}, {"is_shareable", false}, {"has_my_invite_links", false}};
    if (snapshot) {
        value.update({{"pinned_chat_ids", json::array()},
                      {"included_chat_ids", json::array({-1001})},
                      {"excluded_chat_ids", json::array()},
                      {"exclude_muted", false},
                      {"exclude_read", false},
                      {"exclude_archived", false},
                      {"include_contacts", false},
                      {"include_non_contacts", false},
                      {"include_bots", false},
                      {"include_groups", false},
                      {"include_channels", false}});
    }
    return value;
}

json topic(bool row) {
    json value{{"chat_id", -1001},
               {"id", 9},
               {"name", "Updates"},
               {"icon", {{"color", "blue"}, {"custom_emoji_id", "0"}}},
               {"creation_date", "2026-08-27T12:00:00Z"},
               {"creator", {{"type", "user"}, {"id", 77}}},
               {"is_general", false},
               {"is_outgoing", true},
               {"is_closed", false},
               {"is_hidden", false},
               {"is_name_implicit", false}};
    if (row) {
        value.update({{"is_pinned", false},
                      {"unread_count", 0},
                      {"unread_mention_count", 0},
                      {"unread_reaction_count", 0},
                      {"unread_poll_vote_count", 0}});
    }
    return value;
}

json storage() {
    return {{"size", 0}, {"count", 0}, {"by_chat", json::array()}};
}

json result_for(tgcli::proto::M6Operation operation) {
    using O = tgcli::proto::M6Operation;
    switch (operation) {
    case O::ContactList:
    case O::ContactSearch:
        return {{"items", json::array({user()})}, {"next", nullptr}};
    case O::ContactAdd:
    case O::ContactRemove:
        return {{"user", user()}, {"is_contact", operation == O::ContactAdd}};
    case O::ContactBlock:
    case O::ContactUnblock:
        return {{"user", user()}, {"blocked", operation == O::ContactBlock}};
    case O::FolderList:
        return {{"items", json::array({folder(false)})}, {"next", nullptr}};
    case O::FolderShow:
    case O::FolderCreate:
    case O::FolderEdit:
        return {{"folder", folder(true)}};
    case O::FolderDelete:
        return {{"folder_id", 7}, {"deleted", true}};
    case O::FolderAddChat:
    case O::FolderRemoveChat:
        return {{"folder", folder(true)},
                {"chat", chat()},
                {"included", operation == O::FolderAddChat}};
    case O::TopicList:
        return {{"items", json::array({topic(true)})}, {"next", nullptr}};
    case O::TopicCreate:
        return {{"topic", topic(false)}};
    case O::TopicEdit:
        return {{"chat", chat()}, {"topic_id", 9}, {"name", "News"}};
    case O::TopicClose:
    case O::TopicReopen:
        return {{"chat", chat()}, {"topic_id", 9}, {"closed", operation == O::TopicClose}};
    case O::ChatSetTitle:
        return {{"chat", chat()}, {"title", "Renamed"}};
    case O::ChatSetPhoto:
        return {{"chat", chat()}, {"photo", "set"}};
    case O::ChatSetDescription:
        return {{"chat", chat()}, {"description", "Description"}};
    case O::ChatInviteLink:
        return {{"chat", chat()}, {"action", "create"}, {"invite_link", "https://t.me/+secret"}};
    case O::ChatPromote:
        return {{"chat", chat()},
                {"user", user()},
                {"status", "administrator"},
                {"can_manage_chat", true},
                {"rights", json::array({"change-info"})}};
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick:
        return {{"chat", chat()}, {"user", user()}, {"status", member_status(operation)}};
    case O::ChatSetPermissions:
        return {{"chat", chat()}, {"permissions", json::array()}};
    case O::StorageStats:
        return storage();
    case O::StorageOptimize:
        return {{"optimized", true}, {"statistics", storage()}};
    }
    return nullptr;
}

std::string result_filename(std::string_view command) {
    std::string filename(command);
    std::ranges::replace(filename, ' ', '-');
    return filename + ".result.schema.json";
}

json error(std::string code, json details) {
    return {{"error",
             {{"code", std::move(code)},
              {"message", "contract error"},
              {"details", std::move(details)}}}};
}

} // namespace

TEST_CASE("all M6 real results satisfy exactly one strict family asset", "[m6][schema]") {
    for (const auto& identity : tgcli::proto::m6_operation_identities()) {
        auto value = result_for(identity.operation);
        INFO(identity.command_path << ": " << value.dump());
        CHECK_THAT(value, matches_json_schema(result_filename(identity.command_path)));

        value["unexpected"] = true;
        CHECK_THAT(value, !matches_json_schema(result_filename(identity.command_path)));
    }
}

TEST_CASE("M6 family error assets close operation and detail ownership", "[m6][schema][error]") {
    struct Family {
        const char* filename;
        const char* operation;
    };
    constexpr std::array families{Family{"contact.error.schema.json", "contact_list"},
                                  Family{"folder.error.schema.json", "folder_list"},
                                  Family{"topic.error.schema.json", "topic_list"},
                                  Family{"chat-admin.error.schema.json", "chat_set_title"},
                                  Family{"storage.error.schema.json", "storage_stats"}};
    for (const auto& family : families) {
        const auto valid =
            error("INTERNAL", {{"operation", family.operation}, {"reason", "internal_error"}});
        CHECK_THAT(valid, matches_json_schema(family.filename));

        auto additional = valid;
        additional["error"]["details"]["extra"] = true;
        CHECK_THAT(additional, !matches_json_schema(family.filename));

        const auto cross_family =
            error("INTERNAL", {{"operation", "session_list"}, {"reason", "internal_error"}});
        CHECK_THAT(cross_family, !matches_json_schema(family.filename));
    }

    CHECK_THAT(error("NOT_FOUND", {{"operation", "folder_show"}, {"folder_id", 7}}),
               matches_json_schema("folder.error.schema.json"));
    CHECK_THAT(error("NOT_FOUND", {{"operation", "folder_list"}, {"folder_id", 7}}),
               !matches_json_schema("folder.error.schema.json"));
    CHECK_THAT(error("NOT_FOUND", {{"operation", "storage_stats"}}),
               !matches_json_schema("storage.error.schema.json"));
}
