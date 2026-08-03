#pragma once

#include <filesystem>
#include <string>

#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

namespace tgcli::test {

class JsonSchemaMatcher final : public Catch::Matchers::MatcherBase<nlohmann::json> {
  public:
    explicit JsonSchemaMatcher(std::string schema_filename);

    bool match(const nlohmann::json& value) const override;
    std::string describe() const override;

  private:
    std::string schema_filename_;
    mutable std::string failure_;
};

JsonSchemaMatcher matches_json_schema(std::string schema_filename);
std::filesystem::path schema_path(const std::string& filename);
nlohmann::json load_schema_document(const std::string& filename);

} // namespace tgcli::test
