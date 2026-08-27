#include "proto/destructive_plan.hpp"
#include "proto/frame.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace tgcli::proto;

namespace {

constexpr auto kSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
    "ino:2;size:3;ctime_ns:4";

json logout_document() {
    return {{"operation", "logout"},
            {"account", "main"},
            {"remote_logout", true},
            {"tdlib_request", "logOut"}};
}

json removal_document() {
    return {{"operation", "account_remove"},
            {"account", "work"},
            {"remote_logout", true},
            {"keep_session", false},
            {"delete_paths", json::array({"/data/accounts/work", "/state/accounts/work"})},
            {"config_path", "/config/tgcli/config.toml"},
            {"config_snapshot", kSnapshot},
            {"data_root",
             {{"path", "/data/accounts/work"},
              {"device", std::uint64_t{1}},
              {"inode", std::uint64_t{2}},
              {"owner", std::uint64_t{1000}}}},
            {"state_root",
             {{"path", "/state/accounts/work"},
              {"device", std::uint64_t{1}},
              {"inode", std::uint64_t{3}},
              {"owner", std::uint64_t{1000}}}},
            {"reassign_default", "main"}};
}

AccountRemovePlanInput removal_input() {
    return {
        .account = "work",
        .keep_session = false,
        .delete_paths = {"/data/accounts/work", "/state/accounts/work"},
        .config_path = "/config/tgcli/config.toml",
        .config_snapshot = kSnapshot,
        .data_root = RootIdentity{"/data/accounts/work", 1, 2, 1000},
        .state_root = RootIdentity{"/state/accounts/work", 1, 3, 1000},
        .reassign_default = "main",
    };
}

json destructive_challenge(std::string action, json target) {
    return {{"kind", "destructive_confirmation"},
            {"nonce", "00112233445566778899aabbccddeeff"},
            {"sequence", 1},
            {"client_generation", nullptr},
            {"auth_sequence", nullptr},
            {"secret", false},
            {"prompt", "Confirm? [y/N] "},
            {"details", {{"action", std::move(action)}, {"target", std::move(target)}}}};
}

std::vector<std::pair<std::string, json>> m6_destructive_documents() {
    const json chat{{"id", -1001},
                    {"title", "Project"},
                    {"type", "supergroup"},
                    {"is_bot", false},
                    {"usernames", json::array({"project"})}};
    const json user{{"id", 77},
                    {"display_name", "Peer"},
                    {"usernames", json::array({"peer"})},
                    {"is_bot", false}};
    const json status{{"kind", "member"}, {"member_until_date", 0}};
    const json folder{{"id", 7},
                      {"name",
                       {{"text", "Work"},
                        {"animate_custom_emoji", false},
                        {"custom_emoji_entities", json::array()}}},
                      {"icon", "work"},
                      {"color_id", 2},
                      {"is_shareable", false},
                      {"has_my_invite_links", false},
                      {"pinned_chat_ids", json::array()},
                      {"included_chat_ids", json::array({-1001})},
                      {"excluded_chat_ids", json::array()},
                      {"exclude_muted", false},
                      {"exclude_read", false},
                      {"exclude_archived", false},
                      {"include_contacts", false},
                      {"include_non_contacts", false},
                      {"include_bots", false},
                      {"include_groups", false},
                      {"include_channels", false}};
    return {
        {"folder_delete",
         {{"operation", "folder_delete"},
          {"account", "main"},
          {"tdlib_request", "deleteChatFolder"},
          {"folder", folder},
          {"leave_chat_ids", json::array()}}},
        {"chat_invite_link",
         {{"operation", "chat_invite_link"},
          {"account", "main"},
          {"tdlib_request", "createChatInviteLink"},
          {"chat", chat},
          {"action", "create"},
          {"invite_link_sha256", nullptr}}},
        {"chat_ban",
         {{"operation", "chat_ban"},
          {"account", "main"},
          {"tdlib_request", "setChatMemberStatus"},
          {"chat", chat},
          {"user", user},
          {"before", status},
          {"after", "banned"}}},
        {"chat_kick",
         {{"operation", "chat_kick"},
          {"account", "main"},
          {"tdlib_request", "setChatMemberStatus"},
          {"chat", chat},
          {"user", user},
          {"before", status},
          {"after", "left"}}},
        {"storage_optimize",
         {{"operation", "storage_optimize"},
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
          {"chat_limit", 100}}},
        {"session_terminate",
         {{"operation", "session_terminate"},
          {"account", "main"},
          {"tdlib_request", "terminateSession"},
          {"session",
           {{"id", "0"},
            {"is_current", false},
            {"is_password_pending", false},
            {"is_unconfirmed", false},
            {"device_type", "linux"},
            {"application_name", "tgcli"},
            {"application_version", "1"},
            {"device_model", "PC"},
            {"platform", "Linux"},
            {"system_version", "6"},
            {"last_active_date", nullptr}}}}},
    };
}

} // namespace

