#include "daemon/dispatch.hpp"
#include "daemon/raw_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

namespace {

using tgcli::daemon::Tier;
using tgcli::daemon::raw::AdmissionTier;
using tgcli::daemon::raw::BodyPolicyDecision;
using tgcli::daemon::raw::Digest;
using tgcli::daemon::raw::Error;
using tgcli::daemon::raw::Failure;
using tgcli::daemon::raw::RawPrincipal;
using tgcli::daemon::raw::TypedFunction;

Failure failure(std::string input) {
    auto parsed = tgcli::daemon::raw::parse(std::move(input));
    REQUIRE(std::holds_alternative<Failure>(parsed));
    return std::get<Failure>(std::move(parsed));
}

TypedFunction success(std::string input) {
    auto parsed = tgcli::daemon::raw::parse(std::move(input));
    REQUIRE(std::holds_alternative<TypedFunction>(parsed));
    return std::get<TypedFunction>(std::move(parsed));
}

Digest digest(TypedFunction& function,
              std::string_view pin = "a17f87c4cff7b90b278d12b91ba0614383aaee82",
              std::string_view tier = "destructive") {
    auto result = function.request_digest(pin, tier);
    REQUIRE(std::holds_alternative<Digest>(result));
    return std::get<Digest>(std::move(result));
}

} // namespace

