#include "daemon/raw_contract.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

using tgcli::daemon::raw::Digest;
using tgcli::daemon::raw::Error;
using tgcli::daemon::raw::Failure;
using tgcli::daemon::raw::Field;
using tgcli::daemon::raw::Primitive;
using tgcli::daemon::raw::TypedFunction;
using tgcli::daemon::raw::TypePtr;

TypePtr schema() {
    const auto nested = tgcli::daemon::raw::object(
        "fixture", {Field{"enabled", tgcli::daemon::raw::primitive(Primitive::Boolean)}});
    return tgcli::daemon::raw::function(
        "testRawFixture", {
                              Field{"small", tgcli::daemon::raw::primitive(Primitive::Int32)},
                              Field{"safe", tgcli::daemon::raw::primitive(Primitive::Int53)},
                              Field{"wide", tgcli::daemon::raw::primitive(Primitive::Int64)},
                              Field{"bytes", tgcli::daemon::raw::primitive(Primitive::Bytes)},
                              Field{"text", tgcli::daemon::raw::primitive(Primitive::String)},
                              Field{"ratio", tgcli::daemon::raw::primitive(Primitive::Double)},
                              Field{"nested", nested},
                              Field{"ids", tgcli::daemon::raw::vector(
                                               tgcli::daemon::raw::primitive(Primitive::Int64))},
                          });
}

std::string request(std::string_view wide = R"("9223372036854775807")",
                    std::string_view bytes = R"("AAEC/w==")", std::string_view ratio = "1e-7") {
    std::string output = R"({"text":"line\nvalue","@type":"testRawFixture","wide":)";
    output.append(wide);
    output += R"(,"small":-2147483648,"safe":9007199254740991,"bytes":)";
    output.append(bytes);
    output += R"(,"ratio":)";
    output.append(ratio);
    output += R"(,"nested":{"enabled":true,"@type":"fixture"},"ids":["-1",2]})";
    return output;
}

Failure failure(std::string input, const TypePtr& type = schema()) {
    auto parsed = tgcli::daemon::raw::parse(std::move(input), type);
    REQUIRE(std::holds_alternative<Failure>(parsed));
    return std::get<Failure>(std::move(parsed));
}

