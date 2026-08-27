#include "daemon/m6_model.hpp"

#include "common/utf8.hpp"
#include "daemon/fetch_domain.hpp"
#include "daemon/m6_domain.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

bool valid_user_id(std::int64_t value) {
    return value > 0 && value <= kMaximumInt53;
}

bool valid_chat_id(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_usernames(const std::vector<std::string>& usernames) {
    std::unordered_set<std::string> seen;
    return std::ranges::all_of(usernames, [&](const auto& username) {
        return !username.empty() && common::valid_utf8(username) && seen.insert(username).second;
    });
}

std::optional<std::size_t> utf16_code_units(std::string_view text) {
    std::size_t units = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        std::size_t bytes = 1;
        std::size_t scalar_units = 1;
        if (first >= 0xC2U && first <= 0xDFU) {
            bytes = 2;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            bytes = 3;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            bytes = 4;
            scalar_units = 2;
        }
        if (offset > text.size() - bytes ||
            units > std::numeric_limits<std::size_t>::max() - scalar_units) {
            return std::nullopt;
        }
        offset += bytes;
        units += scalar_units;
    }
    return units;
}

bool is_utf16_boundary(std::string_view text, std::size_t target) {
    std::size_t units = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        if (units == target) {
            return true;
        }
        const auto first = static_cast<unsigned char>(text[offset]);
        std::size_t bytes = 1;
        std::size_t scalar_units = 1;
        if (first >= 0xC2U && first <= 0xDFU) {
            bytes = 2;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            bytes = 3;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            bytes = 4;
            scalar_units = 2;
        }
        offset += bytes;
        units += scalar_units;
    }
    return units == target;
}

std::string_view folder_icon_name(core::TdM6FolderIcon icon) {
    const auto index = static_cast<std::size_t>(icon);
    const auto names = m6_folder_icon_names();
    return index < names.size() ? names[index] : std::string_view{};
}

std::string_view topic_color_name(core::TdM6TopicColor color) {
    const auto index = static_cast<std::size_t>(color);
    const auto names = m6_topic_color_names();
    return index < names.size() ? names[index] : std::string_view{};
}

