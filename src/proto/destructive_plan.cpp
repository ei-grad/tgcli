#include "proto/destructive_plan.hpp"

#include "common/paths.hpp"
#include "common/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <string_view>
#include <utility>

namespace tgcli::proto {

namespace {

using nlohmann::json;

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) {
        return false;
    }
    return std::all_of(fields.begin(), fields.end(), [&value](std::string_view field_name) {
        return value.contains(std::string(field_name));
    });
}

bool valid_unsigned_decimal(std::string_view value) {
    if (value.empty() || (value.size() != 1 && value.front() == '0') ||
        !std::all_of(value.begin(), value.end(),
                     [](char character) { return character >= '0' && character <= '9'; })) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool consume_identity_field(std::string_view& identity, std::string_view prefix, bool final) {
    if (!identity.starts_with(prefix)) {
        return false;
    }
    identity.remove_prefix(prefix.size());
    const auto separator = identity.find(';');
    const auto field = final ? identity : identity.substr(0, separator);
    if ((!final && separator == std::string_view::npos) || !valid_unsigned_decimal(field)) {
        return false;
    }
    identity.remove_prefix(final ? identity.size() : separator + 1);
    return true;
}

bool valid_normalized_absolute_path(std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path candidate{std::string(value)};
    if (!candidate.is_absolute() || candidate == candidate.root_path()) {
        return false;
    }
    return candidate.lexically_normal().generic_string() == value;
}

bool valid_int53(const json& value, bool positive = false) {
    constexpr auto maximum = std::int64_t{9'007'199'254'740'991};
    if (!value.is_number_integer()) {
        return false;
    }
    const auto number = value.get<std::int64_t>();
    return positive ? number > 0 && number <= maximum
                    : number != 0 && number >= -maximum && number <= maximum;
}

bool valid_chat_identity(const json& value) {
    if (!exact_fields(value, {"id", "title", "type", "is_bot", "usernames"}) ||
        !valid_int53(value["id"]) || !value["title"].is_string() ||
        value["title"].get_ref<const std::string&>().size() > 1'048'576 ||
        !common::valid_utf8(value["title"].get_ref<const std::string&>()) ||
        !value["type"].is_string() || !value["is_bot"].is_boolean() ||
        !value["usernames"].is_array() || value["usernames"].size() > 100) {
        return false;
    }
    const auto& type = value["type"].get_ref<const std::string&>();
    if ((type != "private" && type != "basic_group" && type != "supergroup" && type != "channel") ||
        (type != "private" && value["is_bot"].get<bool>())) {
        return false;
    }
    return std::ranges::all_of(value["usernames"], [](const json& item) {
        if (!item.is_string()) {
            return false;
        }
        const auto& username = item.get_ref<const std::string&>();
        return !username.empty() && username.size() <= 32 &&
               std::ranges::all_of(username, [](char character) {
                   return (character >= 'A' && character <= 'Z') ||
                          (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '_';
               });
    });
}

std::optional<std::vector<std::int64_t>> parse_message_ids(const json& value) {
    if (!value.is_array() || value.empty() || value.size() > 100) {
        return std::nullopt;
    }
    std::vector<std::int64_t> result;
    result.reserve(value.size());
    for (const auto& item : value) {
        if (!valid_int53(item, true)) {
            return std::nullopt;
        }
        const auto id = item.get<std::int64_t>();
        if (!result.empty() && id <= result.back()) {
            return std::nullopt;
        }
        result.push_back(id);
    }
    return result;
}

bool equivalent_path(std::string_view lhs, std::string_view rhs) {
    return std::filesystem::path(std::string(lhs)).lexically_normal() ==
           std::filesystem::path(std::string(rhs)).lexically_normal();
}

bool component_prefix(const std::filesystem::path& prefix, const std::filesystem::path& value) {
    auto prefix_component = prefix.begin();
    auto value_component = value.begin();
    while (prefix_component != prefix.end() && value_component != value.end()) {
        if (*prefix_component != *value_component) {
            return false;
        }
        ++prefix_component;
        ++value_component;
    }
    return prefix_component == prefix.end();
}

bool overlapping_paths(std::string_view lhs, std::string_view rhs) {
    const auto left = std::filesystem::path(std::string(lhs)).lexically_normal();
    const auto right = std::filesystem::path(std::string(rhs)).lexically_normal();
    return component_prefix(left, right) || component_prefix(right, left);
}

bool valid_root(const std::optional<RootIdentity>& root, std::string_view expected_path) {
    return !root || (root->path == expected_path && valid_normalized_absolute_path(root->path));
}

bool duplicate_roots(const std::optional<RootIdentity>& lhs,
                     const std::optional<RootIdentity>& rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    return equivalent_path(lhs->path, rhs->path) ||
           (lhs->device == rhs->device && lhs->inode == rhs->inode);
}

std::optional<RootIdentity> parse_root(const json& value, std::string_view expected_path,
                                       std::string& error) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!exact_fields(value, {"path", "device", "inode", "owner"}) || !value["path"].is_string() ||
        !value["device"].is_number_unsigned() || !value["inode"].is_number_unsigned() ||
        !value["owner"].is_number_unsigned()) {
        error = "root identity must contain exactly path and unsigned device, inode, owner";
        return std::nullopt;
    }
    RootIdentity root{value["path"].get<std::string>(), value["device"].get<std::uint64_t>(),
                      value["inode"].get<std::uint64_t>(), value["owner"].get<std::uint64_t>()};
    if (root.path != expected_path || !valid_normalized_absolute_path(root.path)) {
        error = "root identity path must exactly match its normalized delete path";
        return std::nullopt;
    }
    return root;
}

json serialize_root(const std::optional<RootIdentity>& root) {
    if (!root) {
        return nullptr;
    }
    return {{"path", root->path},
            {"device", root->device},
            {"inode", root->inode},
            {"owner", root->owner}};
}

json serialize_nullable_string(const std::optional<std::string>& value) {
    return value ? json(value.value_or(std::string{})) : json(nullptr);
}

} // namespace