TypedFunction success(std::string input) {
    auto parsed = tgcli::daemon::raw::parse(std::move(input), schema());
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

TEST_CASE("dormant raw parser produces one typed identity and TD-aware canonical bytes",
          "[raw][foundation][canonical]") {
    auto parsed = success(request());
    const void* identity = parsed.identity();
    CHECK(parsed.name() == "testRawFixture");
    CHECK(
        parsed.canonical() ==
        R"({"@type":"testRawFixture","small":-2147483648,"safe":9007199254740991,"wide":"9223372036854775807","bytes":"AAEC/w==","text":"line\u000avalue","ratio":1e-7,"nested":{"@type":"fixture","enabled":true},"ids":["-1","2"]})");

    const auto request_hash = digest(parsed);
    CHECK(request_hash.bytes == 217);
    CHECK(request_hash.bytes == parsed.canonical().size());
    CHECK(request_hash.sha256 ==
          "sha256:38ab4105c4176b127814cebde6f9fea9644be3ca259adf772c6f7812a52b0491");
    CHECK(parsed.identity() == identity);

    auto equivalent = success(request("9223372036854775807", R"("AAEC/w==")", "0.0000001"));
    CHECK(equivalent.canonical() == parsed.canonical());
    CHECK(digest(equivalent).sha256 == request_hash.sha256);
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

TEST_CASE("dormant raw parser rejects duplicate unknown forbidden and malformed structure",
          "[raw][foundation][parse]") {
    CHECK(failure(R"({"@type":"testRawFixture","@type":"testRawFixture"})").error ==
          Error::DuplicateField);
    CHECK(failure(R"({"@type":"testRawFixture","unknown":1})").error == Error::UnknownField);
    CHECK(failure(R"({"@type":"testRawFixture","@extra":1})").error == Error::UnknownField);
    CHECK(failure(R"({"@type":"testRawFixture","@client_id":1})").error == Error::UnknownField);
    CHECK(failure("[]").error == Error::InvalidTopLevel);
    CHECK(failure(R"({"small":1})").error == Error::MissingType);
    CHECK(failure(R"({"@type":"other"})").error == Error::UnexpectedType);
    CHECK(failure(R"({"@type":"testRawFixture"})").error == Error::MissingField);
    CHECK(failure(R"({"@type":"testRawFixture","small":1,)").error == Error::InvalidJson);

    auto nested_duplicate = request();
    constexpr std::string_view needle = R"("enabled":true)";
    nested_duplicate.insert(nested_duplicate.find(needle) + std::string(needle).size(),
                            R"(,"enabled":false)");
    const auto nested_failure = failure(std::move(nested_duplicate));
    CHECK(nested_failure.error == Error::DuplicateField);

    const auto duplicate_schema = tgcli::daemon::raw::function(
        "testRawFixture", {Field{"value", tgcli::daemon::raw::primitive(Primitive::String)},
                           Field{"value", tgcli::daemon::raw::primitive(Primitive::String)}});
    CHECK(failure(R"({"@type":"testRawFixture","value":"x"})", duplicate_schema).error ==
          Error::InvalidPolicyMetadata);
    const auto reserved_schema = tgcli::daemon::raw::function(
        "testRawFixture", {Field{"@extra", tgcli::daemon::raw::primitive(Primitive::String)}});
    CHECK(failure(R"({"@type":"testRawFixture","@extra":"x"})", reserved_schema).error ==
          Error::InvalidPolicyMetadata);
}

TEST_CASE("dormant raw typed conversion enforces integer bytes double depth and size bounds",
          "[raw][foundation][bounds]") {
    auto int32_overflow = request();
    int32_overflow.replace(int32_overflow.find("-2147483648"), 11, "-2147483649");
    const auto int32_failure = failure(std::move(int32_overflow));
    CHECK(int32_failure.error == Error::InvalidInteger);

    auto int53_overflow = request();
    int53_overflow.replace(int53_overflow.find("9007199254740991"), 16, "9007199254740992");
    const auto int53_failure = failure(std::move(int53_overflow));
    CHECK(int53_failure.error == Error::InvalidInteger);
    CHECK(failure(request(R"("09223372036854775807")")).error == Error::InvalidInteger);
    CHECK(failure(request(R"("9223372036854775808")")).error == Error::InvalidInteger);
    CHECK(failure(request(R"("1")", R"("AAE")")).error == Error::InvalidBase64);
    CHECK(failure(request(R"("1")", R"("A===")")).error == Error::InvalidBase64);

    auto string_double = request();
    string_double.replace(string_double.find("1e-7"), 4, R"("1e-7")");
    const auto double_failure = failure(std::move(string_double));
    CHECK(double_failure.error == Error::InvalidDouble);

    std::string nested = "0";
    for (std::size_t depth = 0; depth <= tgcli::daemon::raw::kMaximumJsonDepth; ++depth) {
        nested.insert(nested.begin(), '[');
        nested.push_back(']');
    }
    const auto depth_failure = failure(std::move(nested));
    CHECK(depth_failure.error == Error::MaximumDepthExceeded);
    CHECK(failure(std::string(tgcli::daemon::raw::kMaximumRequestBytes + 1, ' ')).error ==
          Error::InputTooLarge);

    auto exact_physical = request();
    exact_physical.append(tgcli::daemon::raw::kMaximumRequestBytes - exact_physical.size(), ' ');
    auto exact_parsed = tgcli::daemon::raw::parse(std::move(exact_physical), schema());
    CHECK(std::holds_alternative<TypedFunction>(exact_parsed));

    auto expanded = request();
    const auto text = expanded.find(R"(line\nvalue)");
    REQUIRE(text != std::string::npos);
    std::string escaped_newlines;
    escaped_newlines.reserve(360'000);
    for (std::size_t index = 0; index < 180'000; ++index) {
        escaped_newlines += "\\n";
    }
    expanded.replace(text, std::string_view(R"(line\nvalue)").size(), escaped_newlines);
    CHECK(expanded.size() <= tgcli::daemon::raw::kMaximumRequestBytes);
    const auto expanded_failure = failure(std::move(expanded));
    CHECK(expanded_failure.error == Error::CanonicalTooLarge);
}

TEST_CASE("dormant raw double canonicalization follows RFC 8785 thresholds",
          "[raw][foundation][canonical][double]") {
    CHECK(success(request(R"("1")", R"("AAEC/w==")", "-0")).canonical().find(R"("ratio":0)") !=
          std::string_view::npos);
    CHECK(success(request(R"("1")", R"("AAEC/w==")", "0.000001"))
              .canonical()
              .find(R"("ratio":0.000001)") != std::string_view::npos);
    CHECK(success(request(R"("1")", R"("AAEC/w==")", "100000000000000000000"))
              .canonical()
              .find(R"("ratio":100000000000000000000)") != std::string_view::npos);
    CHECK(
        success(request(R"("1")", R"("AAEC/w==")", "1e21")).canonical().find(R"("ratio":1e+21)") !=
        std::string_view::npos);
    CHECK(success(request(R"("1")", R"("AAEC/w==")", "333333333.33333329"))
              .canonical()
              .find(R"("ratio":333333333.3333333)") != std::string_view::npos);
}

TEST_CASE("dormant raw response digest enforces the canonical response ceiling",
          "[raw][foundation][response]") {
    auto exact = tgcli::daemon::raw::response_digest(
        "testRawFixture", std::string(tgcli::daemon::raw::kMaximumResponseBytes, 'x'));
    REQUIRE(std::holds_alternative<tgcli::daemon::raw::Digest>(exact));
    CHECK(std::get<tgcli::daemon::raw::Digest>(exact).bytes ==
          tgcli::daemon::raw::kMaximumResponseBytes);

    auto oversized = tgcli::daemon::raw::response_digest(
        "testRawFixture", std::string(tgcli::daemon::raw::kMaximumResponseBytes + 1, 'x'));
    REQUIRE(std::holds_alternative<Failure>(oversized));
    CHECK(std::get<Failure>(oversized).error == Error::CanonicalTooLarge);

    auto invalid_function = tgcli::daemon::raw::response_digest("not-a-function", "{}");
    REQUIRE(std::holds_alternative<Failure>(invalid_function));
    CHECK(std::get<Failure>(invalid_function).error == Error::InvalidPolicyMetadata);
    auto empty = tgcli::daemon::raw::response_digest("testRawFixture", "");
    REQUIRE(std::holds_alternative<Failure>(empty));
    CHECK(std::get<Failure>(empty).error == Error::InvalidPolicyMetadata);
}

TEST_CASE("dormant raw sensitive owners wipe physical AST and canonical buffers",
          "[raw][foundation][wipe]") {
    std::vector<std::string> stages;
    auto physical = request();
    {
        auto parsed = tgcli::daemon::raw::parse(
            std::move(physical), schema(),
            [&](std::string_view stage, const char* bytes, std::size_t size) {
                CHECK(std::string_view(bytes, size) == std::string(size, '\0'));
                stages.emplace_back(stage);
            });
        REQUIRE(std::holds_alternative<TypedFunction>(parsed));
    }
    // The rvalue-reference contract deliberately clears the caller-owned source.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    CHECK(physical.empty());
    CHECK(std::ranges::find(stages, "raw_physical_input") != stages.end());
    CHECK(std::ranges::find(stages, "raw_ast_string") != stages.end());
    CHECK(std::ranges::find(stages, "raw_ast_key") != stages.end());
    CHECK(std::ranges::find(stages, "raw_canonical") != stages.end());

    std::string response = R"({"@type":"ok"})";
    auto hashed = tgcli::daemon::raw::response_digest("testRawFixture", std::move(response));
    REQUIRE(std::holds_alternative<Digest>(hashed));
    // The rvalue-reference contract deliberately clears the caller-owned source.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    CHECK(response.empty());
}