std::optional<std::int64_t> int64_string(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        std::to_string(parsed) != value) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<json> folder_name_json(const core::TdM6FolderName& name) {
    if (!valid_m6_canonical_text(M6TextKind::FolderName, name.text)) {
        return std::nullopt;
    }
    const auto text_units = utf16_code_units(name.text);
    if (!text_units ||
        *text_units > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    json entities = json::array();
    std::int64_t previous_end = 0;
    for (const auto& entity : name.custom_emoji_entities) {
        const auto custom_emoji_id = int64_string(entity.custom_emoji_id);
        const auto end = static_cast<std::int64_t>(entity.offset) + entity.length;
        if (!custom_emoji_id || *custom_emoji_id <= 0 || entity.offset < 0 || entity.length <= 0 ||
            entity.offset < previous_end || end > static_cast<std::int64_t>(*text_units) ||
            !is_utf16_boundary(name.text, static_cast<std::size_t>(entity.offset)) ||
            !is_utf16_boundary(name.text, static_cast<std::size_t>(end))) {
            return std::nullopt;
        }
        entities.push_back({{"offset", entity.offset},
                            {"length", entity.length},
                            {"custom_emoji_id", entity.custom_emoji_id}});
        previous_end = end;
    }
    return json{{"text", name.text},
                {"animate_custom_emoji", name.animate_custom_emoji},
                {"custom_emoji_entities", std::move(entities)}};
}

bool duplicate_or_crossed(const core::TdM6ChatFolder& folder) {
    std::set<std::int64_t> seen;
    const auto add = [&](const auto& ids) {
        for (const auto id : ids) {
            if (!valid_chat_id(id) || !seen.insert(id).second) {
                return false;
            }
        }
        return true;
    };
    return !add(folder.pinned_chat_ids) || !add(folder.included_chat_ids) ||
           !add(folder.excluded_chat_ids);
}

std::optional<json> sender_json(const core::TdM6Sender& sender) {
    if (sender.kind == core::TdM6SenderKind::User && valid_user_id(sender.id)) {
        return json{{"type", "user"}, {"id", sender.id}};
    }
    if (sender.kind == core::TdM6SenderKind::Chat && valid_chat_id(sender.id)) {
        return json{{"type", "chat"}, {"id", sender.id}};
    }
    return std::nullopt;
}

std::vector<std::string> right_names(const core::TdM6AdminRights& rights) {
    const std::array enabled{
        rights.can_change_info,     rights.can_post_messages,
        rights.can_edit_messages,   rights.can_delete_messages,
        rights.can_invite_users,    rights.can_restrict_members,
        rights.can_pin_messages,    rights.can_manage_topics,
        rights.can_promote_members, rights.can_manage_video_chats,
        rights.can_post_stories,    rights.can_edit_stories,
        rights.can_delete_stories,  rights.can_manage_direct_messages,
        rights.can_manage_tags,     rights.is_anonymous,
    };
    std::vector<std::string> result;
    const auto names = m6_admin_right_names();
    for (std::size_t index = 0; index < enabled.size(); ++index) {
        if (enabled[index]) {
            result.emplace_back(names[index]);
        }
    }
    return result;
}

std::vector<std::string> permission_names(const core::TdM6ChatPermissions& permissions) {
    const std::array enabled{
        permissions.can_send_basic_messages, permissions.can_send_audios,
        permissions.can_send_documents,      permissions.can_send_photos,
        permissions.can_send_videos,         permissions.can_send_video_notes,
        permissions.can_send_voice_notes,    permissions.can_send_polls,
        permissions.can_send_other_messages, permissions.can_add_link_previews,
        permissions.can_react_to_messages,   permissions.can_edit_tag,
        permissions.can_change_info,         permissions.can_invite_users,
        permissions.can_pin_messages,        permissions.can_create_topics,
    };
    std::vector<std::string> result;
    const auto names = m6_chat_permission_names();
    for (std::size_t index = 0; index < enabled.size(); ++index) {
        if (enabled[index]) {
            result.emplace_back(names[index]);
        }
    }
    return result;
}

std::string_view storage_file_type_name(core::TdM6StorageFileType type) {
    static constexpr std::array<std::string_view, 25> names{
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
        "wallpaper",
    };
    const auto index = static_cast<std::size_t>(type);
    return index < names.size() ? names[index] : std::string_view{};
}

} // namespace

std::optional<UserIdentity> m6_user_identity(const core::TdUserSummary& user) {
    if (!valid_user_id(user.id) || !common::valid_utf8(user.first_name) ||
        !common::valid_utf8(user.last_name) || !common::valid_utf8(user.phone_number) ||
        !valid_usernames(user.usernames)) {
        return std::nullopt;
    }
    std::string display_name = user.first_name;
    if (!user.last_name.empty()) {
        if (!display_name.empty()) {
            display_name.push_back(' ');
        }
        display_name.append(user.last_name);
    }
    return UserIdentity{.id = user.id,
                        .display_name = std::move(display_name),
                        .usernames = user.usernames,
                        .is_bot = user.is_bot};
}

json m6_user_identity_json(const UserIdentity& user) {
    return {{"id", user.id},
            {"display_name", user.display_name},
            {"usernames", user.usernames},
            {"is_bot", user.is_bot}};
}

std::optional<json> m6_contact_list_json(const core::TdM6Users& users,
                                         const std::vector<core::TdUserSummary>& hydrated,
                                         bool search) {
    constexpr std::size_t maximum_list_users = 131'072;
    constexpr std::size_t maximum_search_users = 100;
    constexpr std::size_t maximum_bytes = 16'777'216;
    constexpr std::size_t maximum_item_bytes = 262'144;
    const auto maximum_users = search ? maximum_search_users : maximum_list_users;
    if (users.total_count < 0 ||
        static_cast<std::size_t>(users.total_count) < users.user_ids.size() ||
        users.user_ids.size() > maximum_users || hydrated.size() != users.user_ids.size()) {
        return std::nullopt;
    }
    std::unordered_set<std::int64_t> seen;
    std::size_t charged_bytes = 0;
    json items = json::array();
    for (std::size_t index = 0; index < users.user_ids.size(); ++index) {
        const auto id = users.user_ids[index];
        const auto identity = m6_user_identity(hydrated[index]);
        if (!valid_user_id(id) || !seen.insert(id).second || !identity || identity->id != id) {
            return std::nullopt;
        }
        auto item = m6_user_identity_json(*identity);
        const auto bytes = item.dump().size();
        if (bytes > maximum_item_bytes || charged_bytes > maximum_bytes - bytes) {
            return std::nullopt;
        }
        charged_bytes += bytes;
        items.push_back(std::move(item));
    }
    return json{{"items", std::move(items)}, {"next", nullptr}};
}

