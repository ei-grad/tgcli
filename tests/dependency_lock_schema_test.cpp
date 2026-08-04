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

    auto falsely_resolved = canonical;
    auto* musl = find_component(falsely_resolved, "musl");
    REQUIRE(musl != nullptr);
    (*musl)["version"] = "unverified";
    CHECK_FALSE(validate(falsely_resolved).empty());

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
