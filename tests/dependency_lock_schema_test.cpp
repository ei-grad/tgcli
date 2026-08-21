#include <fstream>
#include <iterator>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

jsoncons::json load_json(const std::string& filename) {
    std::ifstream input(std::string(TGCLI_RELEASE_DIR) + "/" + filename);
    REQUIRE(input);
    return jsoncons::json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

std::vector<std::string> validate(const jsoncons::json& instance) {
    const auto compiled =
        jsoncons::jsonschema::make_json_schema(load_json("dependencies.lock.schema.json"));
    std::vector<std::string> failures;
    auto reporter = [&failures](const jsoncons::jsonschema::validation_message& message) {
        failures.push_back(message.instance_location().string() + ": " + message.message());
        return jsoncons::jsonschema::walk_state::advance;
    };
    compiled.validate(instance, reporter);
    return failures;
}

jsoncons::json* find_component(jsoncons::json& document, std::string_view id) {
    auto& components = document["components"];
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (components[index]["id"].as<std::string>() == id) {
            return &components[index];
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("dependency source lock matches its strict schema", "[dependency-lock]") {
    const auto lock = load_json("dependencies.lock.json");
    CHECK(validate(lock).empty());
}

TEST_CASE("dependency source lock schema rejects incomplete and unknown data",
          "[dependency-lock]") {
    const auto canonical = load_json("dependencies.lock.json");

    auto unknown = canonical;
    unknown["unexpected"] = true;
    CHECK_FALSE(validate(unknown).empty());

    auto incomplete = canonical;
    incomplete["components"][0].erase("archive_sha256");
    CHECK_FALSE(validate(incomplete).empty());

    auto unresolved = canonical;
    auto* musl = find_component(unresolved, "musl");
    REQUIRE(musl != nullptr);
    (*musl)["lock_state"] = "unresolved";
    CHECK_FALSE(validate(unresolved).empty());

    auto incomplete_toolchain = canonical;
    incomplete_toolchain["release_toolchains"][0].erase("producer_build_log");
    CHECK_FALSE(validate(incomplete_toolchain).empty());

    auto incomplete_runtime = canonical;
    incomplete_runtime["release_toolchains"][0]["runtime_files"] = jsoncons::json::array();
    CHECK_FALSE(validate(incomplete_runtime).empty());

    auto invalid_archive_size = canonical;
    invalid_archive_size["components"][0]["archive_size"] = true;
    CHECK_FALSE(validate(invalid_archive_size).empty());

    auto incomplete_tree = canonical;
    incomplete_tree["components"][0].erase("source_tree_sha256");
    CHECK_FALSE(validate(incomplete_tree).empty());

    auto incomplete_generated_tree = canonical;
    auto* generated_tdlib = find_component(incomplete_generated_tree, "tdlib");
    REQUIRE(generated_tdlib != nullptr);
    generated_tdlib->erase("generated_source_tree_sha256");
    CHECK_FALSE(validate(incomplete_generated_tree).empty());

    auto unexpected_tree = canonical;
    auto* unexpected_musl = find_component(unexpected_tree, "musl");
    REQUIRE(unexpected_musl != nullptr);
    (*unexpected_musl)["source_tree_sha256"] = std::string(64, '0');
    CHECK_FALSE(validate(unexpected_tree).empty());

    auto unpinned_image = canonical;
    unpinned_image["release_toolchains"][0]["image"] = "docker.io/dockcross/base:latest";
    CHECK_FALSE(validate(unpinned_image).empty());

    auto boolean_version = canonical;
    boolean_version["schema_version"] = true;
    CHECK_FALSE(validate(boolean_version).empty());
}

TEST_CASE("TDLib embedded SQLite licensing is complete", "[dependency-lock]") {
    auto lock = load_json("dependencies.lock.json");
    auto* tdlib = find_component(lock, "tdlib");
    REQUIRE(tdlib != nullptr);

    jsoncons::json* sqlite = nullptr;
    for (auto& embedded : (*tdlib)["embedded_components"].array_range()) {
        if (embedded["id"].as<std::string>() == "tdlib-sqlite-sqlcipher") {
            sqlite = &embedded;
            break;
        }
    }
    REQUIRE(sqlite != nullptr);
    CHECK((*sqlite)["license_expression"].as<std::string>() == "blessing AND BSD-3-Clause");

    std::set<std::string> license_paths;
    for (const auto& license_file : (*sqlite)["license_files"].array_range()) {
        license_paths.insert(license_file["path"].as<std::string>());
    }
    CHECK(license_paths == std::set<std::string>{"release/licenses/SQLite-blessing.txt",
                                                 "release/licenses/TDLib-SQLCipher.txt"});
}

TEST_CASE("RE2 source and embedded UTF licensing match the accepted gate", "[dependency-lock]") {
    auto lock = load_json("dependencies.lock.json");
    auto* re2 = find_component(lock, "re2");
    REQUIRE(re2 != nullptr);

    CHECK((*re2)["version"].as<std::string>() == "2022-12-01");
    CHECK((*re2)["immutable_ref"].as<std::string>() == "4be240789d5b322df9f02b7e19c8651f3ccbf205");
    CHECK((*re2)["archive_sha256"].as<std::string>() ==
          "da5c23ecdb9a55c82d6802ee55812dfb99a035a4838287c0b7c0051bd0fdb9fc");
    CHECK((*re2)["source_tree_sha256"].as<std::string>() ==
          "6d3942bcd96377f18ec60a7b190d1b217d037ff0132ff6ae8dc463347c067046");

    const auto& embedded = (*re2)["embedded_components"];
    REQUIRE(embedded.size() == 1);
    CHECK(embedded[0]["id"].as<std::string>() == "re2-plan9-utf");
    CHECK(embedded[0]["source_path"].as<std::string>() == "util/rune.cc and util/utf.h");
    CHECK(embedded[0]["license_expression"].as<std::string>() == "LicenseRef-RE2-Lucent-2002");
}