TEST_CASE("dormant raw parser retains one pinned native Function identity",
          "[raw][foundation][canonical]") {
    auto parsed = success(R"({"x":"AAEC/w==","@type":"testCallBytes"})");
    const void* identity = parsed.identity();
    CHECK(parsed.name() == "testCallBytes");
    CHECK(parsed.result_type() == "TestBytes");
    CHECK(parsed.native().get_id() == td::td_api::testCallBytes::ID);
    CHECK(static_cast<const td::td_api::testCallBytes&>(parsed.native()).x_ ==
          std::string("\0\1\2\xff", 4));
    CHECK(parsed.canonical() == R"({"@type":"testCallBytes","x":"AAEC/w=="})");
    const auto policy = tgcli::daemon::raw::policy_metadata(parsed);
    REQUIRE(policy);
    CHECK(policy->name == "testCallBytes");
    CHECK(policy->admission == AdmissionTier::Denied);
    CHECK(policy->body_validator == "deny");
    CHECK(policy->sensitive_input);
    CHECK(policy->sensitive_output);
    CHECK(policy->reviewed);
    const auto denied = tgcli::daemon::raw::evaluate_body_policy(parsed);
    CHECK(denied.decision == BodyPolicyDecision::Deny);
    CHECK_FALSE(denied.effective_tier);

    const auto request_hash = digest(parsed);
    CHECK(request_hash.bytes == parsed.canonical().size());
    CHECK(request_hash.sha256 ==
          "sha256:6674411e52ee43aaf148d97e4683579e63db74c2dad6b1a9fd76d6a90be4cc4d");
    CHECK(parsed.identity() == identity);
    CHECK(digest(parsed, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb").sha256 != request_hash.sha256);
    CHECK(digest(parsed, "a17f87c4cff7b90b278d12b91ba0614383aaee82", "write").sha256 !=
          request_hash.sha256);
    for (const auto& invalid :
         {parsed.request_digest("other-pin", "destructive"),
          parsed.request_digest("a17f87c4cff7b90b278d12b91ba0614383aaee82", "caller-tier")}) {
        REQUIRE(std::holds_alternative<Failure>(invalid));
        CHECK(std::get<Failure>(invalid).error == Error::InvalidPolicyMetadata);
    }
}

TEST_CASE("dormant raw policy owns tier principal and non-secret chat preflight",
          "[raw][foundation][policy][preflight]") {
    auto local = success(R"({"@type":"cleanFileName","file_name":"a/b"})");
    const auto local_policy = tgcli::daemon::raw::policy_metadata(local);
    REQUIRE(local_policy);
    CHECK(local_policy->principal == RawPrincipal::Both);
    CHECK(local_policy->admission == AdmissionTier::Read);
    CHECK(local_policy->body_validator == "none");
    const auto local_outcome = tgcli::daemon::raw::evaluate_body_policy(local);
    CHECK(local_outcome.decision == BodyPolicyDecision::Preserve);
    CHECK(local_outcome.effective_tier == Tier::Read);
    CHECK(local_outcome.preflight.non_secret_chat_count == 0);

    auto read = success(R"({"@type":"getMessage","chat_id":-1001,"message_id":7})");
    const auto read_policy = tgcli::daemon::raw::policy_metadata(read);
    REQUIRE(read_policy);
    CHECK(read_policy->principal == RawPrincipal::User);
    CHECK(read_policy->admission == AdmissionTier::Read);
    CHECK(read_policy->body_validator == "chat_targets");
    const auto read_outcome = tgcli::daemon::raw::evaluate_body_policy(read);
    CHECK(read_outcome.decision == BodyPolicyDecision::Preserve);
    CHECK(read_outcome.effective_tier == Tier::Read);
    REQUIRE(read_outcome.preflight.non_secret_chat_count == 1);
    CHECK(read_outcome.preflight.non_secret_chat_ids[0] == -1001);

    auto member_chat = success(
        R"({"@type":"getChatMember","chat_id":-1001,"member_id":{"@type":"messageSenderChat","chat_id":-2002}})");
    const auto member_chat_outcome = tgcli::daemon::raw::evaluate_body_policy(member_chat);
    CHECK(member_chat_outcome.effective_tier == Tier::Read);
    REQUIRE(member_chat_outcome.preflight.non_secret_chat_count == 2);
    CHECK(member_chat_outcome.preflight.non_secret_chat_ids[0] == -1001);
    CHECK(member_chat_outcome.preflight.non_secret_chat_ids[1] == -2002);

    auto member_user = success(
        R"({"@type":"getChatMember","chat_id":-1001,"member_id":{"@type":"messageSenderUser","user_id":7}})");
    const auto member_user_outcome = tgcli::daemon::raw::evaluate_body_policy(member_user);
    CHECK(member_user_outcome.effective_tier == Tier::Read);
    REQUIRE(member_user_outcome.preflight.non_secret_chat_count == 1);
    CHECK(member_user_outcome.preflight.non_secret_chat_ids[0] == -1001);

    auto missing_member = success(R"({"@type":"getChatMember","chat_id":-1001})");
    CHECK_FALSE(tgcli::daemon::raw::evaluate_body_policy(missing_member).effective_tier);

    auto optional_sender = success(
        R"({"@type":"searchChatMessages","chat_id":-1001,"sender_id":{"@type":"messageSenderChat","chat_id":-2002}})");
    const auto optional_sender_outcome = tgcli::daemon::raw::evaluate_body_policy(optional_sender);
    CHECK(optional_sender_outcome.effective_tier == Tier::Read);
    REQUIRE(optional_sender_outcome.preflight.non_secret_chat_count == 2);
    CHECK(optional_sender_outcome.preflight.non_secret_chat_ids[0] == -1001);
    CHECK(optional_sender_outcome.preflight.non_secret_chat_ids[1] == -2002);

    auto missing_optional_sender = success(R"({"@type":"searchChatMessages","chat_id":-1001})");
    const auto missing_optional_outcome =
        tgcli::daemon::raw::evaluate_body_policy(missing_optional_sender);
    CHECK(missing_optional_outcome.effective_tier == Tier::Read);
    REQUIRE(missing_optional_outcome.preflight.non_secret_chat_count == 1);
    CHECK(missing_optional_outcome.preflight.non_secret_chat_ids[0] == -1001);

    auto invalid_target = success(R"({"@type":"getMessage","chat_id":0,"message_id":7})");
    CHECK_FALSE(tgcli::daemon::raw::evaluate_body_policy(invalid_target).effective_tier);

    auto destructive =
        success(R"({"@type":"deleteMessages","chat_id":-1001,"message_ids":[7],"revoke":false})");
    const auto destructive_policy = tgcli::daemon::raw::policy_metadata(destructive);
    REQUIRE(destructive_policy);
    CHECK(destructive_policy->admission == AdmissionTier::Destructive);
    const auto destructive_outcome = tgcli::daemon::raw::evaluate_body_policy(destructive);
    CHECK(destructive_outcome.effective_tier == Tier::Destructive);
    REQUIRE(destructive_outcome.preflight.non_secret_chat_count == 1);
    CHECK(destructive_outcome.preflight.non_secret_chat_ids[0] == -1001);

    auto destructive_member = success(
        R"({"@type":"setChatMemberStatus","chat_id":-1001,"member_id":{"@type":"messageSenderChat","chat_id":-2002},"status":{"@type":"chatMemberStatusLeft"}})");
    const auto destructive_member_outcome =
        tgcli::daemon::raw::evaluate_body_policy(destructive_member);
    CHECK(destructive_member_outcome.effective_tier == Tier::Destructive);
    REQUIRE(destructive_member_outcome.preflight.non_secret_chat_count == 2);
    CHECK(destructive_member_outcome.preflight.non_secret_chat_ids[0] == -1001);
    CHECK(destructive_member_outcome.preflight.non_secret_chat_ids[1] == -2002);

    auto circular = success(R"({"@type":"getChat","chat_id":-1001})");
    const auto circular_policy = tgcli::daemon::raw::policy_metadata(circular);
    REQUIRE(circular_policy);
    CHECK(circular_policy->admission == AdmissionTier::Denied);
    CHECK_FALSE(tgcli::daemon::raw::evaluate_body_policy(circular).effective_tier);
}

TEST_CASE("dormant raw body policy decisions preserve or raise static tiers",
          "[raw][foundation][policy][tier]") {
    using Case = std::tuple<AdmissionTier, BodyPolicyDecision, std::optional<Tier>>;
    const std::array cases{
        Case{AdmissionTier::Denied, BodyPolicyDecision::Deny, std::nullopt},
        Case{AdmissionTier::Denied, BodyPolicyDecision::Preserve, std::nullopt},
        Case{AdmissionTier::Denied, BodyPolicyDecision::RaiseWrite, std::nullopt},
        Case{AdmissionTier::Denied, BodyPolicyDecision::RaiseDestructive, std::nullopt},
        Case{AdmissionTier::Read, BodyPolicyDecision::Deny, std::nullopt},
        Case{AdmissionTier::Read, BodyPolicyDecision::Preserve, Tier::Read},
        Case{AdmissionTier::Read, BodyPolicyDecision::RaiseWrite, Tier::Write},
        Case{AdmissionTier::Read, BodyPolicyDecision::RaiseDestructive, Tier::Destructive},
        Case{AdmissionTier::Write, BodyPolicyDecision::Deny, std::nullopt},
        Case{AdmissionTier::Write, BodyPolicyDecision::Preserve, Tier::Write},
        Case{AdmissionTier::Write, BodyPolicyDecision::RaiseWrite, Tier::Write},
        Case{AdmissionTier::Write, BodyPolicyDecision::RaiseDestructive, Tier::Destructive},
        Case{AdmissionTier::Destructive, BodyPolicyDecision::Deny, std::nullopt},
        Case{AdmissionTier::Destructive, BodyPolicyDecision::Preserve, Tier::Destructive},
        Case{AdmissionTier::Destructive, BodyPolicyDecision::RaiseWrite, Tier::Destructive},
        Case{AdmissionTier::Destructive, BodyPolicyDecision::RaiseDestructive, Tier::Destructive},
    };
    for (const auto& [static_tier, decision, expected] : cases) {
        const auto outcome = tgcli::daemon::raw::apply_body_policy_decision(static_tier, decision);
        CHECK(outcome.decision == (expected ? decision : BodyPolicyDecision::Deny));
        CHECK(outcome.effective_tier == expected);
    }

    const auto invalid = tgcli::daemon::raw::apply_body_policy_decision(
        static_cast<AdmissionTier>(-1), BodyPolicyDecision::Preserve);
    CHECK(invalid.decision == BodyPolicyDecision::Deny);
    CHECK_FALSE(invalid.effective_tier);
    const auto invalid_decision = tgcli::daemon::raw::apply_body_policy_decision(
        AdmissionTier::Read, static_cast<BodyPolicyDecision>(-1));
    CHECK(invalid_decision.decision == BodyPolicyDecision::Deny);
    CHECK_FALSE(invalid_decision.effective_tier);
}

TEST_CASE("dormant raw graph rejects duplicate unknown reserved and malformed input",
          "[raw][foundation][parse]") {
    CHECK(failure(R"({"@type":"testCallEmpty","@type":"testCallEmpty"})").error ==
          Error::DuplicateField);
    CHECK(failure(R"({"@type":"testCallEmpty","unknown":1})").error == Error::UnknownField);
    CHECK(failure(R"({"@type":"testCallEmpty","@extra":1})").error == Error::UnknownField);
    CHECK(failure(R"({"@type":"testCallEmpty","@client_id":1})").error == Error::UnknownField);
    CHECK(failure("[]").error == Error::InvalidTopLevel);
    CHECK(failure(R"({"x":1})").error == Error::MissingType);
    CHECK(failure(R"({"@type":"notPinned"})").error == Error::UnexpectedType);
    CHECK(failure(R"({"@type":"testSquareInt","x":1,})").error == Error::InvalidJson);

    CHECK(
        failure(
            R"({"@type":"setOption","name":"x","value":{"@type":"optionValueBoolean","value":true,"value":false}})")
            .error == Error::DuplicateField);
    CHECK(
        failure(
            R"({"@type":"setOption","name":"x","value":{"@type":"optionValueBoolean","other":true}})")
            .error == Error::UnknownField);
    CHECK(
        failure(
            R"({"@type":"setOption","name":"x","value":{"@type":"optionValueString","value":"x","@extra":1}})")
            .error == Error::UnknownField);
}

TEST_CASE("dormant raw graph materializes TD defaults concrete and abstract variants",
          "[raw][foundation][canonical][defaults]") {
    auto missing = success(R"({"@type":"testSquareInt"})");
    auto explicit_null = success(R"({"@type":"testSquareInt","x":null})");
    auto explicit_zero = success(R"({"@type":"testSquareInt","x":0})");
    CHECK(missing.canonical() == R"({"@type":"testSquareInt","x":0})");
    CHECK(explicit_null.canonical() == missing.canonical());
    CHECK(explicit_zero.canonical() == missing.canonical());
    CHECK(digest(explicit_null).sha256 == digest(missing).sha256);

    auto nested = success(
        R"({"@type":"setOption","name":"fixture","value":{"@type":"optionValueInteger","value":9223372036854775807}})");
    auto nested_string = success(
        R"({"value":{"value":"9223372036854775807","@type":"optionValueInteger"},"name":"fixture","@type":"setOption"})");
    CHECK(
        nested.canonical() ==
        R"({"@type":"setOption","name":"fixture","value":{"@type":"optionValueInteger","value":"9223372036854775807"}})");
    CHECK(nested_string.canonical() == nested.canonical());
    CHECK(digest(nested_string).sha256 == digest(nested).sha256);

    auto concrete = success(
        R"({"@type":"testCallVectorIntObject","x":[{"value":1},{"@type":"testInt","value":2}]})");
    CHECK(
        concrete.canonical() ==
        R"({"@type":"testCallVectorIntObject","x":[{"@type":"testInt","value":1},{"@type":"testInt","value":2}]})");
    CHECK(
        failure(
            R"({"@type":"setOption","name":"x","value":{"@type":"optionValueString","value":false}})")
            .error == Error::InvalidFieldType);
}

TEST_CASE("dormant raw generated types enforce numeric bytes depth and size bounds",
          "[raw][foundation][bounds]") {
    CHECK(failure(R"({"@type":"testSquareInt","x":-2147483649})").error == Error::InvalidInteger);
    CHECK(failure(R"({"@type":"getMessage","chat_id":9007199254740992})").error ==
          Error::InvalidInteger);
    CHECK(
        failure(
            R"({"@type":"setOption","value":{"@type":"optionValueInteger","value":"09223372036854775807"}})")
            .error == Error::InvalidInteger);
    CHECK(failure(R"({"@type":"testCallBytes","x":"AAE"})").error == Error::InvalidBase64);
    CHECK(failure(R"({"@type":"testCallBytes","x":"A==="})").error == Error::InvalidBase64);
    CHECK(failure(R"({"@type":"testProxy","timeout":"1e-7"})").error == Error::InvalidDouble);

    std::string nested = "0";
    for (std::size_t depth = 0; depth <= tgcli::daemon::raw::kMaximumJsonDepth; ++depth) {
        nested.insert(nested.begin(), '[');
        nested.push_back(']');
    }
    const auto nested_failure = failure(std::move(nested));
    CHECK(nested_failure.error == Error::MaximumDepthExceeded);
    CHECK(failure(std::string(tgcli::daemon::raw::kMaximumRequestBytes + 1, ' ')).error ==
          Error::InputTooLarge);

    std::string exact = R"({"@type":"testCallEmpty"})";
    exact.append(tgcli::daemon::raw::kMaximumRequestBytes - exact.size(), ' ');
    auto exact_result = tgcli::daemon::raw::parse(std::move(exact));
    CHECK(std::holds_alternative<TypedFunction>(exact_result));

    std::string expanded = R"({"@type":"testCallString","x":")";
    for (std::size_t index = 0; index < 180'000; ++index) {
        expanded += "\\n";
    }
    expanded += R"("})";
    REQUIRE(expanded.size() <= tgcli::daemon::raw::kMaximumRequestBytes);
    const auto expanded_failure = failure(std::move(expanded));
    CHECK(expanded_failure.error == Error::CanonicalTooLarge);
}

