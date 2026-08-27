#include "proto/operation.hpp"
#include "schema_matcher.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

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

json member() {
    return {{"kind", "member"}, {"member_until_date", 0}};
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

TEST_CASE("M6 family errors reject operation-detail cross products", "[m6][schema][error]") {
    CHECK_THAT(error("INTERNAL", {{"operation", "contact_list"},
                                  {"reason", "capacity_exhausted"},
                                  {"resource", "users"},
                                  {"limit", 131'072}}),
               matches_json_schema("contact.error.schema.json"));
    CHECK_THAT(error("INTERNAL", {{"operation", "contact_search"},
                                  {"reason", "capacity_exhausted"},
                                  {"resource", "users"},
                                  {"limit", 131'072}}),
               !matches_json_schema("contact.error.schema.json"));
    CHECK_THAT(error("INTERNAL", {{"operation", "topic_list"},
                                  {"reason", "capacity_exhausted"},
                                  {"resource", "users"},
                                  {"limit", 4'096}}),
               !matches_json_schema("topic.error.schema.json"));

    CHECK_THAT(error("PRECONDITION_FAILED", {{"operation", "folder_edit"},
                                             {"folder_id", 7},
                                             {"chat_id", nullptr},
                                             {"reason", "no_change"}}),
               matches_json_schema("folder.error.schema.json"));
    CHECK_THAT(error("PRECONDITION_FAILED", {{"operation", "folder_add_chat"},
                                             {"folder_id", 7},
                                             {"chat_id", nullptr},
                                             {"reason", "folder_capacity"}}),
               !matches_json_schema("folder.error.schema.json"));
    CHECK_THAT(error("PRECONDITION_FAILED", {{"operation", "topic_close"},
                                             {"chat_id", -1001},
                                             {"topic_id", 9},
                                             {"reason", "already_open"}}),
               !matches_json_schema("topic.error.schema.json"));
    CHECK_THAT(error("PRECONDITION_FAILED", {{"operation", "chat_invite_link"},
                                             {"chat_id", -1001},
                                             {"reason", "missing_right"},
                                             {"right", "change-info"}}),
               !matches_json_schema("chat-admin.error.schema.json"));

    const json folder_delete_plan{{"operation", "folder_delete"},
                                  {"account", "main"},
                                  {"tdlib_request", "deleteChatFolder"},
                                  {"folder", folder(true)},
                                  {"leave_chat_ids", json::array()}};
    const json storage_plan{{"operation", "storage_optimize"},
                            {"account", "main"},
                            {"tdlib_request", "optimizeStorage"},
                            {"size", -1},
                            {"ttl", -1},
                            {"count", -1},
                            {"immunity_delay", -1},
                            {"file_types", json::array()},
                            {"chat_ids", json::array()},
                            {"exclude_chat_ids", json::array()},
                            {"return_deleted_file_statistics", false},
                            {"chat_limit", 100}};
    CHECK_THAT(
        error("CONFIRMATION_REQUIRED",
              {{"account", "main"}, {"action", "folder_delete"}, {"target", folder_delete_plan}}),
        matches_json_schema("folder.error.schema.json"));
    CHECK_THAT(error("CONFIRMATION_REQUIRED",
                     {{"account", "main"}, {"action", "folder_delete"}, {"target", storage_plan}}),
               !matches_json_schema("folder.error.schema.json"));
}

TEST_CASE("M6 operation-correlated error arms reject every family cross product",
          "[m6][schema][error][cross-product]") {
    struct Donor {
        std::string filename;
        std::string code;
        json details;
        std::vector<std::string_view> allowed;
    };
    const std::vector<Donor> donors{
        {"contact.error.schema.json",
         "INTERNAL",
         {{"operation", "contact_list"},
          {"reason", "capacity_exhausted"},
          {"resource", "users"},
          {"limit", 131'072}},
         {"contact_list"}},
        {"contact.error.schema.json",
         "INTERNAL",
         {{"operation", "contact_search"},
          {"reason", "capacity_exhausted"},
          {"resource", "users"},
          {"limit", 100}},
         {"contact_search"}},
        {"contact.error.schema.json",
         "INTERNAL",
         {{"operation", "contact_list"},
          {"reason", "capacity_exhausted"},
          {"resource", "bytes"},
          {"limit", 16'777'216}},
         {"contact_list", "contact_search"}},
        {"folder.error.schema.json",
         "NOT_FOUND",
         {{"operation", "folder_show"}, {"folder_id", 7}},
         {"folder_show", "folder_edit", "folder_delete", "folder_add_chat", "folder_remove_chat"}},
        {"folder.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "folder_edit"},
          {"folder_id", 7},
          {"chat_id", nullptr},
          {"reason", "no_change"}},
         {"folder_edit"}},
        {"folder.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "folder_add_chat"},
          {"folder_id", 7},
          {"chat_id", -1001},
          {"reason", "already_in_folder"}},
         {"folder_add_chat"}},
        {"folder.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "folder_remove_chat"},
          {"folder_id", 7},
          {"chat_id", -1001},
          {"reason", "not_in_folder"}},
         {"folder_remove_chat"}},
        {"folder.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "folder_add_chat"},
          {"folder_id", 7},
          {"chat_id", -1001},
          {"reason", "folder_capacity"}},
         {"folder_add_chat", "folder_remove_chat"}},
        {"topic.error.schema.json",
         "NOT_FOUND",
         {{"operation", "topic_edit"}, {"chat_id", -1001}, {"topic_id", 9}},
         {"topic_edit", "topic_close", "topic_reopen"}},
        {"topic.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "topic_create"},
          {"chat_id", -1001},
          {"topic_id", nullptr},
          {"reason", "missing_right"}},
         {"topic_create"}},
        {"topic.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "topic_edit"},
          {"chat_id", -1001},
          {"topic_id", 9},
          {"reason", "no_change"}},
         {"topic_edit"}},
        {"topic.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "topic_close"},
          {"chat_id", -1001},
          {"topic_id", 9},
          {"reason", "already_closed"}},
         {"topic_close"}},
        {"topic.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "topic_reopen"},
          {"chat_id", -1001},
          {"topic_id", 9},
          {"reason", "already_open"}},
         {"topic_reopen"}},
        {"topic.error.schema.json",
         "PAGINATION_INVALID",
         {{"operation", "topic_list"}, {"reason", "non_advancing_upstream"}},
         {"topic_list"}},
        {"chat-admin.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "chat_set_title"},
          {"chat_id", -1001},
          {"reason", "missing_right"},
          {"right", "change-info"}},
         {"chat_set_title", "chat_set_photo", "chat_set_description"}},
        {"chat-admin.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "chat_invite_link"},
          {"chat_id", -1001},
          {"reason", "missing_right"},
          {"right", "invite-users"}},
         {"chat_invite_link"}},
        {"chat-admin.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "chat_promote"},
          {"chat_id", -1001},
          {"reason", "missing_right"},
          {"right", "promote-members"}},
         {"chat_promote", "chat_demote"}},
        {"chat-admin.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "chat_ban"},
          {"chat_id", -1001},
          {"reason", "missing_right"},
          {"right", "restrict-members"}},
         {"chat_ban", "chat_unban", "chat_kick", "chat_set_permissions"}},
        {"chat-admin.error.schema.json",
         "PRECONDITION_FAILED",
         {{"operation", "chat_ban"},
          {"chat_id", -1001},
          {"user_id", 77},
          {"reason", "wrong_member_state"}},
         {"chat_promote", "chat_demote", "chat_ban", "chat_unban", "chat_kick"}},
        {"chat-admin.error.schema.json",
         "INPUT_CHANGED",
         {{"operation", "chat_set_photo"}, {"path", "/tmp/photo.jpg"}},
         {"chat_set_photo"}},
    };
    const std::array<std::vector<std::string_view>, 4> operations{{
        {"contact_list", "contact_search", "contact_add", "contact_remove", "contact_block",
         "contact_unblock"},
        {"folder_list", "folder_show", "folder_create", "folder_edit", "folder_delete",
         "folder_add_chat", "folder_remove_chat"},
        {"topic_list", "topic_create", "topic_edit", "topic_close", "topic_reopen"},
        {"chat_set_title", "chat_set_photo", "chat_set_description", "chat_invite_link",
         "chat_promote", "chat_demote", "chat_ban", "chat_unban", "chat_kick",
         "chat_set_permissions"},
    }};
    const auto family_index = [](std::string_view filename) {
        if (filename.starts_with("contact")) {
            return std::size_t{0};
        }
        if (filename.starts_with("folder")) {
            return std::size_t{1};
        }
        if (filename.starts_with("topic")) {
            return std::size_t{2};
        }
        return std::size_t{3};
    };
    for (const auto& donor : donors) {
        CAPTURE(donor.filename, donor.code, donor.details);
        CHECK_THAT(error(donor.code, donor.details), matches_json_schema(donor.filename));
        for (const auto operation : operations.at(family_index(donor.filename))) {
            auto crossed = donor.details;
            crossed["operation"] = operation;
            const bool allowed = std::ranges::find(donor.allowed, operation) != donor.allowed.end();
            CAPTURE(operation, allowed);
            if (allowed) {
                CHECK_THAT(error(donor.code, crossed), matches_json_schema(donor.filename));
            } else {
                CHECK_THAT(error(donor.code, crossed), !matches_json_schema(donor.filename));
            }
        }
    }

    auto edit_nullability = donors[4].details;
    edit_nullability["chat_id"] = -1001;
    CHECK_THAT(error("PRECONDITION_FAILED", edit_nullability),
               !matches_json_schema("folder.error.schema.json"));
    auto add_nullability = donors[5].details;
    add_nullability["chat_id"] = nullptr;
    CHECK_THAT(error("PRECONDITION_FAILED", add_nullability),
               !matches_json_schema("folder.error.schema.json"));
    auto topic_nullability = donors[9].details;
    topic_nullability["topic_id"] = 9;
    CHECK_THAT(error("PRECONDITION_FAILED", topic_nullability),
               !matches_json_schema("topic.error.schema.json"));
}