LogoutPlan::LogoutPlan(std::string account) : account_(std::move(account)) {}

const std::string& LogoutPlan::account() const {
    return account_;
}

AccountRemovePlan::AccountRemovePlan(AccountRemovePlanInput input) : input_(std::move(input)) {}

const std::string& AccountRemovePlan::account() const {
    return input_.account;
}

bool AccountRemovePlan::keep_session() const {
    return input_.keep_session;
}

bool AccountRemovePlan::remote_logout() const {
    return !input_.keep_session;
}

const std::array<std::string, 2>& AccountRemovePlan::delete_paths() const {
    return input_.delete_paths;
}

const std::string& AccountRemovePlan::config_path() const {
    return input_.config_path;
}

const std::string& AccountRemovePlan::config_snapshot() const {
    return input_.config_snapshot;
}

const std::optional<RootIdentity>& AccountRemovePlan::data_root() const {
    return input_.data_root;
}

const std::optional<RootIdentity>& AccountRemovePlan::state_root() const {
    return input_.state_root;
}

const std::optional<std::string>& AccountRemovePlan::reassign_default() const {
    return input_.reassign_default;
}

MsgDeletePlan::MsgDeletePlan(std::string account, json chat, std::vector<std::int64_t> message_ids,
                             bool requested_for_all, bool effective_for_all)
    : account_(std::move(account)), chat_(std::move(chat)), message_ids_(std::move(message_ids)),
      requested_for_all_(requested_for_all), effective_for_all_(effective_for_all) {}

const std::string& MsgDeletePlan::account() const {
    return account_;
}

const json& MsgDeletePlan::chat() const {
    return chat_;
}

const std::vector<std::int64_t>& MsgDeletePlan::message_ids() const {
    return message_ids_;
}

bool MsgDeletePlan::requested_for_all() const {
    return requested_for_all_;
}

bool MsgDeletePlan::effective_for_all() const {
    return effective_for_all_;
}

ChatLeavePlan::ChatLeavePlan(std::string account, json chat)
    : account_(std::move(account)), chat_(std::move(chat)) {}

const std::string& ChatLeavePlan::account() const {
    return account_;
}

const json& ChatLeavePlan::chat() const {
    return chat_;
}

std::optional<LogoutPlan> make_logout_plan(std::string account, std::string& error) {
    if (!paths::valid_account_name(account)) {
        error = "logout plan account is invalid";
        return std::nullopt;
    }
    error.clear();
    return LogoutPlan(std::move(account));
}