std::optional<json> m6_folder_list_json(const core::TdM6ChatFoldersUpdate& update) {
    if (!core::valid_td_m6_chat_folders_update(update)) {
        return std::nullopt;
    }
    json items = json::array();
    for (const auto& folder : update.folders) {
        auto summary = m6_folder_summary_json(folder);
        if (!summary) {
            return std::nullopt;
        }
        items.push_back(std::move(*summary));
    }
    return json{{"items", std::move(items)}, {"next", nullptr}};
}

std::optional<json> m6_session_list_json(const core::TdSessions& sessions) {
    if (sessions.inactive_session_ttl_days < 1 || sessions.inactive_session_ttl_days > 366 ||
        sessions.items.size() > 4'096) {
        return std::nullopt;
    }
    std::unordered_set<std::string> ids;
    json items = json::array();
    for (const auto& session : sessions.items) {
        if (!int64_string(session.id) || !ids.insert(session.id).second) {
            return std::nullopt;
        }
        items.push_back(
            {{"id", session.id},
             {"is_current", session.is_current},
             {"is_password_pending", session.is_password_pending},
             {"is_unconfirmed", session.is_unconfirmed},
             {"can_accept_secret_chats", session.can_accept_secret_chats},
             {"can_accept_calls", session.can_accept_calls},
             {"device_type", core::td_session_device_type_name(session.device_type)},
             {"api_id", session.api_id},
             {"application_name", session.application_name},
             {"application_version", session.application_version},
             {"is_official_application", session.is_official_application},
             {"device_model", session.device_model},
             {"platform", session.platform},
             {"system_version", session.system_version},
             {"log_in_date", session.log_in_date ? json(*session.log_in_date) : json(nullptr)},
             {"last_active_date",
              session.last_active_date ? json(*session.last_active_date) : json(nullptr)},
             {"ip_address", session.ip_address},
             {"location", session.location}});
    }
    return json{{"items", std::move(items)},
                {"inactive_session_ttl_days", sessions.inactive_session_ttl_days},
                {"next", nullptr}};
}

std::optional<json> m6_folder_summary_json(const core::TdM6FolderInfo& info) {
    const auto name = folder_name_json(info.name);
    const auto icon = folder_icon_name(info.icon);
    if (!name || icon.empty() || info.id <= 0 || info.color_id < -1 || info.color_id > 6) {
        return std::nullopt;
    }
    return json{{"id", info.id},
                {"name", *name},
                {"icon", icon},
                {"color_id", info.color_id},
                {"is_shareable", info.is_shareable},
                {"has_my_invite_links", info.has_my_invite_links}};
}

std::optional<json> m6_folder_snapshot_json(std::int32_t folder_id,
                                            const core::TdM6ChatFolder& folder,
                                            const core::TdM6FolderInfo& info) {
    const auto name = folder_name_json(folder.name);
    const auto configured_icon = folder.icon ? folder_icon_name(*folder.icon) : std::string_view{};
    if (!name || folder_id <= 0 || info.id != folder_id ||
        (folder.icon && configured_icon.empty()) || folder.color_id < -1 || folder.color_id > 6 ||
        folder.pinned_chat_ids.size() + folder.included_chat_ids.size() > 100 ||
        folder.excluded_chat_ids.size() > 100 || duplicate_or_crossed(folder)) {
        return std::nullopt;
    }
    return json{{"id", folder_id},
                {"name", *name},
                {"icon", folder.icon ? json(configured_icon) : json(nullptr)},
                {"color_id", folder.color_id},
                {"is_shareable", folder.is_shareable},
                {"has_my_invite_links", info.has_my_invite_links},
                {"pinned_chat_ids", folder.pinned_chat_ids},
                {"included_chat_ids", folder.included_chat_ids},
                {"excluded_chat_ids", folder.excluded_chat_ids},
                {"exclude_muted", folder.exclude_muted},
                {"exclude_read", folder.exclude_read},
                {"exclude_archived", folder.exclude_archived},
                {"include_contacts", folder.include_contacts},
                {"include_non_contacts", folder.include_non_contacts},
                {"include_bots", folder.include_bots},
                {"include_groups", folder.include_groups},
                {"include_channels", folder.include_channels}};
}

