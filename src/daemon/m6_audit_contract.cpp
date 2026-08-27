#include "daemon/m6_audit_contract.hpp"

#include "common/paths.hpp"
#include "common/utf8.hpp"
#include "daemon/m6_domain.hpp"
#include "daemon/m6_write_policy.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;
using O = proto::M6Operation;

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields,
                               [&](std::string_view field) { return value.contains(field); });
}

template <typename String, std::size_t Size>
bool exact_fields(const json& value, const std::array<String, Size>& fields) {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields, [&](const auto& field) { return value.contains(field); });
}

bool valid_string(const json& value, bool empty = true) {
    return value.is_string() && common::valid_utf8(value.get_ref<const std::string&>()) &&
           value.get_ref<const std::string&>().find('\0') == std::string::npos &&
           (empty || !value.get_ref<const std::string&>().empty());
}

bool valid_int53(const json& value, bool positive = false) {
    if (!value.is_number_integer()) {
        return false;
    }
    const auto number = value.get<std::int64_t>();
    return number >= (positive ? 1 : -kMaximumInt53) && number <= kMaximumInt53;
}

bool valid_positive_int32(const json& value) {
    return value.is_number_integer() && value.get<std::int64_t>() >= 1 &&
           value.get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max();
}

bool valid_hash(const json& value) {
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

bool valid_canonical_int64(const json& value, bool positive = false) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.front() == '+' || (text.size() > 1 && text.front() == '0')) ||
        text == "-0") {
        return false;
    }
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return error == std::errc{} && end == text.data() + text.size() && (!positive || parsed > 0);
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

bool valid_chat_identity(const json& value) {
    if (!exact_fields(value, {"id", "title", "type", "is_bot", "usernames"}) ||
        !valid_int53(value["id"]) || value["id"] == 0 || !valid_string(value["title"]) ||
        !value["type"].is_string() || !value["is_bot"].is_boolean() ||
        !valid_string_array(value["usernames"], 64)) {
        return false;
    }
    const auto& type = value["type"].get_ref<const std::string&>();
    return type == "private" || type == "basic_group" || type == "supergroup" || type == "channel";
}

bool valid_closed_array(const json& value, std::span<const std::string_view> names,
                        std::size_t minimum) {
    if (!value.is_array() || value.size() < minimum || value.size() > names.size()) {
        return false;
    }
    std::size_t previous = 0;
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
        if (!first && index <= previous) {
            return false;
        }
        previous = index;
        first = false;
    }
    return true;
}

bool valid_folder_name(const json& value) {
    if (!exact_fields(value, {"text", "animate_custom_emoji", "custom_emoji_entities"}) ||
        !value["text"].is_string() ||
        !valid_m6_canonical_text(M6TextKind::FolderName,
                                 value["text"].get_ref<const std::string&>()) ||
        !value["animate_custom_emoji"].is_boolean() || !value["custom_emoji_entities"].is_array()) {
        return false;
    }
    std::int64_t previous_end = 0;
    for (const auto& entity : value["custom_emoji_entities"]) {
        if (!exact_fields(entity, {"offset", "length", "custom_emoji_id"}) ||
            !entity["offset"].is_number_integer() || !entity["length"].is_number_integer() ||
            !valid_canonical_int64(entity["custom_emoji_id"], true)) {
            return false;
        }
        const auto offset = entity["offset"].get<std::int64_t>();
        const auto length = entity["length"].get<std::int64_t>();
        if (offset < previous_end || length <= 0 ||
            offset > std::numeric_limits<std::int32_t>::max() - length) {
            return false;
        }
        previous_end = offset + length;
    }
    return true;
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
        !valid_folder_name(value["name"]) ||
        !(value["icon"].is_null() ||
          (value["icon"].is_string() &&
           parse_m6_folder_icon(value["icon"].get_ref<const std::string&>()).has_value())) ||
        !value["color_id"].is_number_integer() || value["color_id"].get<std::int64_t>() < -1 ||
        value["color_id"].get<std::int64_t>() > 6 || !value["is_shareable"].is_boolean() ||
        !value["has_my_invite_links"].is_boolean()) {
        return false;
    }
    std::set<std::int64_t> ids;
    const auto valid_ids = [&](std::string_view field, std::size_t maximum) {
        const auto& array = value.at(field);
        if (!array.is_array() || array.size() > maximum) {
            return false;
        }
        return std::ranges::all_of(array, [&](const json& item) {
            return valid_int53(item) && item != 0 && ids.insert(item.get<std::int64_t>()).second;
        });
    };
    if (!valid_ids("pinned_chat_ids", 100) || !valid_ids("included_chat_ids", 100) ||
        value["pinned_chat_ids"].size() + value["included_chat_ids"].size() > 100 ||
        !valid_ids("excluded_chat_ids", 100)) {
        return false;
    }
    for (const auto field :
         {"exclude_muted", "exclude_read", "exclude_archived", "include_contacts",
          "include_non_contacts", "include_bots", "include_groups", "include_channels"}) {
        if (!value[field].is_boolean()) {
            return false;
        }
    }
    return true;
}