TEST_CASE("M6 destructive confirmation arms correlate every action and target",
          "[m6][schema][error][cross-product]") {
    const json folder_delete{{"operation", "folder_delete"},
                             {"account", "main"},
                             {"tdlib_request", "deleteChatFolder"},
                             {"folder", folder(true)},
                             {"leave_chat_ids", json::array()}};
    const json invite_create{{"operation", "chat_invite_link"},
                             {"account", "main"},
                             {"tdlib_request", "createChatInviteLink"},
                             {"chat", chat()},
                             {"action", "create"},
                             {"invite_link_sha256", nullptr}};
    const json invite_revoke{{"operation", "chat_invite_link"},
                             {"account", "main"},
                             {"tdlib_request", "revokeChatInviteLink"},
                             {"chat", chat()},
                             {"action", "revoke"},
                             {"invite_link_sha256", "sha256:" + std::string(64, 'a')}};
    const json ban{
        {"operation", "chat_ban"}, {"account", "main"}, {"tdlib_request", "setChatMemberStatus"},
        {"chat", chat()},          {"user", user()},    {"before", member()},
        {"after", "banned"}};
    const json kick{
        {"operation", "chat_kick"}, {"account", "main"}, {"tdlib_request", "setChatMemberStatus"},
        {"chat", chat()},           {"user", user()},    {"before", member()},
        {"after", "left"}};
    const json optimize{{"operation", "storage_optimize"},
                        {"account", "main"},
                        {"tdlib_request", "optimizeStorage"},
                        {"size", -1},
                        {"ttl", -1},
                        {"count", -1},
                        {"immunity_delay", -1},
                        {"file_types", json::array()},
                        {"chat_ids", json::array()},
                        {"exclude_chat_ids", json::array()},
                        {"return_deleted_file_statistics", false},
                        {"chat_limit", 100}};
    struct Arm {
        std::string_view filename;
        std::string_view action;
        const json* target;
    };
    const std::array arms{Arm{"folder.error.schema.json", "folder_delete", &folder_delete},
                          Arm{"chat-admin.error.schema.json", "chat_invite_link", &invite_create},
                          Arm{"chat-admin.error.schema.json", "chat_ban", &ban},
                          Arm{"chat-admin.error.schema.json", "chat_kick", &kick},
                          Arm{"storage.error.schema.json", "storage_optimize", &optimize}};
    for (const auto& arm : arms) {
        const auto valid =
            error("CONFIRMATION_REQUIRED",
                  {{"account", "main"}, {"action", arm.action}, {"target", *arm.target}});
        CAPTURE(arm.filename, arm.action, *arm.target);
        CHECK_THAT(valid, matches_json_schema(std::string(arm.filename)));
        for (const auto& crossed : arms) {
            if (crossed.action == arm.action) {
                continue;
            }
            auto wrong_action = valid;
            wrong_action["error"]["details"]["action"] = crossed.action;
            CHECK_THAT(wrong_action, !matches_json_schema(std::string(arm.filename)));

            auto wrong_target = valid;
            wrong_target["error"]["details"]["target"] = *crossed.target;
            CHECK_THAT(wrong_target, !matches_json_schema(std::string(arm.filename)));
        }
    }

    CHECK_THAT(
        error("CONFIRMATION_REQUIRED",
              {{"account", "main"}, {"action", "chat_invite_link"}, {"target", invite_revoke}}),
        matches_json_schema("chat-admin.error.schema.json"));
    for (const auto& [field, value] : std::array<std::pair<std::string_view, json>, 3>{{
             {"tdlib_request", "revokeChatInviteLink"},
             {"action", "revoke"},
             {"invite_link_sha256", "sha256:" + std::string(64, 'a')},
         }}) {
        auto crossed = invite_create;
        crossed[field] = value;
        CHECK_THAT(
            error("CONFIRMATION_REQUIRED",
                  {{"account", "main"}, {"action", "chat_invite_link"}, {"target", crossed}}),
            !matches_json_schema("chat-admin.error.schema.json"));
    }
}