std::optional<json> m6_topic_info_json(const core::TdM6ForumTopicInfo& topic) {
    const auto color = topic_color_name(topic.icon.color);
    const auto creator = sender_json(topic.creator);
    const auto creation_date = format_fetch_timestamp(topic.creation_date);
    if (!valid_chat_id(topic.chat_id) || topic.id <= 0 ||
        !valid_m6_canonical_text(M6TextKind::TopicName, topic.name) || color.empty() ||
        !int64_string(topic.icon.custom_emoji_id) || topic.creation_date <= 0 || !creation_date ||
        !creator) {
        return std::nullopt;
    }
    return json{{"chat_id", topic.chat_id},
                {"id", topic.id},
                {"name", topic.name},
                {"icon", {{"color", color}, {"custom_emoji_id", topic.icon.custom_emoji_id}}},
                {"creation_date", *creation_date},
                {"creator", *creator},
                {"is_general", topic.is_general},
                {"is_outgoing", topic.is_outgoing},
                {"is_closed", topic.is_closed},
                {"is_hidden", topic.is_hidden},
                {"is_name_implicit", topic.is_name_implicit}};
}

std::optional<json> m6_topic_row_json(const core::TdM6ForumTopic& topic) {
    auto info = m6_topic_info_json(topic.info);
    if (!info || topic.unread_count < 0 || topic.unread_mention_count < 0 ||
        topic.unread_reaction_count < 0 || topic.unread_poll_vote_count < 0) {
        return std::nullopt;
    }
    (*info)["is_pinned"] = topic.is_pinned;
    (*info)["unread_count"] = topic.unread_count;
    (*info)["unread_mention_count"] = topic.unread_mention_count;
    (*info)["unread_reaction_count"] = topic.unread_reaction_count;
    (*info)["unread_poll_vote_count"] = topic.unread_poll_vote_count;
    return info;
}

