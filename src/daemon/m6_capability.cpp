#include "daemon/m6_capability.hpp"

#include <array>
#include <cstddef>

namespace tgcli::daemon {

namespace {

using K = M6ChatKind;
using R = M6AdminRight;

constexpr std::array<std::array<bool, 16>, 4> kApplicability{{
    {{true, false, false, true, true, true, true, false, true, true, false, false, false, false,
      true, false}},
    {{true, false, false, true, true, true, true, true, true, true, true, true, true, false, true,
      true}},
    {{true, false, false, true, true, true, true, false, true, true, true, true, true, false, true,
      true}},
    {{true, true, true, true, true, true, false, false, true, true, true, true, true, true, false,
      false}},
}};

std::array<bool, 16> enabled(const core::TdM6AdminRights& rights) {
    return {rights.can_change_info,     rights.can_post_messages,
            rights.can_edit_messages,   rights.can_delete_messages,
            rights.can_invite_users,    rights.can_restrict_members,
            rights.can_pin_messages,    rights.can_manage_topics,
            rights.can_promote_members, rights.can_manage_video_chats,
            rights.can_post_stories,    rights.can_edit_stories,
            rights.can_delete_stories,  rights.can_manage_direct_messages,
            rights.can_manage_tags,     rights.is_anonymous};
}

bool applicable(M6ChatKind kind, R right) {
    return kApplicability.at(static_cast<std::size_t>(kind)).at(static_cast<std::size_t>(right));
}

bool admin_has(const core::TdM6MemberStatus& status, R right) {
    return enabled(status.rights).at(static_cast<std::size_t>(right));
}

bool caller_has(const core::TdM6MemberStatus& caller, M6ChatKind kind, R right) {
    if (caller.kind == core::TdM6MemberStatusKind::Creator) {
        return applicable(kind, right);
    }
    return caller.kind == core::TdM6MemberStatusKind::Administrator && admin_has(caller, right);
}

bool operation_requires(proto::M6Operation operation, R right) {
    switch (operation) {
    case proto::M6Operation::ChatSetTitle:
    case proto::M6Operation::ChatSetPhoto:
    case proto::M6Operation::ChatSetDescription:
        return right == R::ChangeInfo;
    case proto::M6Operation::ChatInviteLink:
        return right == R::InviteUsers;
    case proto::M6Operation::ChatPromote:
    case proto::M6Operation::ChatDemote:
        return right == R::PromoteMembers;
    case proto::M6Operation::ChatBan:
    case proto::M6Operation::ChatUnban:
    case proto::M6Operation::ChatKick:
    case proto::M6Operation::ChatSetPermissions:
        return right == R::RestrictMembers;
    default:
        return false;
    }
}

bool valid_admin_snapshot(const core::TdM6AdminRights& rights, M6ChatKind kind) {
    const auto values = enabled(rights);
    bool any = false;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values.at(index) && !kApplicability.at(static_cast<std::size_t>(kind)).at(index)) {
            return false;
        }
        any = any || values.at(index);
    }
    return !any || rights.can_manage_chat;
}

} // namespace

bool valid_m6_member_status(const core::TdM6MemberStatus& status, M6ChatKind kind) noexcept {
    switch (status.kind) {
    case core::TdM6MemberStatusKind::Creator:
        return !status.can_be_edited;
    case core::TdM6MemberStatusKind::Administrator:
        return valid_admin_snapshot(status.rights, kind);
    case core::TdM6MemberStatusKind::Member:
        return status.member_until_date >= 0;
    case core::TdM6MemberStatusKind::Restricted:
        return status.restricted_until_date >= 0;
    case core::TdM6MemberStatusKind::Left:
        return true;
    case core::TdM6MemberStatusKind::Banned:
        return status.banned_until_date >= 0;
    case core::TdM6MemberStatusKind::Unknown:
        return false;
    }
    return false;
}

