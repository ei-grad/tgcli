#include "daemon/account_audit.hpp"
#include "daemon/m6_audit_contract.hpp"
#include "daemon/m6_write_policy.hpp"
#include "daemon/write_contract.hpp"
#include "schema_matcher.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using nlohmann::json;
namespace daemon = tgcli::daemon;
namespace proto = tgcli::proto;
using O = proto::M6Operation;

json user() {
    return {{"id", 42}, {"display_name", "Ada"}, {"usernames", {"ada"}}, {"is_bot", false}};
}

json chat() {
    return {{"id", -1001},
            {"title", "Project"},
            {"type", "supergroup"},
            {"is_bot", false},
            {"usernames", {"project"}}};
}

json folder_name(std::string text = "Work") {
    return {{"text", std::move(text)},
            {"animate_custom_emoji", false},
            {"custom_emoji_entities", json::array()}};
}

json folder(std::string name = "Work") {
    return {{"id", 7},
            {"name", folder_name(std::move(name))},
            {"icon", "work"},
            {"color_id", 3},
            {"is_shareable", false},
            {"has_my_invite_links", false},
            {"pinned_chat_ids", json::array()},
            {"included_chat_ids", {-1001}},
            {"excluded_chat_ids", json::array()},
            {"exclude_muted", false},
            {"exclude_read", false},
            {"exclude_archived", false},
            {"include_contacts", false},
            {"include_non_contacts", false},
            {"include_bots", false},
            {"include_groups", false},
            {"include_channels", false}};
}

json topic(bool closed = false) {
    return {{"chat_id", -1001},
            {"id", 9},
            {"name", "Topic"},
            {"icon", {{"color", "blue"}, {"custom_emoji_id", "0"}}},
            {"creation_date", "1970-01-01T00:00:01Z"},
            {"creator", {{"type", "user"}, {"id", 42}}},
            {"is_general", false},
            {"is_outgoing", true},
            {"is_closed", closed},
            {"is_hidden", false},
            {"is_name_implicit", false}};
}

