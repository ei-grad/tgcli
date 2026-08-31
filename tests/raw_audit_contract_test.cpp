#include "daemon/raw_audit_contract.hpp"

#include <array>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
using tgcli::daemon::raw::audit_v3::RecoveryAction;

constexpr std::string_view kInvocation = "00112233445566778899aabbccddeeff";
constexpr std::string_view kToken = "ffeeddccbbaa99887766554433221100";
constexpr std::string_view kHash =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

json intent() {
    return {
        {"schema_version", 3},          {"record_type", "raw_intent"},
        {"invocation_id", kInvocation}, {"function", "deleteMessages"},
        {"tier", "destructive"},        {"tdlib_sha", "a17f87c4cff7b90b278d12b91ba0614383aaee82"},
        {"request_sha256", kHash},      {"request_bytes", 128}};
}

json dispatch() {
    return {{"schema_version", 3},
            {"record_type", "raw_checkpoint"},
            {"invocation_id", kInvocation},
            {"stage", "raw_dispatch_started"},
            {"data", {{"dispatch_token", kToken}, {"generation", "7"}}}};
}

json response(std::string_view kind = "result") {
    return {{"schema_version", 3},
            {"record_type", "raw_checkpoint"},
            {"invocation_id", kInvocation},
            {"stage", "raw_response_received"},
            {"data",
             {{"dispatch_token", kToken},
              {"generation", "7"},
              {"kind", kind},
              {"response_type", kind == "result" ? "ok" : "error"},
              {"td_error_code", kind == "result" ? json(nullptr) : json(400)},
              {"response_sha256", kHash},
              {"response_bytes", 32}}}};
}

json malformed_response(std::optional<std::string_view> type = std::string_view{"updateNewChat"},
                        bool digest = true) {
    return {{"schema_version", 3},
            {"record_type", "raw_checkpoint"},
            {"invocation_id", kInvocation},
            {"stage", "raw_response_received"},
            {"data",
             {{"dispatch_token", kToken},
              {"generation", "7"},
              {"kind", "malformed"},
              {"response_type", type ? json(*type) : json(nullptr)},
              {"td_error_code", nullptr},
              {"response_sha256", digest ? json(kHash) : json(nullptr)},
              {"response_bytes", digest ? json(32) : json(nullptr)}}}};
}

json oversized_response() {
    auto value = malformed_response("ok", false);
    value["data"]["kind"] = "result_too_large";
    return value;
}

json result_outcome() {
    return {{"schema_version", 3},
            {"record_type", "raw_outcome"},
            {"invocation_id", kInvocation},
            {"mutation_state", "confirmed"},
            {"terminal",
             {{"kind", "result_digest"},
              {"response_type", "ok"},
              {"response_sha256", kHash},
              {"response_bytes", 32}}}};
}

json error_outcome() {
    return {
        {"schema_version", 3},
        {"record_type", "raw_outcome"},
        {"invocation_id", kInvocation},
        {"mutation_state", "possible"},
        {"terminal", {{"kind", "error_summary"}, {"code", "TDLIB_ERROR"}, {"td_error_code", 400}}}};
}

json unconfirmed_outcome() {
    auto value = error_outcome();
    value["terminal"] = {
        {"kind", "error_summary"}, {"code", "RAW_OUTCOME_UNCONFIRMED"}, {"td_error_code", nullptr}};
    return value;
}

json malformed_outcome() {
    auto value = error_outcome();
    value["terminal"] = {{"kind", "error_summary"},
                         {"code", "INTERNAL"},
                         {"reason", "unexpected_response"},
                         {"td_error_code", nullptr}};
    return value;
}

json oversized_outcome() {
    auto value = malformed_outcome();
    value["terminal"]["reason"] = "result_too_large";
    return value;
}

json none_outcome() {
    return {{"schema_version", 3},
            {"record_type", "raw_outcome"},
            {"invocation_id", kInvocation},
            {"mutation_state", "none"},
            {"terminal", nullptr}};
}

RecoveryAction action(std::initializer_list<json> records) {
    const std::vector<json> journal(records);
    const auto scanned = tgcli::daemon::raw::audit_v3::scan(journal);
    REQUIRE(scanned.decisions.size() == 1);
    return scanned.decisions.front().action;
}

} // namespace

