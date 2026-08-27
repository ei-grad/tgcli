#include "daemon/m6_domain.hpp"
#include "proto/operation.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;

TEST_CASE("M6 long-tail operation policy is closed and complete", "[m6][foundation]") {
    const auto identities = proto::m6_operation_identities();
    REQUIRE(identities.size() == 30);

    std::set<proto::M6Operation> operations;
    std::set<std::string_view> canonical_names;
    std::set<std::string_view> command_paths;
    std::size_t reads = 0;
    std::size_t writes = 0;
    std::size_t destructive = 0;
    std::size_t mutations = 0;
    std::size_t idempotent = 0;

    for (const auto& identity : identities) {
        CHECK(operations.insert(identity.operation).second);
        CHECK(canonical_names.insert(identity.canonical_name).second);
        CHECK(command_paths.insert(identity.command_path).second);
        CHECK(proto::parse_m6_operation(identity.canonical_name) == identity.operation);
        CHECK(proto::m6_operation_for_command(identity.command_path) == identity.operation);
        CHECK(proto::m6_operation_for_command(std::span<const std::string>{}) == std::nullopt);
        switch (identity.tier) {
        case proto::M6Tier::Read:
            ++reads;
            CHECK_FALSE(identity.mutation);
            CHECK_FALSE(identity.idempotent);
            break;
        case proto::M6Tier::Write:
            ++writes;
            CHECK(identity.mutation);
            break;
        case proto::M6Tier::Destructive:
            ++destructive;
            CHECK(identity.mutation);
            break;
        }
        mutations += identity.mutation ? 1U : 0U;
        idempotent += identity.idempotent ? 1U : 0U;
    }

    CHECK(reads == 6);
    CHECK(writes == 19);
    CHECK(destructive == 5);
    CHECK(mutations == 24);
    CHECK(idempotent == 22);
    CHECK_FALSE(proto::m6_operation_identity(proto::M6Operation::ChatInviteLink)->idempotent);
    CHECK_FALSE(proto::m6_operation_identity(proto::M6Operation::StorageOptimize)->idempotent);
    CHECK(proto::m6_operation_identity(proto::M6Operation::FolderDelete)->idempotent);
    CHECK(proto::parse_m6_operation("session_terminate") == std::nullopt);
    CHECK(proto::m6_operation_for_command("raw contact list") == std::nullopt);
}

TEST_CASE("M6 command token matching rejects prefixes suffixes and aliases", "[m6][foundation]") {
    const std::vector<std::string> exact{"folder", "add-chat"};
    const std::vector<std::string> prefix{"folder"};
    const std::vector<std::string> suffix{"folder", "add-chat", "extra"};
    const std::vector<std::string> alias{"folders", "add-chat"};

    CHECK(proto::m6_operation_for_command(exact) == proto::M6Operation::FolderAddChat);
    CHECK(proto::m6_operation_for_command(prefix) == std::nullopt);
    CHECK(proto::m6_operation_for_command(suffix) == std::nullopt);
    CHECK(proto::m6_operation_for_command(alias) == std::nullopt);
}

TEST_CASE("M6 closed enum spellings round-trip without aliases", "[m6][foundation]") {
    CHECK(daemon::m6_folder_icon_names().size() == 30);
    CHECK(daemon::m6_topic_color_names().size() == 6);
    CHECK(daemon::m6_admin_right_names().size() == 16);
    CHECK(daemon::m6_chat_permission_names().size() == 16);

    for (const auto name : daemon::m6_folder_icon_names()) {
        const auto parsed = daemon::parse_m6_folder_icon(name);
        REQUIRE(parsed);
        CHECK(daemon::m6_folder_icon_name(*parsed) == name);
    }
    for (const auto name : daemon::m6_topic_color_names()) {
        const auto parsed = daemon::parse_m6_topic_color(name);
        REQUIRE(parsed);
        CHECK(daemon::m6_topic_color_name(*parsed) == name);
    }
    for (const auto name : daemon::m6_admin_right_names()) {
        const auto parsed = daemon::parse_m6_admin_right(name);
        REQUIRE(parsed);
        CHECK(daemon::m6_admin_right_name(*parsed) == name);
    }
    for (const auto name : daemon::m6_chat_permission_names()) {
        const auto parsed = daemon::parse_m6_chat_permission(name);
        REQUIRE(parsed);
        CHECK(daemon::m6_chat_permission_name(*parsed) == name);
    }
    CHECK_FALSE(daemon::parse_m6_folder_icon("All"));
    CHECK_FALSE(daemon::parse_m6_topic_color("BLUE"));
    CHECK_FALSE(daemon::parse_m6_admin_right("can_manage_chat"));
    CHECK_FALSE(daemon::parse_m6_chat_permission("none"));
}