json plan(O operation) {
    const auto* policy = daemon::m6_write_policy(operation);
    REQUIRE(policy != nullptr);
    json value{{"operation", policy->audit_name},
               {"account", "main"},
               {"tdlib_request", policy->tdlib_functions[0]}};
    switch (operation) {
    case O::ContactAdd:
        value.update({{"user", user()},
                      {"first_name", "Ada"},
                      {"last_name", ""},
                      {"phone_number_sha256",
                       "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
                      {"share_phone_number", false}});
        break;
    case O::ContactRemove:
        value.update({{"user", user()}, {"is_contact", false}});
        break;
    case O::ContactBlock:
    case O::ContactUnblock:
        value.update({{"user", user()}, {"blocked", operation == O::ContactBlock}});
        break;
    case O::FolderCreate:
        value.update(
            {{"name", folder_name()}, {"icon", "work"}, {"color_id", 3}, {"chat_ids", {-1001}}});
        break;
    case O::FolderEdit:
        value.update({{"folder_id", 7}, {"before", folder()}, {"after", folder("Other")}});
        break;
    case O::FolderDelete:
        value.update({{"folder", folder()}, {"leave_chat_ids", json::array()}});
        break;
    case O::FolderAddChat:
    case O::FolderRemoveChat: {
        auto before = folder();
        auto after = folder();
        after["included_chat_ids"] =
            operation == O::FolderAddChat ? json::array({-1001, -1002}) : json::array();
        value.update({{"folder_id", 7}, {"chat", chat()}, {"before", before}, {"after", after}});
        break;
    }
    case O::TopicCreate:
        value.update({{"chat", chat()}, {"name", "Topic"}, {"icon", "blue"}});
        break;
    case O::TopicEdit:
        value.update({{"chat", chat()}, {"before", topic()}, {"name", "Other"}});
        break;
    case O::TopicClose:
    case O::TopicReopen:
        value.update({{"chat", chat()},
                      {"before", topic(operation == O::TopicReopen)},
                      {"closed", operation == O::TopicClose}});
        break;
    case O::ChatSetTitle:
        value.update({{"chat", chat()}, {"title", "Title"}});
        break;
    case O::ChatSetPhoto:
        value.update({{"chat", chat()}, {"delete", true}, {"file", nullptr}});
        break;
    case O::ChatSetDescription:
        value.update({{"chat", chat()}, {"description", "Description"}});
        break;
    case O::ChatInviteLink:
        value.update({{"chat", chat()}, {"action", "create"}, {"invite_link_sha256", nullptr}});
        break;
    case O::ChatPromote:
        value.update({{"chat", chat()},
                      {"user", user()},
                      {"before", {{"kind", "member"}, {"member_until_date", 0}}},
                      {"can_manage_chat", true},
                      {"rights", {"change-info"}}});
        break;
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick: {
        std::string_view after = "left";
        if (operation == O::ChatDemote) {
            after = "member";
        } else if (operation == O::ChatBan) {
            after = "banned";
        }
        value.update({{"chat", chat()},
                      {"user", user()},
                      {"before", {{"kind", "member"}, {"member_until_date", 0}}},
                      {"after", after}});
        break;
    }
    case O::ChatSetPermissions:
        value.update({{"chat", chat()}, {"permissions", {"send-basic-messages"}}});
        break;
    case O::StorageOptimize:
        value.update({{"size", -1},
                      {"ttl", -1},
                      {"count", -1},
                      {"immunity_delay", -1},
                      {"file_types", json::array()},
                      {"chat_ids", json::array()},
                      {"exclude_chat_ids", json::array()},
                      {"return_deleted_file_statistics", false},
                      {"chat_limit", 100}});
        break;
    case O::ContactList:
    case O::ContactSearch:
    case O::FolderList:
    case O::FolderShow:
    case O::TopicList:
    case O::StorageStats:
        FAIL("read operation has no durable plan");
    }
    return value;
}

json result(O operation, const json& planned) {
    switch (operation) {
    case O::ContactAdd:
    case O::ContactRemove:
        return {{"user", planned["user"]}, {"is_contact", operation == O::ContactAdd}};
    case O::ContactBlock:
    case O::ContactUnblock:
        return {{"user", planned["user"]}, {"blocked", operation == O::ContactBlock}};
    case O::FolderCreate: {
        auto created = folder();
        created["name"] = planned["name"];
        created["icon"] = planned["icon"];
        created["color_id"] = planned["color_id"];
        created["included_chat_ids"] = planned["chat_ids"];
        return {{"folder", created}};
    }
    case O::FolderEdit:
        return {{"folder", planned["after"]}};
    case O::FolderDelete:
        return {{"folder_id", planned["folder"]["id"]}, {"deleted", true}};
    case O::FolderAddChat:
    case O::FolderRemoveChat:
        return {{"folder", planned["after"]},
                {"chat", planned["chat"]},
                {"included", operation == O::FolderAddChat}};
    case O::TopicCreate:
        return {{"topic", topic()}};
    case O::TopicEdit:
        return {{"chat", planned["chat"]},
                {"topic_id", planned["before"]["id"]},
                {"name", planned["name"]}};
    case O::TopicClose:
    case O::TopicReopen:
        return {{"chat", planned["chat"]},
                {"topic_id", planned["before"]["id"]},
                {"closed", planned["closed"]}};
    case O::ChatSetTitle:
        return {{"chat", planned["chat"]}, {"title", planned["title"]}};
    case O::ChatSetPhoto:
        return {{"chat", planned["chat"]}, {"photo", "deleted"}};
    case O::ChatSetDescription:
        return {{"chat", planned["chat"]}, {"description", planned["description"]}};
    case O::ChatInviteLink:
        return {{"chat", planned["chat"]},
                {"action", planned["action"]},
                {"invite_link", "https://t.me/+abcdef"}};
    case O::ChatPromote:
        return {{"chat", planned["chat"]},
                {"user", planned["user"]},
                {"status", "administrator"},
                {"can_manage_chat", true},
                {"rights", planned["rights"]}};
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick:
        return {{"chat", planned["chat"]}, {"user", planned["user"]}, {"status", planned["after"]}};
    case O::ChatSetPermissions:
        return {{"chat", planned["chat"]}, {"permissions", planned["permissions"]}};
    case O::StorageOptimize:
        return {{"optimized", true},
                {"statistics", {{"size", 0}, {"count", 0}, {"by_chat", json::array()}}}};
    case O::ContactList:
    case O::ContactSearch:
    case O::FolderList:
    case O::FolderShow:
    case O::TopicList:
    case O::StorageStats:
        FAIL("read operation has no durable result");
    }
    return {};
}

TEST_CASE("M6 audit plans form one strict branch for every mutation", "[m6][audit-contract]") {
    for (const auto& policy : daemon::m6_write_policies()) {
        CAPTURE(policy.audit_name);
        auto value = plan(policy.operation);
        CHECK(daemon::valid_m6_audit_plan(policy.operation, value, "main"));
        value["unexpected"] = true;
        CHECK_FALSE(daemon::valid_m6_audit_plan(policy.operation, value, "main"));
    }
}

TEST_CASE("M6 audit plan binds exact operation account and TD request", "[m6][audit-contract]") {
    auto value = plan(O::ChatInviteLink);
    CHECK(daemon::valid_m6_audit_plan(O::ChatInviteLink, value, "main"));
    value["tdlib_request"] = "revokeChatInviteLink";
    CHECK_FALSE(daemon::valid_m6_audit_plan(O::ChatInviteLink, value, "main"));
    value = plan(O::StorageOptimize);
    CHECK_FALSE(daemon::valid_m6_audit_plan(O::StorageOptimize, value, "other"));
}

TEST_CASE("M6 WriteOperation binds strict plans and redacted arguments into shared contracts",
          "[m6][audit-contract][write-operation]") {
    for (const auto& policy : daemon::m6_write_policies()) {
        CAPTURE(policy.audit_name);
        const daemon::WriteOperation operation(policy.operation);
        REQUIRE(operation);
        REQUIRE(operation.audit());
        CHECK(operation.name() == policy.audit_name);
        CHECK(operation.idempotent() == policy.idempotent);
        CHECK(operation.uses_photo_spool() == policy.uses_photo_spool);

        auto value = plan(policy.operation);
        std::string error;
        CHECK(daemon::write_contract::make_plan(operation, "main", value, error));
        value.erase("operation");
        value.erase("account");
        value.erase("tdlib_request");
        CHECK(daemon::write_contract::make_arguments(operation, value, error));
    }
}

TEST_CASE("M6 account audit accepts the exact direct stage lifecycle for all 24 mutations",
          "[m6][audit-contract][write-operation]") {
    constexpr std::string_view hash =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    constexpr std::string_view snapshot =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;"
        "dev:1;ino:2;size:3;ctime_ns:4";
    for (const auto& policy : daemon::m6_write_policies()) {
        CAPTURE(policy.audit_name);
        const daemon::WriteOperation operation(policy.operation);
        REQUIRE(operation.audit());
        auto planned = plan(policy.operation);
        auto arguments = planned;
        arguments.erase("operation");
        arguments.erase("account");
        arguments.erase("tdlib_request");
        std::string error;
        auto intent = daemon::make_account_audit_intent(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:00Z"},
             "main",
             *operation.audit(),
             arguments,
             planned,
             std::string(hash),
             std::string(snapshot),
             "request",
             operation.destructive() ? std::optional<std::string>{"yes"} : std::nullopt,
             policy.idempotent ? std::optional<std::string>{std::string(hash)} : std::nullopt,
             100},
            error);
        INFO(error);
        REQUIRE(intent);
        CHECK(
            tgcli::test::matches_json_schema("audit-intent.schema.json").match(intent->document()));

        std::vector<daemon::AccountAuditCheckpointInput> history;
        std::uint32_t sequence = 0;
        const auto require_schema_checkpoint =
            [&error](const daemon::AccountAuditCheckpointInput& input) {
                auto checkpoint = daemon::make_account_audit_checkpoint(input, error);
                INFO(error);
                REQUIRE(checkpoint);
                CHECK(tgcli::test::matches_json_schema("audit-checkpoint.schema.json")
                          .match(checkpoint->document()));
            };
        if (policy.idempotent) {
            history.push_back({{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:01Z"},
                               "main",
                               *operation.audit(),
                               ++sequence,
                               daemon::AccountAuditStage::IdempotencyPending,
                               {{"key_hash", hash},
                                {"request_fingerprint", hash},
                                {"expires_at", std::uint64_t{1}},
                                {"reserved_terminal_bytes",
                                 daemon::account_audit_terminal_reservation(*operation.audit())}}});
            require_schema_checkpoint(history.back());
        }
        history.push_back({{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:02Z"},
                           "main",
                           *operation.audit(),
                           ++sequence,
                           daemon::AccountAuditStage::DispatchStarted,
                           {{"tdlib_function", planned["tdlib_request"]},
                            {"dispatch_token", "0123456789abcdef0123456789abcdef"},
                            {"client_generation", std::uint64_t{1}}}});
        require_schema_checkpoint(history.back());
        const json terminal{{"kind", "result"}, {"data", result(policy.operation, planned)}};
        history.push_back({{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:03Z"},
                           "main",
                           *operation.audit(),
                           ++sequence,
                           daemon::AccountAuditStage::MutationConfirmed,
                           {{"terminal", terminal}}});
        require_schema_checkpoint(history.back());
        INFO(error);
        CHECK(daemon::validate_account_audit_stage_history(*operation.audit(), history, error));
        auto outcome = daemon::make_account_audit_outcome(
            {{"0123456789abcdef0123456789abcdef", "2026-08-19T12:00:04Z"},
             "main",
             *operation.audit(),
             true,
             daemon::AccountAuditMutationState::Confirmed,
             policy.idempotent ? std::vector{daemon::AccountAuditStage::IdempotencyPending,
                                             daemon::AccountAuditStage::DispatchStarted,
                                             daemon::AccountAuditStage::MutationConfirmed}
                               : std::vector{daemon::AccountAuditStage::DispatchStarted,
                                             daemon::AccountAuditStage::MutationConfirmed},
             terminal},
            error);
        INFO(error);
        REQUIRE(outcome);
        CHECK(tgcli::test::matches_json_schema("audit-outcome.schema.json")
                  .match(outcome->document()));
    }
}

} // namespace
