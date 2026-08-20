#include "common/canonical_json.hpp"
#include "common/sha256.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

std::string canonical(const json& value) {
    auto result = tgcli::common::canonical_json(value);
    REQUIRE(std::holds_alternative<std::string>(result));
    return std::get<std::string>(std::move(result));
}

} // namespace

TEST_CASE("SHA-256 matches standard one-shot vectors", "[hash]") {
    CHECK(tgcli::common::sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(tgcli::common::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(tgcli::common::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(tgcli::common::sha256_hex(std::string(1'000'000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const std::string binary{"\x00\xff\x80"
                             "A",
                             4};
    CHECK(tgcli::common::sha256_hex(binary) ==
          "3507b01e644277ad3cd10dadd6e33cb801151e62e3cb899a67409ef701d6079c");
}

TEST_CASE("SHA-256 streaming is invariant across every block split", "[hash]") {
    std::string input;
    input.reserve(257);
    for (std::size_t index = 0; index < 257; ++index) {
        input.push_back(static_cast<char>(index & 0xffU));
    }
    const auto expected = tgcli::common::sha256_hex(input);
    for (std::size_t split = 0; split <= input.size(); ++split) {
        tgcli::common::Sha256 digest;
        digest.update(std::string_view(input).substr(0, split));
        digest.update(std::string_view(input).substr(split));
        CHECK(digest.finish_hex() == expected);
        CHECK(digest.finish_hex() == expected);
    }

    for (std::size_t chunk = 1; chunk <= 80; ++chunk) {
        tgcli::common::Sha256 digest;
        for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
            digest.update(std::string_view(input).substr(offset, chunk));
        }
        CHECK(digest.finish_hex() == expected);
    }
}

TEST_CASE("domain-separated SHA-256 inserts one explicit separator", "[hash]") {
    CHECK(tgcli::common::domain_separated_sha256("tgcli-idempotency-key-v1", "alpha") ==
          "sha256:89f2ff7a73975b9a4fed4b6b61588c7b6d1108fd718cd41c81662036091f901c");
    CHECK(tgcli::common::domain_separated_sha256("tgcli-invite-link-v1", "https://t.me/+secret") ==
          "sha256:6605f3c708c3aa61aed5f82aa89061e8e96a1f0340264de1fc6476fbc4aa4f76");
    CHECK_THROWS_AS(
        tgcli::common::domain_separated_sha256(std::string_view("invalid\0domain", 14), "payload"),
        std::invalid_argument);
}

TEST_CASE("canonical JSON emits the closed scalar domain", "[canonical-json]") {
    CHECK(canonical(nullptr) == "null");
    CHECK(canonical(true) == "true");
    CHECK(canonical(false) == "false");
    CHECK(canonical(json(std::numeric_limits<std::int64_t>::min())) == "-9223372036854775808");
    CHECK(canonical(json(std::numeric_limits<std::uint64_t>::max())) == "18446744073709551615");
    CHECK(canonical(json(0)) == "0");
    CHECK(canonical(json::array({nullptr, false, -1, 2, "text"})) == "[null,false,-1,2,\"text\"]");
}

TEST_CASE("canonical JSON sorts object keys by unsigned UTF-8 bytes", "[canonical-json]") {
    json value = json::object();
    value["aa"] = 2;
    value["a"] = 1;
    value[std::string("\xc2\x80", 2)] = 4;
    value["z"] = 3;
    value[std::string("\xc3\xa9", 2)] = 5;
    CHECK(canonical(value) ==
          std::string("{\"a\":1,\"aa\":2,\"z\":3,\"") + "\xc2\x80\":4,\"" + "\xc3\xa9\":5}");

    const json nested{{"outer", json{{"b", 2}, {"a", 1}}}, {"array", json::array({2, 1})}};
    CHECK(canonical(nested) == "{\"array\":[2,1],\"outer\":{\"a\":1,\"b\":2}}");
}

TEST_CASE("canonical JSON uses required escapes and raw Unicode", "[canonical-json]") {
    const std::string controls{"\x00\x01\x1f\"\\/\n", 7};
    CHECK(canonical(controls) == "\"\\u0000\\u0001\\u001f\\\"\\\\/\\u000a\"");
    CHECK(canonical(std::string("caf\xc3\xa9 \xf0\x9f\x91\x8d", 10)) ==
          std::string("\"caf\xc3\xa9 \xf0\x9f\x91\x8d\"", 12));
}

TEST_CASE("canonical JSON rejects values outside its exact domain", "[canonical-json]") {
    for (const auto& value : {json(1.5), json::binary({1, 2}), json::parse("{", nullptr, false)}) {
        auto result = tgcli::common::canonical_json(value);
        REQUIRE(std::holds_alternative<tgcli::common::CanonicalJsonError>(result));
        CHECK(std::get<tgcli::common::CanonicalJsonError>(result) ==
              tgcli::common::CanonicalJsonError::UnsupportedType);
    }

    for (const auto& bytes : {std::string("\xff", 1), std::string("\xc0\xaf", 2),
                              std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4)}) {
        auto invalid_string = tgcli::common::canonical_json(json(bytes));
        REQUIRE(std::holds_alternative<tgcli::common::CanonicalJsonError>(invalid_string));
        CHECK(std::get<tgcli::common::CanonicalJsonError>(invalid_string) ==
              tgcli::common::CanonicalJsonError::InvalidUtf8);
    }

    json invalid_key = json::object();
    invalid_key[std::string("\xed\xa0\x80", 3)] = 1;
    auto invalid_object = tgcli::common::canonical_json(invalid_key);
    REQUIRE(std::holds_alternative<tgcli::common::CanonicalJsonError>(invalid_object));
    CHECK(std::get<tgcli::common::CanonicalJsonError>(invalid_object) ==
          tgcli::common::CanonicalJsonError::InvalidUtf8);
}
