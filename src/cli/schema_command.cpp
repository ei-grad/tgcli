#include "cli/schema_command.hpp"

#include "cli/schema_catalog.hpp"
#include "common/exit_codes.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::cli {

namespace {

constexpr std::string_view kHelp = R"(Print curated JSON schemas

Usage:
  tgcli schema [OPTIONS] command...

Positionals:
  command TEXT ... REQUIRED    command path (for example: account list)

Options:
  -h,--help                    Print this help message and exit
  --all                        include every cataloged result, item, and error schema
)";

void write_stdout(std::string_view bytes) {
    std::fwrite(bytes.data(), sizeof(char), bytes.size(), stdout);
}

int report_missing_target() {
    const nlohmann::json rendered{
        {"error",
         {{"code", "USAGE"},
          {"message", "required command argument is missing"},
          {"details", {{"argument", nullptr}, {"reason", "missing_argument"}}}}}};
    std::fputs((rendered.dump() + "\n").c_str(), stderr);
    return kUsage;
}

int report_unsupported_option(std::string_view option) {
    const nlohmann::json rendered{
        {"error",
         {{"code", "USAGE"},
          {"message", std::string(option) + " is not supported for this command"},
          {"details", {{"argument", option}, {"reason", "unsupported_mode"}}}}}};
    std::fputs((rendered.dump() + "\n").c_str(), stderr);
    return kUsage;
}

int report_unknown_target(std::string_view normalized_target) {
    const nlohmann::json rendered{
        {"error",
         {{"code", "USAGE"},
          {"message", "no curated schema is available for command"},
          {"details", {{"argument", normalized_target}, {"reason", "unknown_command"}}}}}};
    std::fputs((rendered.dump() + "\n").c_str(), stderr);
    return kUsage;
}

} // namespace

int run_schema_command(const SchemaCommandInvocation& invocation) {
    if (invocation.help) {
        write_stdout(kHelp);
        return kOk;
    }
    if (!invocation.unsupported_option.empty()) {
        return report_unsupported_option(invocation.unsupported_option);
    }

    std::vector<std::string_view> target_views;
    target_views.reserve(invocation.target_tokens.size());
    for (const auto& token : invocation.target_tokens) {
        target_views.emplace_back(token);
    }
    const std::optional<std::string> normalized = normalize_schema_target(target_views);
    if (!normalized) {
        return report_missing_target();
    }

    const auto schemas = find_schema_set(canonicalize_schema_target(*normalized));
    if (!schemas) {
        return report_unknown_target(*normalized);
    }

    const SchemaMappingView* primary = primary_schema(*schemas);
    if (!invocation.all && primary == nullptr) {
        return report_unknown_target(*normalized);
    }
    if (invocation.verbose) {
        std::fputs("diagnostic: transport=local\n", stderr);
    }
    if (invocation.all) {
        write_stdout(render_all_schemas(*schemas));
    } else {
        write_stdout(primary->bytes);
    }
    return kOk;
}

} // namespace tgcli::cli
