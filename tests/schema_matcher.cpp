#include "schema_matcher.hpp"

#include <fstream>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <stdexcept>
#include <utility>

namespace tgcli::test {

JsonSchemaMatcher::JsonSchemaMatcher(std::string schema_filename)
    : schema_filename_(std::move(schema_filename)) {}

bool JsonSchemaMatcher::match(const nlohmann::json& value) const {
    failure_.clear();
    try {
        const auto schema = jsoncons::json::parse(load_schema_document(schema_filename_).dump());
        const auto compiled = jsoncons::jsonschema::make_json_schema(schema);
        const auto instance = jsoncons::json::parse(value.dump());
        auto reporter = [this](const jsoncons::jsonschema::validation_message& message) {
            if (failure_.empty()) {
                failure_ = message.instance_location().string() + ": " + message.message();
            }
            return jsoncons::jsonschema::walk_state::abort;
        };
        compiled.validate(instance, reporter);
        return failure_.empty();
    } catch (const std::exception& error) {
        failure_ = error.what();
        return false;
    }
}

std::string JsonSchemaMatcher::describe() const {
    std::string description = "matches " + schema_filename_;
    if (!failure_.empty()) {
        description += " (" + failure_ + ")";
    }
    return description;
}

JsonSchemaMatcher matches_json_schema(std::string schema_filename) {
    return JsonSchemaMatcher(std::move(schema_filename));
}

std::filesystem::path schema_path(const std::string& filename) {
    return std::filesystem::path(TGCLI_SCHEMA_DIR) / filename;
}

nlohmann::json load_schema_document(const std::string& filename) {
    const auto file = schema_path(filename);
    std::ifstream input(file);
    if (!input) {
        throw std::runtime_error("cannot open schema file " + file.string());
    }
    return nlohmann::json::parse(input);
}

} // namespace tgcli::test