std::optional<AccountRemovePlan> make_account_remove_plan(AccountRemovePlanInput input,
                                                          std::string& error) {
    if (!paths::valid_account_name(input.account)) {
        error = "account removal plan account is invalid";
        return std::nullopt;
    }
    if (!valid_normalized_absolute_path(input.delete_paths[0]) ||
        !valid_normalized_absolute_path(input.delete_paths[1]) ||
        overlapping_paths(input.delete_paths[0], input.delete_paths[1])) {
        error = "account removal delete paths must be disjoint normalized absolute paths";
        return std::nullopt;
    }
    if (!valid_normalized_absolute_path(input.config_path) ||
        overlapping_paths(input.config_path, input.delete_paths[0]) ||
        overlapping_paths(input.config_path, input.delete_paths[1])) {
        error = "account removal config path must not overlap either delete path";
        return std::nullopt;
    }
    if (!valid_config_snapshot_identity(input.config_snapshot, false)) {
        error = "account removal config snapshot identity is invalid";
        return std::nullopt;
    }
    if (!valid_root(input.data_root, input.delete_paths[0]) ||
        !valid_root(input.state_root, input.delete_paths[1])) {
        error = "account removal root identity does not match its delete path";
        return std::nullopt;
    }
    if (duplicate_roots(input.data_root, input.state_root)) {
        error = "account removal roots identify the same path or filesystem object";
        return std::nullopt;
    }
    if (input.reassign_default && (!paths::valid_account_name(*input.reassign_default) ||
                                   *input.reassign_default == input.account)) {
        error = "account removal default reassignment is invalid";
        return std::nullopt;
    }
    error.clear();
    return AccountRemovePlan(std::move(input));
}

std::optional<LogoutPlan> parse_logout_plan(const json& value, std::string& error) {
    if (!exact_fields(value, {"operation", "account", "remote_logout", "tdlib_request"}) ||
        !value["operation"].is_string() || value["operation"] != "logout" ||
        !value["account"].is_string() || !value["remote_logout"].is_boolean() ||
        !value["remote_logout"].get<bool>() || !value["tdlib_request"].is_string() ||
        value["tdlib_request"] != "logOut") {
        error = "logout plan must have the exact contract shape and constants";
        return std::nullopt;
    }
    return make_logout_plan(value["account"].get<std::string>(), error);
}

std::optional<AccountRemovePlan> parse_account_remove_plan(const json& value, std::string& error) {
    if (!exact_fields(value, {"operation", "account", "remote_logout", "keep_session",
                              "delete_paths", "config_path", "config_snapshot", "data_root",
                              "state_root", "reassign_default"}) ||
        !value["operation"].is_string() || value["operation"] != "account_remove" ||
        !value["account"].is_string() || !value["remote_logout"].is_boolean() ||
        !value["keep_session"].is_boolean() ||
        value["remote_logout"].get<bool>() == value["keep_session"].get<bool>() ||
        !value["delete_paths"].is_array() || value["delete_paths"].size() != 2 ||
        !value["delete_paths"][0].is_string() || !value["delete_paths"][1].is_string() ||
        !value["config_path"].is_string() || !value["config_snapshot"].is_string() ||
        (!value["reassign_default"].is_null() && !value["reassign_default"].is_string())) {
        error = "account removal plan must have the exact contract shape and types";
        return std::nullopt;
    }

    AccountRemovePlanInput input;
    input.account = value["account"].get<std::string>();
    input.keep_session = value["keep_session"].get<bool>();
    input.delete_paths = {value["delete_paths"][0].get<std::string>(),
                          value["delete_paths"][1].get<std::string>()};
    input.config_path = value["config_path"].get<std::string>();
    input.config_snapshot = value["config_snapshot"].get<std::string>();
    if (!value["reassign_default"].is_null()) {
        input.reassign_default = value["reassign_default"].get<std::string>();
    }

    std::string root_error;
    if (!value["data_root"].is_null()) {
        input.data_root = parse_root(value["data_root"], input.delete_paths[0], root_error);
        if (!input.data_root) {
            error = std::move(root_error);
            return std::nullopt;
        }
    }
    if (!value["state_root"].is_null()) {
        input.state_root = parse_root(value["state_root"], input.delete_paths[1], root_error);
        if (!input.state_root) {
            error = std::move(root_error);
            return std::nullopt;
        }
    }
    return make_account_remove_plan(std::move(input), error);
}

std::optional<MsgDeletePlan> parse_msg_delete_plan(const json& value, std::string& error) {
    if (!exact_fields(value, {"operation", "account", "tdlib_request", "chat", "message_ids",
                              "requested_for_all", "effective_for_all"}) ||
        value["operation"] != "msg_delete" || !value["account"].is_string() ||
        !paths::valid_account_name(value["account"].get_ref<const std::string&>()) ||
        value["tdlib_request"] != "deleteMessages" || !valid_chat_identity(value["chat"]) ||
        !value["requested_for_all"].is_boolean() || !value["effective_for_all"].is_boolean()) {
        error = "message deletion plan must have the exact contract shape and types";
        return std::nullopt;
    }
    auto ids = parse_message_ids(value["message_ids"]);
    if (!ids) {
        error = "message deletion plan ids must be unique ascending positive int53 values";
        return std::nullopt;
    }
    const auto& type = value["chat"]["type"].get_ref<const std::string&>();
    const bool requested = value["requested_for_all"].get<bool>();
    const bool effective = value["effective_for_all"].get<bool>();
    if ((type == "supergroup" || type == "channel") ? !effective : effective != requested) {
        error = "message deletion plan revoke policy contradicts its chat identity";
        return std::nullopt;
    }
    error.clear();
    return MsgDeletePlan(value["account"].get<std::string>(), value["chat"], std::move(*ids),
                         requested, effective);
}