TEST_CASE("destructive plans parse and serialize exact closed shapes", "[destructive][plan]") {
    std::string error;
    auto logout = parse_logout_plan(logout_document(), error);
    INFO(error);
    REQUIRE(logout.has_value());
    CHECK(serialize(*logout) == logout_document());
    auto logout_round_trip = parse_destructive_plan(serialize(*logout), error);
    REQUIRE(logout_round_trip.has_value());
    CHECK(std::get<LogoutPlan>(*logout_round_trip) == *logout);

    auto removal = parse_account_remove_plan(removal_document(), error);
    INFO(error);
    REQUIRE(removal.has_value());
    CHECK(serialize(*removal) == removal_document());
    auto removal_round_trip = parse_destructive_plan(serialize(*removal), error);
    REQUIRE(removal_round_trip.has_value());
    CHECK(std::get<AccountRemovePlan>(*removal_round_trip) == *removal);
}

TEST_CASE("logout plan rejects every shape and constant deviation", "[destructive][plan]") {
    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value.erase("account"); },
             [](json& value) { value["extra"] = true; },
             [](json& value) { value["operation"] = "account_remove"; },
             [](json& value) { value["operation"] = 1; },
             [](json& value) { value["account"] = "../main"; },
             [](json& value) { value["account"] = false; },
             [](json& value) { value["remote_logout"] = false; },
             [](json& value) { value["remote_logout"] = "true"; },
             [](json& value) { value["tdlib_request"] = "logout"; },
             [](json& value) { value["password"] = "sentinel-secret"; },
         }) {
        auto value = logout_document();
        mutate(value);
        std::string error;
        INFO(value.dump());
        CHECK_FALSE(parse_logout_plan(value, error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("account removal plan rejects invalid and equivalent identities", "[destructive][plan]") {
    for (const auto& mutate : std::vector<std::function<void(json&)>>{
             [](json& value) { value.erase("config_path"); },
             [](json& value) { value["extra"] = true; },
             [](json& value) { value["operation"] = "logout"; },
             [](json& value) { value["account"] = ""; },
             [](json& value) { value["remote_logout"] = false; },
             [](json& value) { value["keep_session"] = "false"; },
             [](json& value) { value["delete_paths"] = json::array({"/data/accounts/work"}); },
             [](json& value) { value["delete_paths"][0] = "relative"; },
             [](json& value) { value["delete_paths"][0] = "/data/./accounts/work"; },
             [](json& value) { value["delete_paths"][1] = value["delete_paths"][0]; },
             [](json& value) { value["delete_paths"][1] = "/data/accounts/../accounts/work"; },
             [](json& value) {
                 value["delete_paths"][1] = "/data/accounts/work/state";
                 value["state_root"]["path"] = value["delete_paths"][1];
             },
             [](json& value) { value["config_path"] = value["delete_paths"][0]; },
             [](json& value) { value["config_path"] = "/data/accounts"; },
             [](json& value) { value["config_path"] = "/data/accounts/work/config.toml"; },
             [](json& value) { value["config_path"] = false; },
             [](json& value) { value["config_snapshot"] = "missing"; },
             [](json& value) { value["config_snapshot"] = "sha256:bad"; },
             [](json& value) { value["data_root"]["path"] = "/data/accounts/other"; },
             [](json& value) { value["data_root"]["device"] = -1; },
             [](json& value) { value["data_root"]["secret"] = "sentinel-secret"; },
             [](json& value) { value["state_root"] = json::array(); },
             [](json& value) {
                 value["state_root"]["device"] = value["data_root"]["device"];
                 value["state_root"]["inode"] = value["data_root"]["inode"];
             },
             [](json& value) { value["reassign_default"] = "work"; },
             [](json& value) { value["reassign_default"] = "../main"; },
             [](json& value) { value["bot_token"] = "sentinel-secret"; },
         }) {
        auto value = removal_document();
        mutate(value);
        std::string error;
        INFO(value.dump());
        CHECK_FALSE(parse_account_remove_plan(value, error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("account removal roots are component-wise disjoint at every plan boundary",
          "[destructive][plan]") {
    auto nested = removal_input();
    nested.delete_paths[1] = "/data/accounts/work/state";
    nested.state_root = RootIdentity{nested.delete_paths[1], 1, 3, 1000};
    std::string error;
    CHECK_FALSE(make_account_remove_plan(nested, error).has_value());

    auto config_descendant = removal_input();
    config_descendant.config_path = "/data/accounts/work/config.toml";
    CHECK_FALSE(make_account_remove_plan(config_descendant, error).has_value());

    auto config_ancestor = removal_input();
    config_ancestor.config_path = "/state/accounts";
    CHECK_FALSE(make_account_remove_plan(config_ancestor, error).has_value());

    auto component_siblings = removal_input();
    component_siblings.delete_paths[1] = "/data/accounts/work-state";
    component_siblings.state_root = RootIdentity{component_siblings.delete_paths[1], 1, 3, 1000};
    component_siblings.config_path = "/data/accounts/work-config/config.toml";
    auto plan = make_account_remove_plan(std::move(component_siblings), error);
    INFO(error);
    REQUIRE(plan.has_value());
    const auto serialized = serialize(*plan);
    auto round_trip = parse_account_remove_plan(serialized, error);
    INFO(error);
    REQUIRE(round_trip.has_value());
    CHECK(*round_trip == *plan);

    auto collapsed = serialized;
    collapsed["delete_paths"][1] = "/data/accounts/work/state";
    collapsed["state_root"]["path"] = collapsed["delete_paths"][1];
    CHECK_FALSE(parse_account_remove_plan(collapsed, error).has_value());
    CHECK_FALSE(
        validate_challenge_payload(destructive_challenge("account_remove", collapsed), error));
    Challenge frame{7, destructive_challenge("account_remove", collapsed)};
    CHECK_FALSE(parse(serialize(Frame{frame}), error).has_value());
}

TEST_CASE("account removal plan represents absent roots and keep-session policy exactly",
          "[destructive][plan]") {
    auto value = removal_document();
    value["remote_logout"] = false;
    value["keep_session"] = true;
    value["data_root"] = nullptr;
    value["state_root"] = nullptr;
    value["reassign_default"] = nullptr;

    std::string error;
    auto plan = parse_account_remove_plan(value, error);
    INFO(error);
    REQUIRE(plan.has_value());
    CHECK(plan->keep_session());
    CHECK_FALSE(plan->remote_logout());
    CHECK_FALSE(plan->data_root().has_value());
    CHECK_FALSE(plan->state_root().has_value());
    CHECK(serialize(*plan) == value);
}

TEST_CASE("frame confirmation validation delegates to the shared plan parser",
          "[destructive][plan][proto]") {
    std::string error;
    CHECK(validate_challenge_payload(destructive_challenge("logout", logout_document()), error));
    CHECK(validate_challenge_payload(destructive_challenge("account_remove", removal_document()),
                                     error));

    auto duplicate = removal_document();
    duplicate["state_root"]["device"] = duplicate["data_root"]["device"];
    duplicate["state_root"]["inode"] = duplicate["data_root"]["inode"];
    CHECK_FALSE(
        validate_challenge_payload(destructive_challenge("account_remove", duplicate), error));

    auto overlapping = removal_document();
    overlapping["delete_paths"][1] = "/data/accounts/work/state";
    overlapping["state_root"]["path"] = overlapping["delete_paths"][1];
    CHECK_FALSE(
        validate_challenge_payload(destructive_challenge("account_remove", overlapping), error));

    auto mismatched = destructive_challenge("logout", removal_document());
    CHECK_FALSE(validate_challenge_payload(mismatched, error));

    Challenge frame{7, destructive_challenge("account_remove", duplicate)};
    CHECK_FALSE(parse(serialize(Frame{frame}), error).has_value());
}

TEST_CASE("M6 destructive confirmations correlate every action with one exact plan",
          "[destructive][plan][proto][m6]") {
    const auto documents = m6_destructive_documents();
    for (std::size_t index = 0; index < documents.size(); ++index) {
        const auto& [action, target] = documents[index];
        CAPTURE(action);
        std::string error;
        auto parsed = parse_m6_destructive_plan(target, error);
        INFO(error);
        REQUIRE(parsed);
        CHECK(parsed->action() == action);
        CHECK(serialize(*parsed) == target);
        CHECK(validate_challenge_payload(destructive_challenge(action, target), error));

        auto extra = target;
        extra["unexpected"] = true;
        CHECK_FALSE(validate_challenge_payload(destructive_challenge(action, extra), error));
        auto malformed = target;
        if (action == "folder_delete") {
            malformed["folder"]["excluded_chat_ids"] = json::array({-1001});
        } else if (action == "chat_invite_link") {
            malformed["invite_link_sha256"] =
                "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        } else if (action == "chat_ban" || action == "chat_kick") {
            malformed["after"] = action == "chat_ban" ? "left" : "banned";
        } else if (action == "storage_optimize") {
            malformed["chat_limit"] = 99;
        } else {
            malformed["session"]["is_current"] = true;
        }
        CHECK_FALSE(validate_challenge_payload(destructive_challenge(action, malformed), error));
        const auto& different_action = documents[(index + 1) % documents.size()].first;
        CHECK_FALSE(
            validate_challenge_payload(destructive_challenge(different_action, target), error));
    }
}