TEST_CASE("M6 canonical caller strings reject every pinned rewrite", "[m6][foundation]") {
    using daemon::M6TextKind;
    CHECK(daemon::valid_m6_canonical_text(M6TextKind::FolderName, "Project"));
    CHECK(daemon::valid_m6_canonical_text(M6TextKind::TopicName, "Release notes"));
    CHECK(daemon::valid_m6_canonical_text(M6TextKind::ChatTitle, "Release notes"));
    CHECK(daemon::valid_m6_canonical_text(M6TextKind::ChatDescription, "a  b\nc"));
    CHECK(daemon::valid_m6_canonical_text(M6TextKind::ChatDescription, ""));

    for (const auto& value : {std::string(" leading"), std::string("trailing "),
                              std::string("repeated  space"), std::string("line\nbreak"),
                              std::string("non\xC2\xA0"
                                          "breaking"),
                              std::string("unicode\xE2\x80\xA8separator")}) {
        CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::FolderName, value));
        CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::TopicName, value));
        CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::ChatTitle, value));
    }
    CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::ChatDescription, " leading"));
    CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::ChatDescription, "trailing "));
    CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::ChatDescription, "non\xC2\xA0"
                                                                             "breaking"));
    CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::ChatDescription,
                                                "unicode\xE2\x80\xA8separator"));
    CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::FolderName, std::string(13, 'x')));
    CHECK_FALSE(daemon::valid_m6_canonical_text(M6TextKind::TopicName, std::string(129, 'x')));
    CHECK_FALSE(
        daemon::valid_m6_canonical_text(M6TextKind::ChatDescription, std::string(256, 'x')));
}

TEST_CASE("M6 selectors and scalar fields use exact local grammar", "[m6][foundation]") {
    CHECK(daemon::valid_m6_exact_selector("-100123"));
    CHECK(daemon::valid_m6_exact_selector("@project"));
    CHECK(daemon::valid_m6_exact_selector("https://t.me/project"));
    CHECK_FALSE(daemon::valid_m6_exact_selector("Project title"));
    CHECK_FALSE(daemon::valid_m6_exact_selector("https://t.me/+invite"));
    CHECK_FALSE(daemon::valid_m6_exact_selector("https://example.com/project"));

    CHECK(daemon::parse_m6_positive_int32("1") == 1);
    CHECK(daemon::parse_m6_positive_int32("2147483647") == 2147483647);
    CHECK_FALSE(daemon::parse_m6_positive_int32("0"));
    CHECK_FALSE(daemon::parse_m6_positive_int32("01"));
    CHECK_FALSE(daemon::parse_m6_positive_int32("+1"));
    CHECK_FALSE(daemon::parse_m6_positive_int32("2147483648"));
    CHECK(daemon::valid_m6_contact_query("x"));
    CHECK_FALSE(daemon::valid_m6_contact_query(""));
    CHECK_FALSE(daemon::valid_m6_contact_query(std::string(257, 'x')));
    CHECK(daemon::valid_m6_invite_link("https://t.me/+abcdef"));
    CHECK_FALSE(daemon::valid_m6_invite_link("bad\nlink"));
}