bool valid_topic_info(const json& value) {
    return exact_fields(value,
                        {"chat_id", "id", "name", "icon", "creation_date", "creator", "is_general",
                         "is_outgoing", "is_closed", "is_hidden", "is_name_implicit"}) &&
           valid_int53(value["chat_id"]) && value["chat_id"] != 0 &&
           valid_positive_int32(value["id"]) && value["name"].is_string() &&
           valid_m6_canonical_text(M6TextKind::TopicName,
                                   value["name"].get_ref<const std::string&>()) &&
           exact_fields(value["icon"], {"color", "custom_emoji_id"}) &&
           value["icon"]["color"].is_string() &&
           parse_m6_topic_color(value["icon"]["color"].get_ref<const std::string&>()).has_value() &&
           valid_canonical_int64(value["icon"]["custom_emoji_id"]) &&
           valid_string(value["creation_date"], false) &&
           exact_fields(value["creator"], {"type", "id"}) &&
           (value["creator"]["type"] == "user" || value["creator"]["type"] == "chat") &&
           valid_int53(value["creator"]["id"]) && value["creator"]["id"] != 0 &&
           value["is_general"].is_boolean() && value["is_outgoing"].is_boolean() &&
           value["is_closed"].is_boolean() && value["is_hidden"].is_boolean() &&
           value["is_name_implicit"].is_boolean();
}

