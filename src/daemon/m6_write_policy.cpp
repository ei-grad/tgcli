#include "daemon/m6_write_policy.hpp"

#include <algorithm>

namespace tgcli::daemon {

namespace {

using O = proto::M6Operation;

constexpr std::array<M6WritePolicy, 24> kPolicies{{
    {O::ContactAdd, "contact_add", {"addContact", {}}, 1, true, false},
    {O::ContactRemove, "contact_remove", {"removeContacts", {}}, 1, true, false},
    {O::ContactBlock, "contact_block", {"setMessageSenderBlockList", {}}, 1, true, false},
    {O::ContactUnblock, "contact_unblock", {"setMessageSenderBlockList", {}}, 1, true, false},
    {O::FolderCreate, "folder_create", {"createChatFolder", {}}, 1, true, false},
    {O::FolderEdit, "folder_edit", {"editChatFolder", {}}, 1, true, false},
    {O::FolderDelete, "folder_delete", {"deleteChatFolder", {}}, 1, true, false},
    {O::FolderAddChat, "folder_add_chat", {"editChatFolder", {}}, 1, true, false},
    {O::FolderRemoveChat, "folder_remove_chat", {"editChatFolder", {}}, 1, true, false},
    {O::TopicCreate, "topic_create", {"createForumTopic", {}}, 1, true, false},
    {O::TopicEdit, "topic_edit", {"editForumTopic", {}}, 1, true, false},
    {O::TopicClose, "topic_close", {"toggleForumTopicIsClosed", {}}, 1, true, false},
    {O::TopicReopen, "topic_reopen", {"toggleForumTopicIsClosed", {}}, 1, true, false},
    {O::ChatSetTitle, "chat_set_title", {"setChatTitle", {}}, 1, true, false},
    {O::ChatSetPhoto, "chat_set_photo", {"setChatPhoto", {}}, 1, true, true},
    {O::ChatSetDescription, "chat_set_description", {"setChatDescription", {}}, 1, true, false},
    {O::ChatInviteLink,
     "chat_invite_link",
     {"createChatInviteLink", "revokeChatInviteLink"},
     2,
     false,
     false},
    {O::ChatPromote, "chat_promote", {"setChatMemberStatus", {}}, 1, true, false},
    {O::ChatDemote, "chat_demote", {"setChatMemberStatus", {}}, 1, true, false},
    {O::ChatBan, "chat_ban", {"setChatMemberStatus", {}}, 1, true, false},
    {O::ChatUnban, "chat_unban", {"setChatMemberStatus", {}}, 1, true, false},
    {O::ChatKick, "chat_kick", {"setChatMemberStatus", {}}, 1, true, false},
    {O::ChatSetPermissions, "chat_set_permissions", {"setChatPermissions", {}}, 1, true, false},
    {O::StorageOptimize, "storage_optimize", {"optimizeStorage", {}}, 1, false, false},
}};

} // namespace

std::span<const M6WritePolicy> m6_write_policies() noexcept {
    return kPolicies;
}

const M6WritePolicy* m6_write_policy(proto::M6Operation operation) noexcept {
    const auto found = std::ranges::find(kPolicies, operation, &M6WritePolicy::operation);
    return found == kPolicies.end() ? nullptr : &*found;
}

std::optional<proto::M6Operation> parse_m6_write_operation(std::string_view audit_name) noexcept {
    const auto found = std::ranges::find(kPolicies, audit_name, &M6WritePolicy::audit_name);
    return found == kPolicies.end() ? std::nullopt : std::optional{found->operation};
}

bool valid_m6_tdlib_function(proto::M6Operation operation, std::string_view function) noexcept {
    const auto* policy = m6_write_policy(operation);
    return policy != nullptr &&
           std::ranges::find(std::span(policy->tdlib_functions).first(policy->tdlib_function_count),
                             function) !=
               std::span(policy->tdlib_functions).first(policy->tdlib_function_count).end();
}

} // namespace tgcli::daemon
