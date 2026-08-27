#pragma once

#include "core/td_runtime.hpp"
#include "daemon/resolver.hpp"

#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

std::optional<UserIdentity> m6_user_identity(const core::TdUserSummary& user);
nlohmann::json m6_user_identity_json(const UserIdentity& user);
std::optional<nlohmann::json> m6_contact_list_json(const core::TdM6Users& users,
                                                   const std::vector<core::TdUserSummary>& hydrated,
                                                   bool search);
std::optional<nlohmann::json> m6_folder_list_json(const core::TdM6ChatFoldersUpdate& update);
std::optional<nlohmann::json> m6_session_list_json(const core::TdSessions& sessions);

std::optional<nlohmann::json> m6_folder_summary_json(const core::TdM6FolderInfo& info);
std::optional<nlohmann::json> m6_folder_snapshot_json(std::int32_t folder_id,
                                                      const core::TdM6ChatFolder& folder,
                                                      const core::TdM6FolderInfo& info);

std::optional<nlohmann::json> m6_topic_info_json(const core::TdM6ForumTopicInfo& topic);
std::optional<nlohmann::json> m6_topic_row_json(const core::TdM6ForumTopic& topic);

std::optional<nlohmann::json> m6_member_status_json(const core::TdM6MemberStatus& status);
std::optional<nlohmann::json> m6_invite_link_json(const core::TdM6ChatInviteLink& invite_link);
std::optional<nlohmann::json>
m6_storage_statistics_json(const core::TdM6StorageStatistics& statistics);

} // namespace tgcli::daemon
