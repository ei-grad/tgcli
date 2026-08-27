#include "proto/destructive_plan.hpp"

#include "common/paths.hpp"
#include "common/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <limits>
#include <set>
#include <span>
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

template <typename Field, std::size_t Size>
bool exact_fields(const json& value, const std::array<Field, Size>& fields) {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields, [&](const auto& field) { return value.contains(field); });
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

bool valid_positive_int32(const json& value) {
    return value.is_number_integer() && value.get<std::int64_t>() > 0 &&
           value.get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max();
}

bool valid_string(const json& value, bool allow_empty = true) {
    return value.is_string() && (allow_empty || !value.get_ref<const std::string&>().empty()) &&
           value.get_ref<const std::string&>().find('\0') == std::string_view::npos &&
           common::valid_utf8(value.get_ref<const std::string&>());
}

bool valid_canonical_int64(const json& value, bool positive = false) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    std::int64_t parsed = 0;
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return status == std::errc{} && end == text.data() + text.size() &&
           std::to_string(parsed) == text && (!positive || parsed > 0);
}

bool valid_sha256(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 71 && text.starts_with("sha256:") &&
           std::ranges::all_of(text.substr(7), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool valid_string_array(const json& value, std::size_t maximum) {
    if (!value.is_array() || value.size() > maximum) {
        return false;
    }
    std::set<std::string> seen;
    return std::ranges::all_of(value, [&](const json& item) {
        return valid_string(item, false) && seen.insert(item.get<std::string>()).second;
    });
}

bool valid_user_identity(const json& value) {
    return exact_fields(value, {"id", "display_name", "usernames", "is_bot"}) &&
           valid_int53(value["id"], true) && valid_string(value["display_name"]) &&
           valid_string_array(value["usernames"], 64) && value["is_bot"].is_boolean();
}

bool valid_folder_name(const json& value) {
    if (!exact_fields(value, {"text", "animate_custom_emoji", "custom_emoji_entities"}) ||
        !valid_string(value["text"], false) || value["text"].get<std::string>().size() > 48 ||
        !value["animate_custom_emoji"].is_boolean() || !value["custom_emoji_entities"].is_array()) {
        return false;
    }
    std::int64_t prior_end = 0;
    for (const auto& entity : value["custom_emoji_entities"]) {
        if (!exact_fields(entity, {"offset", "length", "custom_emoji_id"}) ||
            !entity["offset"].is_number_integer() || !entity["length"].is_number_integer() ||
            !valid_canonical_int64(entity["custom_emoji_id"], true)) {
            return false;
        }
        const auto offset = entity["offset"].get<std::int64_t>();
        const auto length = entity["length"].get<std::int64_t>();
        if (offset < prior_end || length <= 0 ||
            offset > std::numeric_limits<std::int32_t>::max() - length) {
            return false;
        }
        prior_end = offset + length;
    }
    return true;
}

bool valid_folder_icon(const json& value) {
    constexpr std::array<std::string_view, 30> icons{
        "all",   "unread", "unmuted", "bots",     "channels", "groups", "private", "custom",
        "setup", "cat",    "crown",   "favorite", "flower",   "game",   "home",    "love",
        "mask",  "party",  "sport",   "study",    "trade",    "travel", "work",    "airplane",
        "book",  "light",  "like",    "money",    "note",     "palette"};
    return value.is_null() ||
           (value.is_string() &&
            std::ranges::find(icons, value.get_ref<const std::string&>()) != icons.end());
}

bool valid_folder_snapshot(const json& value) {
    constexpr std::array fields{"id",
                                "name",
                                "icon",
                                "color_id",
                                "is_shareable",
                                "has_my_invite_links",
                                "pinned_chat_ids",
                                "included_chat_ids",
                                "excluded_chat_ids",
                                "exclude_muted",
                                "exclude_read",
                                "exclude_archived",
                                "include_contacts",
                                "include_non_contacts",
                                "include_bots",
                                "include_groups",
                                "include_channels"};
    if (!exact_fields(value, fields) || !valid_positive_int32(value["id"]) ||
        !valid_folder_name(value["name"]) || !valid_folder_icon(value["icon"]) ||
        !value["color_id"].is_number_integer() || value["color_id"].get<std::int64_t>() < -1 ||
        value["color_id"].get<std::int64_t>() > 6 || !value["is_shareable"].is_boolean() ||
        !value["has_my_invite_links"].is_boolean()) {
        return false;
    }
    std::set<std::int64_t> ids;
    const auto valid_ids = [&](std::string_view field) {
        const auto& items = value.at(field);
        return items.is_array() && items.size() <= 100 &&
               std::ranges::all_of(items, [&](const json& item) {
                   return valid_int53(item) && ids.insert(item.get<std::int64_t>()).second;
               });
    };
    if (!valid_ids("pinned_chat_ids") || !valid_ids("included_chat_ids") ||
        value["pinned_chat_ids"].size() + value["included_chat_ids"].size() > 100 ||
        !valid_ids("excluded_chat_ids")) {
        return false;
    }
    constexpr std::array booleans{"exclude_muted",    "exclude_read",         "exclude_archived",
                                  "include_contacts", "include_non_contacts", "include_bots",
                                  "include_groups",   "include_channels"};
    return std::ranges::all_of(booleans,
                               [&](std::string_view field) { return value[field].is_boolean(); });
}

bool valid_closed_strings(const json& value, std::span<const std::string_view> names) {
    if (!value.is_array() || value.size() > names.size()) {
        return false;
    }
    std::size_t prior = 0;
    bool first = true;
    for (const auto& item : value) {
        if (!item.is_string()) {
            return false;
        }
        const auto found = std::ranges::find(names, item.get_ref<const std::string&>());
        if (found == names.end()) {
            return false;
        }
        const auto index = static_cast<std::size_t>(std::distance(names.begin(), found));
        if (!first && index <= prior) {
            return false;
        }
        first = false;
        prior = index;
    }
    return true;
}

bool valid_member_status(const json& value) {
    if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string()) {
        return false;
    }
    const auto& kind = value["kind"].get_ref<const std::string&>();
    constexpr std::array<std::string_view, 16> rights{
        "change-info",     "post-messages",          "edit-messages", "delete-messages",
        "invite-users",    "restrict-members",       "pin-messages",  "manage-topics",
        "promote-members", "manage-video-chats",     "post-stories",  "edit-stories",
        "delete-stories",  "manage-direct-messages", "manage-tags",   "anonymous"};
    constexpr std::array<std::string_view, 16> permissions{
        "send-basic-messages", "send-audios",       "send-documents",    "send-photos",
        "send-videos",         "send-video-notes",  "send-voice-notes",  "send-polls",
        "send-other-messages", "add-link-previews", "react-to-messages", "edit-tag",
        "change-info",         "invite-users",      "pin-messages",      "create-topics"};
    if (kind == "creator") {
        return exact_fields(value, {"kind", "is_anonymous", "is_member"}) &&
               value["is_anonymous"].is_boolean() && value["is_member"].is_boolean();
    }
    if (kind == "administrator") {
        return exact_fields(value, {"kind", "can_be_edited", "can_manage_chat", "rights"}) &&
               value["can_be_edited"].is_boolean() && value["can_manage_chat"].is_boolean() &&
               valid_closed_strings(value["rights"], rights);
    }
    if (kind == "member") {
        return exact_fields(value, {"kind", "member_until_date"}) &&
               value["member_until_date"].is_number_integer() &&
               value["member_until_date"].get<std::int64_t>() >= 0 &&
               value["member_until_date"].get<std::int64_t>() <=
                   std::numeric_limits<std::int32_t>::max();
    }
    if (kind == "restricted") {
        return exact_fields(value, {"kind", "is_member", "restricted_until_date", "permissions"}) &&
               value["is_member"].is_boolean() &&
               value["restricted_until_date"].is_number_integer() &&
               value["restricted_until_date"].get<std::int64_t>() >= 0 &&
               value["restricted_until_date"].get<std::int64_t>() <=
                   std::numeric_limits<std::int32_t>::max() &&
               valid_closed_strings(value["permissions"], permissions);
    }
    if (kind == "left") {
        return exact_fields(value, {"kind"});
    }
    return kind == "banned" && exact_fields(value, {"kind", "banned_until_date"}) &&
           value["banned_until_date"].is_number_integer() &&
           value["banned_until_date"].get<std::int64_t>() >= 0 &&
           value["banned_until_date"].get<std::int64_t>() <=
               std::numeric_limits<std::int32_t>::max();
}

bool valid_session_target(const json& value) {
    constexpr std::array<std::string_view, 17> device_types{
        "android", "apple", "brave",  "chrome", "edge",    "firefox", "ipad",    "iphone", "linux",
        "mac",     "opera", "safari", "ubuntu", "unknown", "vivaldi", "windows", "xbox"};
    return exact_fields(value,
                        {"id", "is_current", "is_password_pending", "is_unconfirmed", "device_type",
                         "application_name", "application_version", "device_model", "platform",
                         "system_version", "last_active_date"}) &&
           valid_canonical_int64(value["id"]) && value["is_current"] == false &&
           value["is_password_pending"].is_boolean() && value["is_unconfirmed"].is_boolean() &&
           value["device_type"].is_string() &&
           std::ranges::find(device_types, value["device_type"].get_ref<const std::string&>()) !=
               device_types.end() &&
           valid_string(value["application_name"]) && valid_string(value["application_version"]) &&
           valid_string(value["device_model"]) && valid_string(value["platform"]) &&
           valid_string(value["system_version"]) &&
           (value["last_active_date"].is_null() || valid_string(value["last_active_date"], false));
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
    if ((type == "supergroup" || type == "channel") ? (!requested || !effective)
                                                    : effective != requested) {
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

M6DestructivePlan::M6DestructivePlan(std::string action, json value)
    : action_(std::move(action)), value_(std::move(value)) {}

const std::string& M6DestructivePlan::action() const {
    return action_;
}

const std::string& M6DestructivePlan::account() const {
    return value_["account"].get_ref<const std::string&>();
}

const json& M6DestructivePlan::value() const {
    return value_;
}

std::optional<M6DestructivePlan> parse_m6_destructive_plan(const json& value, std::string& error) {
    if (!value.is_object() || !value.contains("operation") || !value["operation"].is_string() ||
        !value.contains("account") || !value["account"].is_string() ||
        !paths::valid_account_name(value["account"].get_ref<const std::string&>()) ||
        !value.contains("tdlib_request") || !value["tdlib_request"].is_string()) {
        error = "M6 destructive plan must have the common exact contract fields";
        return std::nullopt;
    }
    const auto& operation = value["operation"].get_ref<const std::string&>();
    const auto exact = [&](std::initializer_list<std::string_view> fields,
                           std::string_view tdlib_request) {
        return exact_fields(value, fields) && value["tdlib_request"] == tdlib_request;
    };
    const bool valid =
        (operation == "folder_delete" &&
         exact({"operation", "account", "tdlib_request", "folder", "leave_chat_ids"},
               "deleteChatFolder") &&
         valid_folder_snapshot(value["folder"]) && value["leave_chat_ids"].is_array() &&
         value["leave_chat_ids"].empty()) ||
        (operation == "chat_invite_link" &&
         exact({"operation", "account", "tdlib_request", "chat", "action", "invite_link_sha256"},
               value.value("action", std::string{}) == "create" ? "createChatInviteLink"
                                                                : "revokeChatInviteLink") &&
         valid_chat_identity(value["chat"]) && value["action"].is_string() &&
         ((value["action"] == "create" && value["invite_link_sha256"].is_null()) ||
          (value["action"] == "revoke" && valid_sha256(value["invite_link_sha256"])))) ||
        ((operation == "chat_ban" || operation == "chat_kick") &&
         exact({"operation", "account", "tdlib_request", "chat", "user", "before", "after"},
               "setChatMemberStatus") &&
         valid_chat_identity(value["chat"]) && valid_user_identity(value["user"]) &&
         valid_member_status(value["before"]) &&
         value["after"] == (operation == "chat_ban" ? "banned" : "left")) ||
        (operation == "storage_optimize" &&
         exact({"operation", "account", "tdlib_request", "size", "ttl", "count", "immunity_delay",
                "file_types", "chat_ids", "exclude_chat_ids", "return_deleted_file_statistics",
                "chat_limit"},
               "optimizeStorage") &&
         value["size"] == -1 && value["ttl"] == -1 && value["count"] == -1 &&
         value["immunity_delay"] == -1 && value["file_types"].is_array() &&
         value["file_types"].empty() && value["chat_ids"].is_array() && value["chat_ids"].empty() &&
         value["exclude_chat_ids"].is_array() && value["exclude_chat_ids"].empty() &&
         value["return_deleted_file_statistics"] == false && value["chat_limit"] == 100) ||
        (operation == "session_terminate" &&
         exact({"operation", "account", "tdlib_request", "session"}, "terminateSession") &&
         valid_session_target(value["session"]));
    if (!valid) {
        error = "M6 destructive plan must match its exact action contract";
        return std::nullopt;
    }
    error.clear();
    return M6DestructivePlan(operation, value);
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
    if (*operation == "folder_delete" || *operation == "chat_invite_link" ||
        *operation == "chat_ban" || *operation == "chat_kick" || *operation == "storage_optimize" ||
        *operation == "session_terminate") {
        auto plan = parse_m6_destructive_plan(value, error);
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

json serialize(const M6DestructivePlan& plan) {
    return plan.value();
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