TEST_CASE("dormant raw canonical doubles follow RFC 8785 thresholds",
          "[raw][foundation][canonical][double]") {
    CHECK(success(R"({"@type":"testProxy","timeout":-0})")
              .canonical()
              .ends_with(R"("dc_id":0,"timeout":0})"));
    CHECK(success(R"({"@type":"testProxy","timeout":0.000001})")
              .canonical()
              .ends_with(R"("timeout":0.000001})"));
    CHECK(success(R"({"@type":"testProxy","timeout":100000000000000000000})")
              .canonical()
              .ends_with(R"("timeout":100000000000000000000})"));
    CHECK(success(R"({"@type":"testProxy","timeout":1e21})")
              .canonical()
              .ends_with(R"("timeout":1e+21})"));
    CHECK(success(R"({"@type":"testProxy","timeout":333333333.33333329})")
              .canonical()
              .ends_with(R"("timeout":333333333.3333333})"));
}

TEST_CASE("dormant raw response hashing validates the declared TD result type",
          "[raw][foundation][response]") {
    auto function = success(R"({"@type":"testCallString","x":""})");
    std::vector<std::string> wiped_stages;
    const auto observer = [&](std::string_view stage, const char* bytes, std::size_t size) {
        CHECK(std::string_view(bytes, size) == std::string(size, '\0'));
        wiped_stages.emplace_back(stage);
    };

    auto empty_response = td::td_api::make_object<td::td_api::testString>("x");
    auto empty_digest =
        tgcli::daemon::raw::response_digest(function, std::move(empty_response), observer);
    REQUIRE(std::holds_alternative<Digest>(empty_digest));
    CHECK(empty_response == nullptr);
    const auto overhead = std::get<Digest>(empty_digest).bytes;
    REQUIRE(overhead > 1);
    REQUIRE(overhead <= tgcli::daemon::raw::kMaximumResponseBytes);

    auto exact_response = td::td_api::make_object<td::td_api::testString>(
        std::string(tgcli::daemon::raw::kMaximumResponseBytes - overhead + 1, 'x'));
    auto exact = tgcli::daemon::raw::response_digest(function, std::move(exact_response), observer);
    REQUIRE(std::holds_alternative<Digest>(exact));
    CHECK(exact_response == nullptr);
    CHECK(std::get<Digest>(exact).bytes == tgcli::daemon::raw::kMaximumResponseBytes);

    auto oversized_response = td::td_api::make_object<td::td_api::testString>(
        std::string(tgcli::daemon::raw::kMaximumResponseBytes - overhead + 2, 'x'));
    auto oversized =
        tgcli::daemon::raw::response_digest(function, std::move(oversized_response), observer);
    REQUIRE(std::holds_alternative<Failure>(oversized));
    CHECK(oversized_response == nullptr);
    CHECK(std::get<Failure>(oversized).error == Error::CanonicalTooLarge);

    auto wrong_response = td::td_api::make_object<td::td_api::text>("secret mismatch");
    auto wrong = tgcli::daemon::raw::response_digest(function, std::move(wrong_response), observer);
    REQUIRE(std::holds_alternative<Failure>(wrong));
    CHECK(wrong_response == nullptr);
    CHECK(std::get<Failure>(wrong).error == Error::UnexpectedResponseType);

    auto null_response = td::td_api::object_ptr<td::td_api::Object>{};
    auto null_result =
        tgcli::daemon::raw::response_digest(function, std::move(null_response), observer);
    REQUIRE(std::holds_alternative<Failure>(null_result));
    CHECK(std::get<Failure>(null_result).error == Error::UnexpectedResponseType);

    auto error_response = td::td_api::make_object<td::td_api::error>(400, "secret error");
    auto td_error =
        tgcli::daemon::raw::response_digest(function, std::move(error_response), observer);
    CHECK(std::holds_alternative<Digest>(td_error));
    CHECK(error_response == nullptr);
    CHECK(std::ranges::count(wiped_stages, "raw_response_canonical") == 4);
    CHECK(std::ranges::count(wiped_stages, "raw_native_string_or_bytes") >= 5);
}