bool valid_member_status(const json& value) {
    if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string()) {
        return false;
    }
    const auto& kind = value["kind"].get_ref<const std::string&>();
    if (kind == "creator") {
        return exact_fields(value, {"kind", "is_anonymous", "is_member"}) &&
               value["is_anonymous"].is_boolean() && value["is_member"].is_boolean();
    }
    if (kind == "administrator") {
        return exact_fields(value, {"kind", "can_be_edited", "can_manage_chat", "rights"}) &&
               value["can_be_edited"].is_boolean() && value["can_manage_chat"].is_boolean() &&
               valid_closed_array(value["rights"], m6_admin_right_names(), 0);
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
               valid_closed_array(value["permissions"], m6_chat_permission_names(), 0);
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

bool valid_file_snapshot(const json& value) {
    return exact_fields(value, {"path", "name", "size", "sha256", "device", "inode", "mtime_ns",
                                "ctime_ns"}) &&
           valid_string(value["path"], false) && valid_string(value["name"], false) &&
           value["size"].is_number_unsigned() && valid_hash(value["sha256"]) &&
           value["device"].is_number_unsigned() && value["inode"].is_number_unsigned() &&
           value["mtime_ns"].is_number_integer() && value["ctime_ns"].is_number_integer();
}

bool valid_storage_statistics(const json& value) {
    static constexpr std::array<std::string_view, 25> file_types{
        "none",
        "animation",
        "audio",
        "document",
        "live-photo-video",
        "notification-sound",
        "photo",
        "photo-story",
        "profile-photo",
        "secret",
        "secret-thumbnail",
        "secure",
        "self-destructing-live-photo-video",
        "self-destructing-photo",
        "self-destructing-video",
        "self-destructing-video-note",
        "self-destructing-voice-note",
        "sticker",
        "thumbnail",
        "unknown",
        "video",
        "video-note",
        "video-story",
        "voice-note",
        "wallpaper"};
    if (!exact_fields(value, {"size", "count", "by_chat"}) || !valid_int53(value["size"]) ||
        value["size"].get<std::int64_t>() < 0 || !value["count"].is_number_integer() ||
        value["count"].get<std::int64_t>() < 0 ||
        value["count"].get<std::int64_t>() > std::numeric_limits<std::int32_t>::max() ||
        !value["by_chat"].is_array() || value["by_chat"].size() > 101) {
        return false;
    }
    std::int64_t total_size = 0;
    std::int64_t total_count = 0;
    std::set<std::int64_t> chats;
    bool aggregate = false;
    for (const auto& row : value["by_chat"]) {
        if (!exact_fields(row, {"chat_id", "size", "count", "by_file_type"}) ||
            !valid_int53(row["chat_id"]) || !valid_int53(row["size"]) ||
            row["size"].get<std::int64_t>() < 0 || !row["count"].is_number_integer() ||
            row["count"].get<std::int64_t>() < 0 || !row["by_file_type"].is_array() ||
            row["by_file_type"].size() > 25) {
            return false;
        }
        const auto chat_id = row["chat_id"].get<std::int64_t>();
        if ((chat_id == 0 && std::exchange(aggregate, true)) ||
            (chat_id != 0 && !chats.insert(chat_id).second)) {
            return false;
        }
        std::int64_t row_size = 0;
        std::int64_t row_count = 0;
        std::set<std::string> types;
        for (const auto& item : row["by_file_type"]) {
            if (!exact_fields(item, {"file_type", "size", "count"}) ||
                !item["file_type"].is_string() ||
                std::ranges::find(file_types, item["file_type"].get_ref<const std::string&>()) ==
                    file_types.end() ||
                !valid_int53(item["size"]) || item["size"].get<std::int64_t>() < 0 ||
                !item["count"].is_number_integer() || item["count"].get<std::int64_t>() < 0 ||
                !types.insert(item["file_type"].get<std::string>()).second ||
                row_size > kMaximumInt53 - item["size"].get<std::int64_t>() ||
                row_count >
                    std::numeric_limits<std::int32_t>::max() - item["count"].get<std::int64_t>()) {
                return false;
            }
            row_size += item["size"].get<std::int64_t>();
            row_count += item["count"].get<std::int64_t>();
        }
        if (row_size != row["size"] || row_count != row["count"] ||
            total_size > kMaximumInt53 - row_size ||
            total_count > std::numeric_limits<std::int32_t>::max() - row_count) {
            return false;
        }
        total_size += row_size;
        total_count += row_count;
    }
    return total_size == value["size"] && total_count == value["count"];
}

bool common_plan(const M6WritePolicy& policy, const json& value, std::string_view account) {
    return value.is_object() && value.contains("operation") &&
           value["operation"] == policy.audit_name && value.contains("account") &&
           value["account"] == account && paths::valid_account_name(std::string(account)) &&
           value.contains("tdlib_request") && value["tdlib_request"].is_string() &&
           valid_m6_tdlib_function(policy.operation,
                                   value["tdlib_request"].get_ref<const std::string&>());
}

} // namespace