TEST_CASE("raw audit v3 validates hash-only records and rejects bodies",
          "[raw][audit-v3][schema]") {
    CHECK(tgcli::daemon::raw::audit_v3::valid_intent(intent()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_checkpoint(dispatch()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_checkpoint(response()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_checkpoint(response("error")));
    CHECK(tgcli::daemon::raw::audit_v3::valid_checkpoint(malformed_response()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_checkpoint(malformed_response(std::nullopt, false)));
    CHECK(tgcli::daemon::raw::audit_v3::valid_checkpoint(oversized_response()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_outcome(result_outcome()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_outcome(error_outcome()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_outcome(unconfirmed_outcome()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_outcome(none_outcome()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_outcome(malformed_outcome()));
    CHECK(tgcli::daemon::raw::audit_v3::valid_outcome(oversized_outcome()));

    auto request_body = intent();
    request_body["request"] = {{"@type", "deleteMessages"}};
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_intent(request_body));
    auto response_body = response();
    response_body["data"]["response"] = {{"@type", "ok"}};
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(response_body));
    auto message = error_outcome();
    message["terminal"]["message"] = "upstream secret";
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_outcome(message));
    auto error_result = result_outcome();
    error_result["terminal"]["response_type"] = "error";
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_outcome(error_result));
}

TEST_CASE("raw audit v3 maps every durable crash cut without resend", "[raw][audit-v3][recovery]") {
    CHECK(action({intent()}) == RecoveryAction::NoMutation);
    CHECK(action({intent(), none_outcome()}) == RecoveryAction::Complete);
    CHECK(action({intent(), dispatch()}) == RecoveryAction::EmitUnconfirmed);
    CHECK(action({intent(), dispatch(), response()}) == RecoveryAction::RepairConfirmedResult);
    CHECK(action({intent(), dispatch(), response("error")}) == RecoveryAction::RepairPossibleError);
    CHECK(action({intent(), dispatch(), malformed_response()}) ==
          RecoveryAction::RepairPossibleInternal);
    CHECK(action({intent(), dispatch(), oversized_response()}) ==
          RecoveryAction::RepairPossibleTooLarge);
    CHECK(action({intent(), dispatch(), response(), result_outcome()}) == RecoveryAction::Complete);
    CHECK(action({intent(), dispatch(), response("error"), error_outcome()}) ==
          RecoveryAction::Complete);
    CHECK(action({intent(), dispatch(), unconfirmed_outcome()}) == RecoveryAction::Complete);
    CHECK(action({intent(), dispatch(), malformed_response(), malformed_outcome()}) ==
          RecoveryAction::Complete);
    CHECK(action({intent(), dispatch(), oversized_response(), oversized_outcome()}) ==
          RecoveryAction::Complete);
}

TEST_CASE("raw audit v3 fails closed on ordering identity and terminal contradictions",
          "[raw][audit-v3][scanner]") {
    for (const auto& journal : std::array{
             std::vector<json>{dispatch(), intent()},
             std::vector<json>{intent(), intent()},
             std::vector<json>{intent(), response()},
             std::vector<json>{intent(), dispatch(), dispatch()},
             std::vector<json>{intent(), dispatch(), response(), response()},
         }) {
        const auto scanned = tgcli::daemon::raw::audit_v3::scan(journal);
        CHECK_FALSE(scanned.valid);
        REQUIRE(scanned.decisions.size() == 1);
        CHECK(scanned.decisions.front().action == RecoveryAction::FailClosed);
    }

    auto wrong_token = response();
    wrong_token["data"]["dispatch_token"] = "11112222333344445555666677778888";
    CHECK(action({intent(), dispatch(), wrong_token}) == RecoveryAction::FailClosed);

    auto wrong_generation = response();
    wrong_generation["data"]["generation"] = "8";
    CHECK(action({intent(), dispatch(), wrong_generation}) == RecoveryAction::FailClosed);

    auto malformed_wrong_token = malformed_response();
    malformed_wrong_token["data"]["dispatch_token"] = "11112222333344445555666677778888";
    CHECK(action({intent(), dispatch(), malformed_wrong_token}) == RecoveryAction::FailClosed);
    auto malformed_mixed_digest = malformed_response();
    malformed_mixed_digest["data"]["response_bytes"] = nullptr;
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(malformed_mixed_digest));
    auto malformed_error_type = malformed_response("error");
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(malformed_error_type));
    auto oversized_missing_type = oversized_response();
    oversized_missing_type["data"]["response_type"] = nullptr;
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(oversized_missing_type));
    auto oversized_digest = oversized_response();
    oversized_digest["data"]["response_sha256"] = kHash;
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(oversized_digest));
    auto oversized_wrong_result = oversized_response();
    oversized_wrong_result["data"]["response_type"] = "text";
    CHECK(action({intent(), dispatch(), oversized_wrong_result}) == RecoveryAction::FailClosed);

    CHECK(action({intent(), dispatch(), response(), unconfirmed_outcome()}) ==
          RecoveryAction::FailClosed);

    auto predispatch_unconfirmed = unconfirmed_outcome();
    CHECK(action({intent(), predispatch_unconfirmed}) == RecoveryAction::FailClosed);

    CHECK(action({none_outcome()}) == RecoveryAction::FailClosed);
    CHECK(action({intent(), dispatch(), none_outcome()}) == RecoveryAction::FailClosed);
    CHECK(action({intent(), none_outcome(), none_outcome()}) == RecoveryAction::FailClosed);
    auto nonnull_none = none_outcome();
    nonnull_none["terminal"] = nlohmann::json::object();
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_outcome(nonnull_none));

    auto wrong_digest = result_outcome();
    wrong_digest["terminal"]["response_sha256"] =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    CHECK(action({intent(), dispatch(), response(), wrong_digest}) == RecoveryAction::FailClosed);

    auto wrong_mutation = error_outcome();
    wrong_mutation["mutation_state"] = "confirmed";
    CHECK(action({intent(), dispatch(), response("error"), wrong_mutation}) ==
          RecoveryAction::FailClosed);
}

TEST_CASE("raw audit v3 enforces exact request and response byte boundaries",
          "[raw][audit-v3][bounds]") {
    auto minimum_intent = intent();
    minimum_intent["request_bytes"] = 2;
    CHECK(tgcli::daemon::raw::audit_v3::valid_intent(minimum_intent));
    minimum_intent["request_bytes"] = 1;
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_intent(minimum_intent));
    minimum_intent["request_bytes"] = 1'048'576;
    CHECK(tgcli::daemon::raw::audit_v3::valid_intent(minimum_intent));
    minimum_intent["request_bytes"] = 1'048'577;
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_intent(minimum_intent));

    auto boundary = response();
    boundary["data"]["response_bytes"] = 16'777'216;
    CHECK(tgcli::daemon::raw::audit_v3::valid_checkpoint(boundary));
    boundary["data"]["response_bytes"] = 16'777'217;
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(boundary));

    auto zero_generation = dispatch();
    zero_generation["data"]["generation"] = "0";
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(zero_generation));
    auto error_as_result = response();
    error_as_result["data"]["response_type"] = "error";
    CHECK_FALSE(tgcli::daemon::raw::audit_v3::valid_checkpoint(error_as_result));
}