TEST_CASE("dormant raw owners wipe physical AST conversion and canonical buffers",
          "[raw][foundation][wipe]") {
    std::vector<std::string> stages;
    const auto observer = [&](std::string_view stage, const char* bytes, std::size_t size) {
        CHECK(std::string_view(bytes, size) == std::string(size, '\0'));
        stages.emplace_back(stage);
    };
    std::string physical = R"({"@type":"testCallBytes","x":"AAEC/w=="})";
    {
        auto parsed = tgcli::daemon::raw::parse(std::move(physical), observer);
        REQUIRE(std::holds_alternative<TypedFunction>(parsed));
    }
    auto function = success(R"({"@type":"testCallString","x":""})");
    auto response = td::td_api::make_object<td::td_api::testString>("sensitive response");
    const auto response_hash =
        tgcli::daemon::raw::response_digest(function, std::move(response), observer);
    REQUIRE(std::holds_alternative<Digest>(response_hash));
    CHECK(response == nullptr);
    // NOLINTNEXTLINE(bugprone-use-after-move): the rvalue contract clears the caller buffer.
    CHECK(physical.empty());
    CHECK(std::ranges::find(stages, "raw_physical_input") != stages.end());
    CHECK(std::ranges::find(stages, "raw_ast_string") != stages.end());
    CHECK(std::ranges::find(stages, "raw_ast_key") != stages.end());
    CHECK(std::ranges::find(stages, "raw_native_conversion_staging") != stages.end());
    CHECK(std::ranges::find(stages, "raw_native_canonical_proof") != stages.end());
    CHECK(std::ranges::find(stages, "raw_native_string_or_bytes") != stages.end());
    CHECK(std::ranges::find(stages, "raw_canonical") != stages.end());
    CHECK(std::ranges::find(stages, "raw_response_canonical") != stages.end());
}