std::optional<ChatLeavePlan> parse_chat_leave_plan(const json& value, std::string& error) {
    if (!exact_fields(value, {"operation", "account", "tdlib_request", "chat"}) ||
        value["operation"] != "chat_leave" || !value["account"].is_string() ||
        !paths::valid_account_name(value["account"].get_ref<const std::string&>()) ||
        value["tdlib_request"] != "leaveChat" || !valid_chat_identity(value["chat"]) ||
        value["chat"]["type"] == "private") {
        error = "chat leave plan must have the exact contract shape and group target";
        return std::nullopt;
    }
    error.clear();
    return ChatLeavePlan(value["account"].get<std::string>(), value["chat"]);
}

std::optional<DestructivePlan> parse_destructive_plan(const json& value, std::string& error) {
    if (!value.is_object()) {
        error = "destructive plan must be an object";
        return std::nullopt;
    }
    const auto operation = value.find("operation");
    if (operation == value.end() || !operation->is_string()) {
        error = "destructive plan operation must be a string";
        return std::nullopt;
    }
    if (*operation == "logout") {
        auto plan = parse_logout_plan(value, error);
        if (plan) {
            return DestructivePlan{std::move(*plan)};
        }
        return std::nullopt;
    }
    if (*operation == "account_remove") {
        auto plan = parse_account_remove_plan(value, error);
        if (plan) {
            return DestructivePlan{std::move(*plan)};
        }
        return std::nullopt;
    }
    if (*operation == "msg_delete") {
        auto plan = parse_msg_delete_plan(value, error);
        if (plan) {
            return DestructivePlan{std::move(*plan)};
        }
        return std::nullopt;
    }
    if (*operation == "chat_leave") {
        auto plan = parse_chat_leave_plan(value, error);
        if (plan) {
            return DestructivePlan{std::move(*plan)};
        }
        return std::nullopt;
    }
    error = "destructive plan operation is unknown";
    return std::nullopt;
}

json serialize(const LogoutPlan& plan) {
    return {{"operation", "logout"},
            {"account", plan.account()},
            {"remote_logout", true},
            {"tdlib_request", "logOut"}};
}

json serialize(const AccountRemovePlan& plan) {
    return {{"operation", "account_remove"},
            {"account", plan.account()},
            {"remote_logout", plan.remote_logout()},
            {"keep_session", plan.keep_session()},
            {"delete_paths", plan.delete_paths()},
            {"config_path", plan.config_path()},
            {"config_snapshot", plan.config_snapshot()},
            {"data_root", serialize_root(plan.data_root())},
            {"state_root", serialize_root(plan.state_root())},
            {"reassign_default", serialize_nullable_string(plan.reassign_default())}};
}

json serialize(const MsgDeletePlan& plan) {
    return {{"operation", "msg_delete"},
            {"account", plan.account()},
            {"tdlib_request", "deleteMessages"},
            {"chat", plan.chat()},
            {"message_ids", plan.message_ids()},
            {"requested_for_all", plan.requested_for_all()},
            {"effective_for_all", plan.effective_for_all()}};
}

json serialize(const ChatLeavePlan& plan) {
    return {{"operation", "chat_leave"},
            {"account", plan.account()},
            {"tdlib_request", "leaveChat"},
            {"chat", plan.chat()}};
}

json serialize(const DestructivePlan& plan) {
    return std::visit([](const auto& value) { return serialize(value); }, plan);
}

bool valid_config_snapshot_identity(std::string_view identity, bool allow_missing) {
    if (identity == "missing") {
        return allow_missing;
    }
    constexpr std::string_view prefix = "sha256:";
    if (!identity.starts_with(prefix)) {
        return false;
    }
    identity.remove_prefix(prefix.size());
    const auto hash_end = identity.find(';');
    if (hash_end != 64 ||
        !std::all_of(identity.begin(), identity.begin() + static_cast<std::ptrdiff_t>(hash_end),
                     [](char character) {
                         return (character >= '0' && character <= '9') ||
                                (character >= 'a' && character <= 'f');
                     })) {
        return false;
    }
    identity.remove_prefix(hash_end + 1);
    return consume_identity_field(identity, "dev:", false) &&
           consume_identity_field(identity, "ino:", false) &&
           consume_identity_field(identity, "size:", false) &&
           consume_identity_field(identity, "ctime_ns:", true) && identity.empty();
}

} // namespace tgcli::proto