bool valid_m6_audit_arguments(proto::M6Operation operation, const json& value) {
    const auto* policy = m6_write_policy(operation);
    if (policy == nullptr || !value.is_object() || value.contains("operation") ||
        value.contains("account") || value.contains("tdlib_request")) {
        return false;
    }
    auto synthetic = value;
    synthetic["operation"] = policy->audit_name;
    synthetic["account"] = "main";
    synthetic["tdlib_request"] = policy->tdlib_functions[0];
    if (operation == O::ChatInviteLink && value.contains("action") && value["action"] == "revoke") {
        synthetic["tdlib_request"] = policy->tdlib_functions[1];
    }
    return valid_m6_audit_plan(operation, synthetic, "main");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed 24-operation plan union.
bool valid_m6_audit_plan(proto::M6Operation operation, const json& value,
                         std::string_view account) {
    const auto* policy = m6_write_policy(operation);
    if (policy == nullptr || !common_plan(*policy, value, account)) {
        return false;
    }
    switch (operation) {
    case O::ContactAdd:
        return exact_fields(value, {"operation", "account", "tdlib_request", "user", "first_name",
                                    "last_name", "phone_number_sha256", "share_phone_number"}) &&
               valid_user_identity(value["user"]) && valid_string(value["first_name"], false) &&
               valid_string(value["last_name"]) && valid_hash(value["phone_number_sha256"]) &&
               value["share_phone_number"] == false;
    case O::ContactRemove:
        return exact_fields(value,
                            {"operation", "account", "tdlib_request", "user", "is_contact"}) &&
               valid_user_identity(value["user"]) && value["is_contact"] == false;
    case O::ContactBlock:
    case O::ContactUnblock:
        return exact_fields(value, {"operation", "account", "tdlib_request", "user", "blocked"}) &&
               valid_user_identity(value["user"]) &&
               value["blocked"] == (operation == O::ContactBlock);
    case O::FolderCreate:
        if (!exact_fields(value, {"operation", "account", "tdlib_request", "name", "icon",
                                  "color_id", "chat_ids"}) ||
            !valid_folder_name(value["name"]) ||
            !(value["icon"].is_null() ||
              (value["icon"].is_string() &&
               parse_m6_folder_icon(value["icon"].get_ref<const std::string&>()).has_value())) ||
            !value["color_id"].is_number_integer() || value["color_id"].get<std::int64_t>() < -1 ||
            value["color_id"].get<std::int64_t>() > 6 || !value["chat_ids"].is_array() ||
            value["chat_ids"].empty() || value["chat_ids"].size() > 100) {
            return false;
        }
        {
            std::set<std::int64_t> ids;
            return std::ranges::all_of(value["chat_ids"], [&](const json& id) {
                return valid_int53(id) && id != 0 && ids.insert(id.get<std::int64_t>()).second;
            });
        }
    case O::FolderEdit:
        return exact_fields(value, {"operation", "account", "tdlib_request", "folder_id", "before",
                                    "after"}) &&
               valid_positive_int32(value["folder_id"]) && valid_folder_snapshot(value["before"]) &&
               valid_folder_snapshot(value["after"]) &&
               value["before"]["id"] == value["folder_id"] &&
               value["after"]["id"] == value["folder_id"] && value["before"] != value["after"];
    case O::FolderDelete:
        return exact_fields(
                   value, {"operation", "account", "tdlib_request", "folder", "leave_chat_ids"}) &&
               valid_folder_snapshot(value["folder"]) && value["leave_chat_ids"].is_array() &&
               value["leave_chat_ids"].empty();
    case O::FolderAddChat:
    case O::FolderRemoveChat:
        return exact_fields(value, {"operation", "account", "tdlib_request", "folder_id", "chat",
                                    "before", "after"}) &&
               valid_positive_int32(value["folder_id"]) && valid_chat_identity(value["chat"]) &&
               valid_folder_snapshot(value["before"]) && valid_folder_snapshot(value["after"]) &&
               value["before"]["id"] == value["folder_id"] &&
               value["after"]["id"] == value["folder_id"] && value["before"] != value["after"];
    case O::TopicCreate:
        return exact_fields(value,
                            {"operation", "account", "tdlib_request", "chat", "name", "icon"}) &&
               valid_chat_identity(value["chat"]) && value["name"].is_string() &&
               valid_m6_canonical_text(M6TextKind::TopicName,
                                       value["name"].get_ref<const std::string&>()) &&
               value["icon"].is_string() &&
               parse_m6_topic_color(value["icon"].get_ref<const std::string&>()).has_value();
    case O::TopicEdit:
        return exact_fields(value,
                            {"operation", "account", "tdlib_request", "chat", "before", "name"}) &&
               valid_chat_identity(value["chat"]) && valid_topic_info(value["before"]) &&
               value["name"].is_string() &&
               valid_m6_canonical_text(M6TextKind::TopicName,
                                       value["name"].get_ref<const std::string&>()) &&
               value["name"] != value["before"]["name"];
    case O::TopicClose:
    case O::TopicReopen:
        return exact_fields(
                   value, {"operation", "account", "tdlib_request", "chat", "before", "closed"}) &&
               valid_chat_identity(value["chat"]) && valid_topic_info(value["before"]) &&
               value["closed"] == (operation == O::TopicClose) &&
               value["before"]["is_closed"] != value["closed"];
    case O::ChatSetTitle:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "title"}) &&
               valid_chat_identity(value["chat"]) && value["title"].is_string() &&
               valid_m6_canonical_text(M6TextKind::ChatTitle,
                                       value["title"].get_ref<const std::string&>());
    case O::ChatSetPhoto:
        return value.contains("delete") && value.contains("file") &&
               exact_fields(value,
                            {"operation", "account", "tdlib_request", "chat", "delete", "file"}) &&
               valid_chat_identity(value["chat"]) && value["delete"].is_boolean() &&
               (value["delete"] == true ? value["file"].is_null()
                                        : valid_file_snapshot(value["file"]));
    case O::ChatSetDescription:
        return exact_fields(value,
                            {"operation", "account", "tdlib_request", "chat", "description"}) &&
               valid_chat_identity(value["chat"]) && value["description"].is_string() &&
               valid_m6_canonical_text(M6TextKind::ChatDescription,
                                       value["description"].get_ref<const std::string&>());
    case O::ChatInviteLink:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "action",
                                    "invite_link_sha256"}) &&
               valid_chat_identity(value["chat"]) &&
               ((value["action"] == "create" && value["invite_link_sha256"].is_null() &&
                 value["tdlib_request"] == "createChatInviteLink") ||
                (value["action"] == "revoke" && valid_hash(value["invite_link_sha256"]) &&
                 value["tdlib_request"] == "revokeChatInviteLink"));
    case O::ChatPromote:
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "user",
                                    "before", "can_manage_chat", "rights"}) &&
               valid_chat_identity(value["chat"]) && valid_user_identity(value["user"]) &&
               valid_member_status(value["before"]) && value["can_manage_chat"] == true &&
               valid_closed_array(value["rights"], m6_admin_right_names(), 1);
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick: {
        const auto expected = operation == O::ChatDemote ? "member"
                              : operation == O::ChatBan  ? "banned"
                                                         : "left";
        return exact_fields(value, {"operation", "account", "tdlib_request", "chat", "user",
                                    "before", "after"}) &&
               valid_chat_identity(value["chat"]) && valid_user_identity(value["user"]) &&
               valid_member_status(value["before"]) && value["after"] == expected;
    }
    case O::ChatSetPermissions:
        return exact_fields(value,
                            {"operation", "account", "tdlib_request", "chat", "permissions"}) &&
               valid_chat_identity(value["chat"]) &&
               valid_closed_array(value["permissions"], m6_chat_permission_names(), 0);
    case O::StorageOptimize:
        return exact_fields(value, {"operation", "account", "tdlib_request", "size", "ttl", "count",
                                    "immunity_delay", "file_types", "chat_ids", "exclude_chat_ids",
                                    "return_deleted_file_statistics", "chat_limit"}) &&
               value["size"] == -1 && value["ttl"] == -1 && value["count"] == -1 &&
               value["immunity_delay"] == -1 && value["file_types"].is_array() &&
               value["file_types"].empty() && value["chat_ids"].is_array() &&
               value["chat_ids"].empty() && value["exclude_chat_ids"].is_array() &&
               value["exclude_chat_ids"].empty() &&
               value["return_deleted_file_statistics"] == false && value["chat_limit"] == 100;
    case O::ContactList:
    case O::ContactSearch:
    case O::FolderList:
    case O::FolderShow:
    case O::TopicList:
    case O::StorageStats:
        return false;
    }
    return false;
}

