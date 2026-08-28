#include "daemon/raw_contract.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

namespace {

using tgcli::daemon::raw::Digest;
using tgcli::daemon::raw::Error;
using tgcli::daemon::raw::Failure;
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
    const auto empty_response = td::td_api::make_object<td::td_api::testString>("");
    auto empty_digest = tgcli::daemon::raw::response_digest(function, *empty_response);
    REQUIRE(std::holds_alternative<Digest>(empty_digest));
    const auto overhead = std::get<Digest>(empty_digest).bytes;
    REQUIRE(overhead < tgcli::daemon::raw::kMaximumResponseBytes);

    const auto exact_response = td::td_api::make_object<td::td_api::testString>(
        std::string(tgcli::daemon::raw::kMaximumResponseBytes - overhead, 'x'));
    auto exact = tgcli::daemon::raw::response_digest(function, *exact_response);
    REQUIRE(std::holds_alternative<Digest>(exact));
    CHECK(std::get<Digest>(exact).bytes == tgcli::daemon::raw::kMaximumResponseBytes);

    const auto oversized_response = td::td_api::make_object<td::td_api::testString>(
        std::string(tgcli::daemon::raw::kMaximumResponseBytes - overhead + 1, 'x'));
    auto oversized = tgcli::daemon::raw::response_digest(function, *oversized_response);
    REQUIRE(std::holds_alternative<Failure>(oversized));
    CHECK(std::get<Failure>(oversized).error == Error::CanonicalTooLarge);

    auto wrong = tgcli::daemon::raw::response_digest(function, td::td_api::ok{});
    REQUIRE(std::holds_alternative<Failure>(wrong));
    CHECK(std::get<Failure>(wrong).error == Error::UnexpectedResponseType);
    auto td_error = tgcli::daemon::raw::response_digest(function, td::td_api::error(400, "x"));
    CHECK(std::holds_alternative<Digest>(td_error));
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
    const auto response = td::td_api::make_object<td::td_api::testString>("sensitive response");
    const auto response_hash = tgcli::daemon::raw::response_digest(function, *response, observer);
    REQUIRE(std::holds_alternative<Digest>(response_hash));
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