M6CapabilityStatus m6_authorize_caller(proto::M6Operation operation, M6ChatKind kind,
                                       const core::TdM6MemberStatus& caller,
                                       std::span<const M6AdminRight> requested_rights) noexcept {
    if (!valid_m6_member_status(caller, kind)) {
        return M6CapabilityStatus::InvalidSnapshot;
    }
    if ((operation == proto::M6Operation::ChatPromote && kind == K::BasicGroup) ||
        (operation == proto::M6Operation::ChatSetPermissions && kind == K::Channel)) {
        return M6CapabilityStatus::UnsupportedChatType;
    }
    if (caller.kind == core::TdM6MemberStatusKind::Member) {
        if ((operation == proto::M6Operation::ChatSetTitle ||
             operation == proto::M6Operation::ChatSetPhoto ||
             operation == proto::M6Operation::ChatSetDescription) &&
            kind != K::Channel && caller.permissions.can_change_info) {
            return M6CapabilityStatus::Allowed;
        }
        return M6CapabilityStatus::MissingRight;
    }
    if (caller.kind != core::TdM6MemberStatusKind::Creator &&
        caller.kind != core::TdM6MemberStatusKind::Administrator) {
        return M6CapabilityStatus::MissingRight;
    }
    for (std::size_t index = 0; index < 16; ++index) {
        const auto right = static_cast<R>(index);
        if (operation_requires(operation, right) && !caller_has(caller, kind, right)) {
            return M6CapabilityStatus::MissingRight;
        }
    }
    if (operation == proto::M6Operation::ChatPromote) {
        if (requested_rights.empty()) {
            return M6CapabilityStatus::MissingRight;
        }
        for (const auto right : requested_rights) {
            if (!applicable(kind, right) || !caller_has(caller, kind, right)) {
                return M6CapabilityStatus::MissingRight;
            }
        }
    }
    return M6CapabilityStatus::Allowed;
}

M6CapabilityStatus m6_authorize_target(proto::M6Operation operation, std::int64_t principal_id,
                                       std::int64_t target_id,
                                       const core::TdM6MemberStatus& target) noexcept {
    if (target_id <= 0 || target_id == principal_id ||
        target.kind == core::TdM6MemberStatusKind::Creator ||
        (target.kind == core::TdM6MemberStatusKind::Administrator && !target.can_be_edited)) {
        return M6CapabilityStatus::InvalidTarget;
    }
    bool allowed = false;
    switch (operation) {
    case proto::M6Operation::ChatPromote:
        allowed = target.kind == core::TdM6MemberStatusKind::Member ||
                  target.kind == core::TdM6MemberStatusKind::Restricted ||
                  target.kind == core::TdM6MemberStatusKind::Administrator;
        break;
    case proto::M6Operation::ChatDemote:
        allowed = target.kind == core::TdM6MemberStatusKind::Administrator;
        break;
    case proto::M6Operation::ChatBan:
        allowed = target.kind == core::TdM6MemberStatusKind::Member ||
                  target.kind == core::TdM6MemberStatusKind::Restricted ||
                  target.kind == core::TdM6MemberStatusKind::Left;
        break;
    case proto::M6Operation::ChatUnban:
        allowed = target.kind == core::TdM6MemberStatusKind::Banned;
        break;
    case proto::M6Operation::ChatKick:
        allowed = target.kind == core::TdM6MemberStatusKind::Member ||
                  target.kind == core::TdM6MemberStatusKind::Restricted;
        break;
    default:
        return M6CapabilityStatus::UnsupportedChatType;
    }
    return allowed ? M6CapabilityStatus::Allowed : M6CapabilityStatus::InvalidTarget;
}

bool m6_topic_mutation_allowed(proto::M6Operation operation, M6ChatKind kind,
                               const core::TdM6MemberStatus& caller, std::int64_t principal_id,
                               std::optional<std::int64_t> topic_creator_id) noexcept {
    if (!valid_m6_member_status(caller, kind) || kind == K::Channel || kind == K::BasicGroup) {
        return false;
    }
    if (operation == proto::M6Operation::TopicCreate) {
        return (caller.kind == core::TdM6MemberStatusKind::Member &&
                caller.permissions.can_create_topics) ||
               caller_has(caller, kind, R::ManageTopics);
    }
    if (operation != proto::M6Operation::TopicEdit && operation != proto::M6Operation::TopicClose &&
        operation != proto::M6Operation::TopicReopen) {
        return false;
    }
    return caller_has(caller, kind, R::ManageTopics) ||
           (topic_creator_id && *topic_creator_id == principal_id);
}

} // namespace tgcli::daemon
