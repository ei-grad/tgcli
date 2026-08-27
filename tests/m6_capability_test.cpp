#include "daemon/m6_capability.hpp"

#include <array>
#include <optional>

#include <catch2/catch_test_macros.hpp>

namespace {

namespace core = tgcli::core;
namespace daemon = tgcli::daemon;
namespace proto = tgcli::proto;

core::TdM6MemberStatus creator() {
    core::TdM6MemberStatus status;
    status.kind = core::TdM6MemberStatusKind::Creator;
    status.is_member = true;
    return status;
}

core::TdM6MemberStatus administrator() {
    core::TdM6MemberStatus status;
    status.kind = core::TdM6MemberStatusKind::Administrator;
    status.can_be_edited = true;
    status.rights.can_manage_chat = true;
    status.rights.can_change_info = true;
    status.rights.can_invite_users = true;
    status.rights.can_restrict_members = true;
    status.rights.can_promote_members = true;
    return status;
}

core::TdM6MemberStatus member() {
    core::TdM6MemberStatus status;
    status.kind = core::TdM6MemberStatusKind::Member;
    status.member_until_date = 0;
    return status;
}

TEST_CASE("M6 administrator snapshots enforce the complete chat-kind applicability matrix",
          "[m6][capability]") {
    auto status = administrator();
    status.rights.can_post_messages = true;
    CHECK_FALSE(daemon::valid_m6_member_status(status, daemon::M6ChatKind::BasicGroup));
    CHECK_FALSE(daemon::valid_m6_member_status(status, daemon::M6ChatKind::ForumSupergroup));
    CHECK(daemon::valid_m6_member_status(status, daemon::M6ChatKind::Channel));

    status = administrator();
    status.rights.can_manage_topics = true;
    CHECK(daemon::valid_m6_member_status(status, daemon::M6ChatKind::ForumSupergroup));
    CHECK_FALSE(daemon::valid_m6_member_status(status, daemon::M6ChatKind::NonForumSupergroup));
    CHECK_FALSE(daemon::valid_m6_member_status(status, daemon::M6ChatKind::Channel));

    status = administrator();
    status.rights.can_manage_chat = false;
    CHECK_FALSE(daemon::valid_m6_member_status(status, daemon::M6ChatKind::BasicGroup));
}

TEST_CASE("M6 caller authority is explicit for creator administrator and member",
          "[m6][capability]") {
    CHECK(daemon::m6_authorize_caller(proto::M6Operation::ChatSetTitle, daemon::M6ChatKind::Channel,
                                      creator()) == daemon::M6CapabilityStatus::Allowed);
    CHECK(daemon::m6_authorize_caller(proto::M6Operation::ChatInviteLink,
                                      daemon::M6ChatKind::ForumSupergroup,
                                      administrator()) == daemon::M6CapabilityStatus::Allowed);

    auto missing = administrator();
    missing.rights.can_invite_users = false;
    CHECK(daemon::m6_authorize_caller(proto::M6Operation::ChatInviteLink,
                                      daemon::M6ChatKind::ForumSupergroup,
                                      missing) == daemon::M6CapabilityStatus::MissingRight);

    auto permitted_member = member();
    permitted_member.permissions.can_change_info = true;
    CHECK(daemon::m6_authorize_caller(proto::M6Operation::ChatSetDescription,
                                      daemon::M6ChatKind::NonForumSupergroup,
                                      permitted_member) == daemon::M6CapabilityStatus::Allowed);
    CHECK(daemon::m6_authorize_caller(proto::M6Operation::ChatSetDescription,
                                      daemon::M6ChatKind::Channel, permitted_member) ==
          daemon::M6CapabilityStatus::MissingRight);

    const std::array requested{daemon::M6AdminRight::ChangeInfo};
    CHECK(daemon::m6_authorize_caller(proto::M6Operation::ChatPromote,
                                      daemon::M6ChatKind::BasicGroup, creator(), requested) ==
          daemon::M6CapabilityStatus::UnsupportedChatType);
}

TEST_CASE("M6 target transitions reject self creator and invalid starting states",
          "[m6][capability]") {
    const auto regular = member();
    CHECK(daemon::m6_authorize_target(proto::M6Operation::ChatBan, 10, 20, regular) ==
          daemon::M6CapabilityStatus::Allowed);

    core::TdM6MemberStatus banned;
    banned.kind = core::TdM6MemberStatusKind::Banned;
    CHECK(daemon::m6_authorize_target(proto::M6Operation::ChatUnban, 10, 20, banned) ==
          daemon::M6CapabilityStatus::Allowed);
    CHECK(daemon::m6_authorize_target(proto::M6Operation::ChatPromote, 10, 20, banned) ==
          daemon::M6CapabilityStatus::InvalidTarget);
    CHECK(daemon::m6_authorize_target(proto::M6Operation::ChatBan, 10, 10, regular) ==
          daemon::M6CapabilityStatus::InvalidTarget);
    CHECK(daemon::m6_authorize_target(proto::M6Operation::ChatBan, 10, 20, creator()) ==
          daemon::M6CapabilityStatus::InvalidTarget);
}

TEST_CASE("M6 topic mutations use manage-topics or exact creator ownership", "[m6][capability]") {
    auto admin = administrator();
    admin.rights.can_manage_topics = true;
    CHECK(daemon::m6_topic_mutation_allowed(proto::M6Operation::TopicClose,
                                            daemon::M6ChatKind::ForumSupergroup, admin, 10,
                                            std::nullopt));

    auto regular = member();
    regular.permissions.can_create_topics = true;
    CHECK(daemon::m6_topic_mutation_allowed(proto::M6Operation::TopicCreate,
                                            daemon::M6ChatKind::ForumSupergroup, regular, 10,
                                            std::nullopt));
    CHECK(daemon::m6_topic_mutation_allowed(proto::M6Operation::TopicEdit,
                                            daemon::M6ChatKind::ForumSupergroup, regular, 10, 10));
    CHECK_FALSE(daemon::m6_topic_mutation_allowed(
        proto::M6Operation::TopicEdit, daemon::M6ChatKind::ForumSupergroup, regular, 10, 20));
    CHECK_FALSE(daemon::m6_topic_mutation_allowed(
        proto::M6Operation::TopicCreate, daemon::M6ChatKind::Channel, regular, 10, std::nullopt));
}

} // namespace