bool valid_m6_audit_result(proto::M6Operation operation, const json& value) {
    switch (operation) {
    case O::ContactAdd:
    case O::ContactRemove:
        return exact_fields(value, {"user", "is_contact"}) && valid_user_identity(value["user"]) &&
               value["is_contact"] == (operation == O::ContactAdd);
    case O::ContactBlock:
    case O::ContactUnblock:
        return exact_fields(value, {"user", "blocked"}) && valid_user_identity(value["user"]) &&
               value["blocked"] == (operation == O::ContactBlock);
    case O::FolderCreate:
    case O::FolderEdit:
        return exact_fields(value, {"folder"}) && valid_folder_snapshot(value["folder"]);
    case O::FolderDelete:
        return exact_fields(value, {"folder_id", "deleted"}) &&
               valid_positive_int32(value["folder_id"]) && value["deleted"] == true;
    case O::FolderAddChat:
    case O::FolderRemoveChat:
        return exact_fields(value, {"folder", "chat", "included"}) &&
               valid_folder_snapshot(value["folder"]) && valid_chat_identity(value["chat"]) &&
               value["included"] == (operation == O::FolderAddChat);
    case O::TopicCreate:
        return exact_fields(value, {"topic"}) && valid_topic_info(value["topic"]);
    case O::TopicEdit:
        return exact_fields(value, {"chat", "topic_id", "name"}) &&
               valid_chat_identity(value["chat"]) && valid_positive_int32(value["topic_id"]) &&
               value["name"].is_string() &&
               valid_m6_canonical_text(M6TextKind::TopicName,
                                       value["name"].get_ref<const std::string&>());
    case O::TopicClose:
    case O::TopicReopen:
        return exact_fields(value, {"chat", "topic_id", "closed"}) &&
               valid_chat_identity(value["chat"]) && valid_positive_int32(value["topic_id"]) &&
               value["closed"] == (operation == O::TopicClose);
    case O::ChatSetTitle:
        return exact_fields(value, {"chat", "title"}) && valid_chat_identity(value["chat"]) &&
               value["title"].is_string();
    case O::ChatSetPhoto:
        return exact_fields(value, {"chat", "photo"}) && valid_chat_identity(value["chat"]) &&
               (value["photo"] == "set" || value["photo"] == "deleted");
    case O::ChatSetDescription:
        return exact_fields(value, {"chat", "description"}) && valid_chat_identity(value["chat"]) &&
               value["description"].is_string();
    case O::ChatInviteLink:
        return exact_fields(value, {"chat", "action", "invite_link"}) &&
               valid_chat_identity(value["chat"]) &&
               ((value["action"] == "create" && valid_string(value["invite_link"], false)) ||
                (value["action"] == "revoke" &&
                 (value["invite_link"].is_null() || valid_string(value["invite_link"], false))));
    case O::ChatPromote:
        return exact_fields(value, {"chat", "user", "status", "can_manage_chat", "rights"}) &&
               valid_chat_identity(value["chat"]) && valid_user_identity(value["user"]) &&
               value["status"] == "administrator" && value["can_manage_chat"] == true &&
               valid_closed_array(value["rights"], m6_admin_right_names(), 1);
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick:
        return exact_fields(value, {"chat", "user", "status"}) &&
               valid_chat_identity(value["chat"]) && valid_user_identity(value["user"]) &&
               value["status"] == (operation == O::ChatDemote ? "member"
                                   : operation == O::ChatBan  ? "banned"
                                                              : "left");
    case O::ChatSetPermissions:
        return exact_fields(value, {"chat", "permissions"}) && valid_chat_identity(value["chat"]) &&
               valid_closed_array(value["permissions"], m6_chat_permission_names(), 0);
    case O::StorageOptimize:
        return exact_fields(value, {"optimized", "statistics"}) && value["optimized"] == true &&
               valid_storage_statistics(value["statistics"]);
    case O::ContactList:
    case O::ContactSearch:
    case O::FolderList:
    case O::FolderShow:
    case O::TopicList:
    case O::StorageStats:
        return false;
    }
    return false;
}

