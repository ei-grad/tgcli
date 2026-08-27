#include "daemon/m6_capability.hpp"

#include <array>
#include <cstddef>
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

void enable_right(core::TdM6AdminRights& rights, std::size_t index) {
    switch (index) {
    case 0:
        rights.can_change_info = true;
        break;
    case 1:
        rights.can_post_messages = true;
        break;
    case 2:
        rights.can_edit_messages = true;
        break;
    case 3:
        rights.can_delete_messages = true;
        break;
    case 4:
        rights.can_invite_users = true;
        break;
    case 5:
        rights.can_restrict_members = true;
        break;
    case 6:
        rights.can_pin_messages = true;
        break;
    case 7:
        rights.can_manage_topics = true;
        break;
    case 8:
        rights.can_promote_members = true;
        break;
    case 9:
        rights.can_manage_video_chats = true;
        break;
    case 10:
        rights.can_post_stories = true;
        break;
    case 11:
        rights.can_edit_stories = true;
        break;
    case 12:
        rights.can_delete_stories = true;
        break;
    case 13:
        rights.can_manage_direct_messages = true;
        break;
    case 14:
        rights.can_manage_tags = true;
        break;
    case 15:
        rights.is_anonymous = true;
        break;
    default:
        break;
    }
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

TEST_CASE("M6 all native administrator rights are validated against every chat kind",
          "[m6][capability][matrix]") {
    constexpr std::array<std::array<bool, 16>, 4> applicable{{
        {{true, false, false, true, true, true, true, false, true, true, false, false, false, false,
          true, false}},
        {{true, false, false, true, true, true, true, true, true, true, true, true, true, false,
          true, true}},
        {{true, false, false, true, true, true, true, false, true, true, true, true, true, false,
          true, true}},
        {{true, true, true, true, true, true, false, false, true, true, true, true, true, true,
          false, false}},
    }};
    for (std::size_t kind_index = 0; kind_index < applicable.size(); ++kind_index) {
        const auto kind = static_cast<daemon::M6ChatKind>(kind_index);
        const auto& applicable_rights = applicable.at(kind_index);
        for (std::size_t right = 0; right < applicable_rights.size(); ++right) {
            CAPTURE(kind_index, right);
            core::TdM6MemberStatus status;
            status.kind = core::TdM6MemberStatusKind::Administrator;
            status.rights.can_manage_chat = true;
            enable_right(status.rights, right);
            CHECK(daemon::valid_m6_member_status(status, kind) == applicable_rights.at(right));
        }
        core::TdM6MemberStatus manager;
        manager.kind = core::TdM6MemberStatusKind::Administrator;
        manager.rights.can_manage_chat = true;
        CHECK(daemon::valid_m6_member_status(manager, kind));
        manager.rights.can_manage_chat = false;
        manager.rights.can_change_info = true;
        CHECK_FALSE(daemon::valid_m6_member_status(manager, kind));
    }
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
