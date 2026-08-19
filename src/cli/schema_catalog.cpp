#include "cli/schema_catalog.hpp"

#include <algorithm>

namespace tgcli::cli {

namespace {

bool ascii_whitespace(char value) {
    switch (value) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
        return true;
    default:
        return false;
    }
}

void append_schema_member(std::string& output, bool& first, std::string_view name,
                          const SchemaMappingView* schema) {
    if (schema == nullptr) {
        return;
    }
    if (!first) {
        output.push_back(',');
    }
    first = false;
    output.push_back('"');
    output.append(name);
    output += "\":";
    output.append(schema->bytes.data(), schema->bytes.size() - 1);
}

} // namespace

std::optional<std::string> normalize_schema_target(std::span<const std::string_view> tokens) {
    std::string normalized;
    for (const auto token : tokens) {
        std::size_t offset = 0;
        while (offset < token.size()) {
            while (offset < token.size() && ascii_whitespace(token[offset])) {
                ++offset;
            }
            const std::size_t begin = offset;
            while (offset < token.size() && !ascii_whitespace(token[offset])) {
                ++offset;
            }
            if (begin == offset) {
                continue;
            }
            if (!normalized.empty()) {
                normalized.push_back(' ');
            }
            normalized.append(token.substr(begin, offset - begin));
        }
    }
    if (normalized.empty()) {
        return std::nullopt;
    }
    return normalized;
}

std::string canonicalize_schema_target(std::string normalized) {
    if (normalized == "history") {
        return "read";
    }
    return normalized;
}

std::optional<SchemaSetView> find_schema_set(std::string_view canonical_command) noexcept {
    const auto mappings = embedded_schema_mappings();
    auto found = std::lower_bound(mappings.begin(), mappings.end(), canonical_command,
                                  [](const SchemaMappingView& mapping, std::string_view command) {
                                      return mapping.command < command;
                                  });
    if (found == mappings.end() || found->command != canonical_command) {
        return std::nullopt;
    }

    SchemaSetView schemas;
    for (; found != mappings.end() && found->command == canonical_command; ++found) {
        switch (found->kind) {
        case SchemaPayloadKind::Result:
            schemas.result = &*found;
            break;
        case SchemaPayloadKind::Item:
            schemas.item = &*found;
            break;
        case SchemaPayloadKind::Error:
            schemas.error = &*found;
            break;
        }
    }
    return schemas;
}

const SchemaMappingView* primary_schema(const SchemaSetView& schemas) noexcept {
    return schemas.result != nullptr ? schemas.result : schemas.item;
}

std::string render_all_schemas(const SchemaSetView& schemas) {
    std::size_t size = 3;
    for (const auto* schema : {schemas.result, schemas.item, schemas.error}) {
        if (schema != nullptr) {
            size += schema->bytes.size() + 10;
        }
    }

    std::string output;
    output.reserve(size);
    output.push_back('{');
    bool first = true;
    append_schema_member(output, first, "result", schemas.result);
    append_schema_member(output, first, "item", schemas.item);
    append_schema_member(output, first, "error", schemas.error);
    output += "}\n";
    return output;
}

} // namespace tgcli::cli
