#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tgcli::cli {

enum class SchemaPayloadKind : std::uint8_t { Result, Item, Error };

struct EmbeddedCatalogView {
    std::string_view filename;
    std::string_view bytes;
};

struct SchemaMappingView {
    std::string_view command;
    SchemaPayloadKind kind;
    std::string_view filename;
    std::string_view bytes;
};

struct SchemaSetView {
    const SchemaMappingView* result = nullptr;
    const SchemaMappingView* item = nullptr;
    const SchemaMappingView* error = nullptr;
};

// Generated build-tree data. These spans are sorted and live for the process lifetime.
std::span<const EmbeddedCatalogView> embedded_schema_catalogs() noexcept;
std::span<const SchemaMappingView> embedded_schema_mappings() noexcept;

// Valid operands after `tgcli schema`; this is not the general CLI command registry.
std::span<const std::string_view> schema_target_completion_keys() noexcept;

std::optional<std::string> normalize_schema_target(std::span<const std::string_view> tokens);
std::string canonicalize_schema_target(std::string normalized);
std::optional<SchemaSetView> find_schema_set(std::string_view canonical_command) noexcept;
const SchemaMappingView* primary_schema(const SchemaSetView& schemas) noexcept;
std::string render_all_schemas(const SchemaSetView& schemas);

} // namespace tgcli::cli
