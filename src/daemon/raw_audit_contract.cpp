#include "daemon/raw_audit_contract.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>

namespace tgcli::daemon::raw::audit_v3 {

namespace {

using nlohmann::json;

constexpr std::uint64_t kMaximumRequestBytes = 1'048'576;
constexpr std::uint64_t kMaximumResponseBytes = 16'777'216;

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields,
                               [&](std::string_view field) { return value.contains(field); });
}

bool ascii_identifier(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 128 &&
           ((text.front() >= 'A' && text.front() <= 'Z') ||
            (text.front() >= 'a' && text.front() <= 'z')) &&
           std::ranges::all_of(text.substr(1), [](char character) {
               return (character >= 'A' && character <= 'Z') ||
                      (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9');
           });
}

bool hex(const json& value, std::size_t length) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == length && std::ranges::all_of(text, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool hash(const json& value) {
    return value.is_string() && value.get_ref<const std::string&>().size() == 71 &&
           value.get_ref<const std::string&>().starts_with("sha256:") &&
           hex(value.get_ref<const std::string&>().substr(7), 64);
}

bool canonical_uint64(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return error == std::errc{} && end == text.data() + text.size() && parsed > 0;
}

bool bounded_unsigned(const json& value, std::uint64_t minimum, std::uint64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number >= minimum && number <= maximum;
    }
    if (!value.is_number_integer()) {
        return false;
    }
    const auto number = value.get<std::int64_t>();
    return number >= 0 && static_cast<std::uint64_t>(number) >= minimum &&
           static_cast<std::uint64_t>(number) <= maximum;
}

bool valid_common(const json& value, std::string_view record_type) {
    return value.contains("schema_version") && value["schema_version"] == 3 &&
           value.contains("record_type") && value["record_type"] == record_type &&
           value.contains("invocation_id") && hex(value["invocation_id"], 32);
}

bool valid_dispatch_data(const json& value) {
    return exact_fields(value, {"dispatch_token", "generation"}) &&
           hex(value["dispatch_token"], 32) && canonical_uint64(value["generation"]);
}

bool valid_response_data(const json& value) {
    if (!exact_fields(value, {"dispatch_token", "generation", "kind", "response_type",
                              "td_error_code", "response_sha256", "response_bytes"}) ||
        !hex(value["dispatch_token"], 32) || !canonical_uint64(value["generation"]) ||
        !value["kind"].is_string() || !ascii_identifier(value["response_type"]) ||
        !hash(value["response_sha256"]) ||
        !bounded_unsigned(value["response_bytes"], 2, kMaximumResponseBytes)) {
        return false;
    }
    if (value["kind"] == "result") {
        return value["response_type"] != "error" && value["td_error_code"].is_null();
    }
    return value["kind"] == "error" && value["response_type"] == "error" &&
           value["td_error_code"].is_number_integer() &&
           value["td_error_code"].get<std::int64_t>() >= std::numeric_limits<std::int32_t>::min() &&
           value["td_error_code"].get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max();
}

bool valid_result_terminal(const json& value) {
    return exact_fields(value, {"kind", "response_type", "response_sha256", "response_bytes"}) &&
           value["kind"] == "result_digest" && ascii_identifier(value["response_type"]) &&
           value["response_type"] != "error" && hash(value["response_sha256"]) &&
           bounded_unsigned(value["response_bytes"], 2, kMaximumResponseBytes);
}

bool valid_error_terminal(const json& value) {
    if (!exact_fields(value, {"kind", "code", "td_error_code"}) ||
        value["kind"] != "error_summary" || !value["code"].is_string()) {
        return false;
    }
    const auto& code = value["code"].get_ref<const std::string&>();
    if (code != "RATE_LIMITED" && code != "TDLIB_ERROR" && code != "RAW_OUTCOME_UNCONFIRMED") {
        return false;
    }
    if (code == "RAW_OUTCOME_UNCONFIRMED") {
        return value["td_error_code"].is_null();
    }
    return value["td_error_code"].is_number_integer() &&
           value["td_error_code"].get<std::int64_t>() >= std::numeric_limits<std::int32_t>::min() &&
           value["td_error_code"].get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max() &&
           (code != "RATE_LIMITED" || value["td_error_code"] == 429);
}

struct Invocation {
    const json* intent = nullptr;
    const json* dispatch = nullptr;
    const json* response = nullptr;
    const json* outcome = nullptr;
    bool invalid = false;
};

bool response_matches_dispatch(const json& dispatch, const json& response) {
    return dispatch["data"]["dispatch_token"] == response["data"]["dispatch_token"] &&
           dispatch["data"]["generation"] == response["data"]["generation"];
}

bool outcome_matches_response(const json& response, const json& outcome) {
    const auto& data = response["data"];
    const auto& terminal = outcome["terminal"];
    if (data["kind"] == "result") {
        return outcome["mutation_state"] == "confirmed" && terminal["kind"] == "result_digest" &&
               terminal["response_type"] == data["response_type"] &&
               terminal["response_sha256"] == data["response_sha256"] &&
               terminal["response_bytes"] == data["response_bytes"];
    }
    if (outcome["mutation_state"] != "possible" || terminal["kind"] != "error_summary") {
        return false;
    }
    constexpr std::string_view rate_limited = "RATE_LIMITED";
    constexpr std::string_view tdlib_error = "TDLIB_ERROR";
    const auto code = data["td_error_code"] == 429 ? rate_limited : tdlib_error;
    return terminal["code"] == code && terminal["td_error_code"] == data["td_error_code"];
}

bool reject(Invocation& invocation) {
    invocation.invalid = true;
    return false;
}

bool consume_intent(const json& record, Invocation& invocation) {
    if (!valid_intent(record) || invocation.intent != nullptr || invocation.dispatch != nullptr ||
        invocation.response != nullptr || invocation.outcome != nullptr) {
        return reject(invocation);
    }
    invocation.intent = &record;
    return true;
}

bool consume_checkpoint(const json& record, Invocation& invocation) {
    if (!valid_checkpoint(record) || invocation.intent == nullptr ||
        invocation.outcome != nullptr) {
        return reject(invocation);
    }
    if (record["stage"] == "raw_dispatch_started") {
        if (invocation.dispatch != nullptr || invocation.response != nullptr) {
            return reject(invocation);
        }
        invocation.dispatch = &record;
        return true;
    }
    if (invocation.dispatch == nullptr || invocation.response != nullptr ||
        !response_matches_dispatch(*invocation.dispatch, record)) {
        return reject(invocation);
    }
    invocation.response = &record;
    return true;
}

bool consume_outcome(const json& record, Invocation& invocation) {
    if (!valid_outcome(record) || invocation.intent == nullptr || invocation.dispatch == nullptr ||
        invocation.outcome != nullptr) {
        return reject(invocation);
    }
    const bool unconfirmed_without_response =
        invocation.response == nullptr && record["mutation_state"] == "possible" &&
        record["terminal"]["kind"] == "error_summary" &&
        record["terminal"]["code"] == "RAW_OUTCOME_UNCONFIRMED";
    if (!unconfirmed_without_response &&
        (invocation.response == nullptr ||
         !outcome_matches_response(*invocation.response, record))) {
        return reject(invocation);
    }
    invocation.outcome = &record;
    return true;
}

bool consume_record(const json& record,
                    std::map<std::string, Invocation, std::less<>>& invocations) {
    if (!record.is_object() || !record.contains("record_type") ||
        !record["record_type"].is_string() || !record.contains("invocation_id") ||
        !record["invocation_id"].is_string()) {
        return false;
    }
    auto& invocation = invocations[record["invocation_id"].get<std::string>()];
    const auto& type = record["record_type"].get_ref<const std::string&>();
    if (type == "raw_intent") {
        return consume_intent(record, invocation);
    }
    if (type == "raw_checkpoint") {
        return consume_checkpoint(record, invocation);
    }
    if (type == "raw_outcome") {
        return consume_outcome(record, invocation);
    }
    return reject(invocation);
}

RecoveryAction recovery_action(const Invocation& invocation) {
    if (invocation.invalid || invocation.intent == nullptr) {
        return RecoveryAction::FailClosed;
    }
    if (invocation.outcome != nullptr) {
        return RecoveryAction::Complete;
    }
    if (invocation.response != nullptr) {
        return (*invocation.response)["data"]["kind"] == "result"
                   ? RecoveryAction::RepairConfirmedResult
                   : RecoveryAction::RepairPossibleError;
    }
    return invocation.dispatch != nullptr ? RecoveryAction::EmitUnconfirmed
                                          : RecoveryAction::NoMutation;
}

} // namespace