bool m6_result_matches_plan(proto::M6Operation operation, const json& result, const json& plan) {
    if (!valid_m6_audit_result(operation, result) || !plan.is_object()) {
        return false;
    }
    if (result.contains("chat") && plan.contains("chat") && result["chat"] != plan["chat"]) {
        return false;
    }
    if (result.contains("user") && plan.contains("user") && result["user"] != plan["user"]) {
        return false;
    }
    switch (operation) {
    case O::ContactAdd:
    case O::ContactRemove:
    case O::ContactBlock:
    case O::ContactUnblock:
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick:
        return true;
    case O::FolderCreate:
        return result["folder"]["name"] == plan["name"] &&
               result["folder"]["color_id"] == plan["color_id"] &&
               (plan["icon"].is_null() || result["folder"]["icon"] == plan["icon"]) &&
               result["folder"]["pinned_chat_ids"].empty() &&
               result["folder"]["included_chat_ids"] == plan["chat_ids"] &&
               result["folder"]["excluded_chat_ids"].empty();
    case O::FolderEdit:
        return result["folder"] == plan["after"];
    case O::FolderDelete:
        return result["folder_id"] == plan["folder"]["id"];
    case O::FolderAddChat:
    case O::FolderRemoveChat:
        return result["folder"] == plan["after"] && result["chat"] == plan["chat"];
    case O::TopicCreate:
        return result["topic"]["chat_id"] == plan["chat"]["id"] &&
               result["topic"]["name"] == plan["name"];
    case O::TopicEdit:
        return result["topic_id"] == plan["before"]["id"] && result["name"] == plan["name"];
    case O::TopicClose:
    case O::TopicReopen:
        return result["topic_id"] == plan["before"]["id"] && result["closed"] == plan["closed"];
    case O::ChatSetTitle:
        return result["title"] == plan["title"];
    case O::ChatSetPhoto:
        return result["photo"] == (plan["delete"] == true ? "deleted" : "set");
    case O::ChatSetDescription:
        return result["description"] == plan["description"];
    case O::ChatInviteLink:
        return result["action"] == plan["action"];
    case O::ChatPromote:
        return result["rights"] == plan["rights"];
    case O::ChatSetPermissions:
        return result["permissions"] == plan["permissions"];
    case O::StorageOptimize:
        return true;
    case O::ContactList:
    case O::ContactSearch:
    case O::FolderList:
    case O::FolderShow:
    case O::TopicList:
    case O::StorageStats:
        return false;
    }
    return false;
}

bool m6_arguments_match_plan(proto::M6Operation operation, const json& arguments,
                             const json& plan) {
    if (!valid_m6_audit_arguments(operation, arguments) || !plan.is_object()) {
        return false;
    }
    auto expected = plan;
    expected.erase("operation");
    expected.erase("account");
    expected.erase("tdlib_request");
    return expected == arguments;
}

} // namespace tgcli::daemon