std::optional<json> m6_member_status_json(const core::TdM6MemberStatus& status) {
    switch (status.kind) {
    case core::TdM6MemberStatusKind::Creator:
        return json{{"kind", "creator"},
                    {"is_anonymous", status.is_anonymous},
                    {"is_member", status.is_member}};
    case core::TdM6MemberStatusKind::Administrator:
        return json{{"kind", "administrator"},
                    {"can_be_edited", status.can_be_edited},
                    {"can_manage_chat", status.rights.can_manage_chat},
                    {"rights", right_names(status.rights)}};
    case core::TdM6MemberStatusKind::Member:
        return json{{"kind", "member"}, {"member_until_date", status.member_until_date}};
    case core::TdM6MemberStatusKind::Restricted:
        return json{{"kind", "restricted"},
                    {"is_member", status.is_member},
                    {"restricted_until_date", status.restricted_until_date},
                    {"permissions", permission_names(status.permissions)}};
    case core::TdM6MemberStatusKind::Left:
        return json{{"kind", "left"}};
    case core::TdM6MemberStatusKind::Banned:
        return json{{"kind", "banned"}, {"banned_until_date", status.banned_until_date}};
    case core::TdM6MemberStatusKind::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<json> m6_invite_link_json(const core::TdM6ChatInviteLink& link) {
    if (!valid_m6_invite_link(link.invite_link) || !common::valid_utf8(link.name) ||
        link.creator_user_id <= 0 || link.date <= 0 || link.edit_date < 0 ||
        (link.edit_date != 0 && link.edit_date < link.date) || link.expiration_date < 0 ||
        (link.expiration_date != 0 && link.expiration_date < link.date) || link.member_limit < 0 ||
        link.member_limit > 99'999 || link.member_count < 0 || link.expired_member_count < 0 ||
        link.pending_join_request_count < 0 ||
        (link.member_limit > 0 && link.member_count > link.member_limit) ||
        (link.creates_join_request && link.member_limit != 0)) {
        return std::nullopt;
    }
    json pricing = nullptr;
    if (link.subscription_pricing) {
        const auto& value = *link.subscription_pricing;
        if ((value.period != 60 && value.period != 300 && value.period != 2'592'000) ||
            value.star_count <= 0 || value.star_count > kMaximumInt53 ||
            link.expiration_date != 0 || link.member_limit != 0 || link.creates_join_request ||
            link.is_primary) {
            return std::nullopt;
        }
        pricing = {{"period", value.period}, {"star_count", value.star_count}};
    } else if (link.expired_member_count != 0) {
        return std::nullopt;
    }
    if (!link.creates_join_request && link.pending_join_request_count != 0) {
        return std::nullopt;
    }
    if (link.is_primary &&
        (!link.name.empty() || link.expiration_date != 0 || link.member_limit != 0 ||
         link.creates_join_request || link.subscription_pricing)) {
        return std::nullopt;
    }
    return json{{"invite_link", link.invite_link},
                {"name", link.name},
                {"creator_user_id", link.creator_user_id},
                {"date", link.date},
                {"edit_date", link.edit_date},
                {"expiration_date", link.expiration_date},
                {"member_limit", link.member_limit},
                {"member_count", link.member_count},
                {"expired_member_count", link.expired_member_count},
                {"pending_join_request_count", link.pending_join_request_count},
                {"creates_join_request", link.creates_join_request},
                {"is_primary", link.is_primary},
                {"is_revoked", link.is_revoked},
                {"subscription_pricing", std::move(pricing)}};
}

std::optional<json> m6_storage_statistics_json(const core::TdM6StorageStatistics& statistics) {
    if (statistics.size < 0 || statistics.size > kMaximumInt53 || statistics.count < 0 ||
        statistics.by_chat.size() > 101) {
        return std::nullopt;
    }
    std::int64_t total_size = 0;
    std::int64_t total_count = 0;
    bool aggregate_seen = false;
    std::unordered_set<std::int64_t> chats;
    json by_chat = json::array();
    for (const auto& chat : statistics.by_chat) {
        if (chat.chat_id == 0) {
            if (aggregate_seen) {
                return std::nullopt;
            }
            aggregate_seen = true;
        } else if (!valid_chat_id(chat.chat_id) || !chats.insert(chat.chat_id).second) {
            return std::nullopt;
        }
        if (chat.size < 0 || chat.size > kMaximumInt53 || chat.count < 0 ||
            chat.by_file_type.size() > 25) {
            return std::nullopt;
        }
        std::int64_t chat_size = 0;
        std::int64_t chat_count = 0;
        std::set<core::TdM6StorageFileType> file_types;
        json by_type = json::array();
        for (const auto& item : chat.by_file_type) {
            const auto type = storage_file_type_name(item.file_type);
            if (type.empty() || !file_types.insert(item.file_type).second || item.size < 0 ||
                item.size > kMaximumInt53 || item.count < 0 ||
                chat_size > kMaximumInt53 - item.size ||
                chat_count > std::numeric_limits<std::int32_t>::max() - item.count) {
                return std::nullopt;
            }
            chat_size += item.size;
            chat_count += item.count;
            by_type.push_back({{"file_type", type}, {"size", item.size}, {"count", item.count}});
        }
        if (chat_size != chat.size || chat_count != chat.count ||
            total_size > kMaximumInt53 - chat.size ||
            total_count > std::numeric_limits<std::int32_t>::max() - chat.count) {
            return std::nullopt;
        }
        total_size += chat.size;
        total_count += chat.count;
        by_chat.push_back({{"chat_id", chat.chat_id},
                           {"size", chat.size},
                           {"count", chat.count},
                           {"by_file_type", std::move(by_type)}});
    }
    if (total_size != statistics.size || total_count != statistics.count) {
        return std::nullopt;
    }
    return json{
        {"size", statistics.size}, {"count", statistics.count}, {"by_chat", std::move(by_chat)}};
}

} // namespace tgcli::daemon