bool valid_intent(const nlohmann::json& value) {
    return exact_fields(value, {"schema_version", "record_type", "invocation_id", "function",
                                "tier", "tdlib_sha", "request_sha256", "request_bytes"}) &&
           valid_common(value, "raw_intent") && ascii_identifier(value["function"]) &&
           (value["tier"] == "write" || value["tier"] == "destructive") &&
           hex(value["tdlib_sha"], 40) && hash(value["request_sha256"]) &&
           bounded_unsigned(value["request_bytes"], 2, kMaximumRequestBytes);
}

bool valid_checkpoint(const nlohmann::json& value) {
    if (!exact_fields(value, {"schema_version", "record_type", "invocation_id", "stage", "data"}) ||
        !valid_common(value, "raw_checkpoint") || !value["stage"].is_string()) {
        return false;
    }
    if (value["stage"] == "raw_dispatch_started") {
        return valid_dispatch_data(value["data"]);
    }
    return value["stage"] == "raw_response_received" && valid_response_data(value["data"]);
}

bool valid_outcome(const nlohmann::json& value) {
    if (!exact_fields(value, {"schema_version", "record_type", "invocation_id", "mutation_state",
                              "terminal"}) ||
        !valid_common(value, "raw_outcome") || !value["mutation_state"].is_string()) {
        return false;
    }
    if (value["mutation_state"] == "confirmed") {
        return valid_result_terminal(value["terminal"]);
    }
    return value["mutation_state"] == "possible" && valid_error_terminal(value["terminal"]);
}

ScanResult scan(std::span<const nlohmann::json> records) {
    std::map<std::string, Invocation, std::less<>> invocations;
    bool valid = true;
    for (const auto& record : records) {
        valid = consume_record(record, invocations) && valid;
    }

    ScanResult result{.valid = valid, .decisions = {}};
    result.decisions.reserve(invocations.size());
    for (const auto& [id, invocation] : invocations) {
        result.decisions.push_back({.invocation_id = id, .action = recovery_action(invocation)});
    }
    return result;
}

} // namespace tgcli::daemon::raw::audit_v3