TEST_CASE("M6 ambiguity arms expose only reachable family candidate kinds",
          "[m6][schema][error][cross-product]") {
    const auto chat_ambiguity = error("AMBIGUOUS", {{"selector", "project"},
                                                    {"scope", "active_dialogs"},
                                                    {"candidates", json::array({chat()})},
                                                    {"truncated", false}});
    const auto user_ambiguity =
        error("AMBIGUOUS",
              {{"selector", "ada"}, {"candidates", json::array({user()})}, {"truncated", false}});

    CHECK_THAT(user_ambiguity, matches_json_schema("contact.error.schema.json"));
    CHECK_THAT(chat_ambiguity, !matches_json_schema("contact.error.schema.json"));
    for (const auto* filename : {"folder.error.schema.json", "topic.error.schema.json"}) {
        CAPTURE(filename);
        CHECK_THAT(chat_ambiguity, matches_json_schema(filename));
        CHECK_THAT(user_ambiguity, !matches_json_schema(filename));
    }
    CHECK_THAT(chat_ambiguity, matches_json_schema("chat-admin.error.schema.json"));
    CHECK_THAT(user_ambiguity, matches_json_schema("chat-admin.error.schema.json"));
    CHECK_THAT(chat_ambiguity, !matches_json_schema("storage.error.schema.json"));
    CHECK_THAT(user_ambiguity, !matches_json_schema("storage.error.schema.json"));
}
