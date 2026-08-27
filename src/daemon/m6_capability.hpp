#pragma once

#include "core/m6_td.hpp"
#include "daemon/m6_domain.hpp"
#include "proto/operation.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace tgcli::daemon {

enum class M6ChatKind { BasicGroup, ForumSupergroup, NonForumSupergroup, Channel };
enum class M6CapabilityStatus {
    Allowed,
    InvalidSnapshot,
    UnsupportedChatType,
    MissingRight,
    InvalidTarget,
};

bool valid_m6_member_status(const core::TdM6MemberStatus& status, M6ChatKind kind) noexcept;
M6CapabilityStatus
m6_authorize_caller(proto::M6Operation operation, M6ChatKind kind,
                    const core::TdM6MemberStatus& caller,
                    std::span<const M6AdminRight> requested_rights = {}) noexcept;
M6CapabilityStatus m6_authorize_target(proto::M6Operation operation, std::int64_t principal_id,
                                       std::int64_t target_id,
                                       const core::TdM6MemberStatus& target) noexcept;
bool m6_topic_mutation_allowed(proto::M6Operation operation, M6ChatKind kind,
                               const core::TdM6MemberStatus& caller, std::int64_t principal_id,
                               std::optional<std::int64_t> topic_creator_id) noexcept;

} // namespace tgcli::daemon
