#include "daemon/m6_write_policy.hpp"

#include <set>

#include <catch2/catch_test_macros.hpp>

namespace {

namespace daemon = tgcli::daemon;
namespace proto = tgcli::proto;

TEST_CASE("M6 durable policy covers exactly all 24 mutations", "[m6][write-policy]") {
    const auto policies = daemon::m6_write_policies();
    REQUIRE(policies.size() == 24);
    std::set<proto::M6Operation> operations;
    std::set<std::string_view> names;
    std::size_t idempotent = 0;
    std::size_t photo_spool = 0;
    for (const auto& policy : policies) {
        const auto* identity = proto::m6_operation_identity(policy.operation);
        REQUIRE(identity != nullptr);
        CHECK(identity->mutation);
        CHECK(identity->canonical_name == policy.audit_name);
        CHECK(operations.insert(policy.operation).second);
        CHECK(names.insert(policy.audit_name).second);
        CHECK(daemon::parse_m6_write_operation(policy.audit_name) == policy.operation);
        CHECK(daemon::m6_write_policy(policy.operation) == &policy);
        REQUIRE(policy.tdlib_function_count >= 1);
        REQUIRE(policy.tdlib_function_count <= policy.tdlib_functions.size());
        for (std::size_t index = 0; index < policy.tdlib_function_count; ++index) {
            CHECK(daemon::valid_m6_tdlib_function(policy.operation, policy.tdlib_functions[index]));
        }
        idempotent += policy.idempotent;
        photo_spool += policy.uses_photo_spool;
    }
    CHECK(idempotent == 22);
    CHECK(photo_spool == 1);
    CHECK_FALSE(daemon::m6_write_policy(proto::M6Operation::ContactList));
    CHECK_FALSE(daemon::parse_m6_write_operation("contact_list"));
}

TEST_CASE("M6 exceptional durable policies are closed", "[m6][write-policy]") {
    const auto* invite = daemon::m6_write_policy(proto::M6Operation::ChatInviteLink);
    REQUIRE(invite != nullptr);
    CHECK_FALSE(invite->idempotent);
    CHECK(invite->tdlib_function_count == 2);
    CHECK(daemon::valid_m6_tdlib_function(proto::M6Operation::ChatInviteLink,
                                          "createChatInviteLink"));
    CHECK(daemon::valid_m6_tdlib_function(proto::M6Operation::ChatInviteLink,
                                          "revokeChatInviteLink"));

    const auto* optimize = daemon::m6_write_policy(proto::M6Operation::StorageOptimize);
    REQUIRE(optimize != nullptr);
    CHECK_FALSE(optimize->idempotent);
    CHECK_FALSE(daemon::valid_m6_tdlib_function(proto::M6Operation::StorageOptimize,
                                                "getStorageStatistics"));

    const auto* photo = daemon::m6_write_policy(proto::M6Operation::ChatSetPhoto);
    REQUIRE(photo != nullptr);
    CHECK(photo->uses_photo_spool);
}

} // namespace
