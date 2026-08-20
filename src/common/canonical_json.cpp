#include "common/canonical_json.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::common {

namespace {

using nlohmann::json;

bool unsigned_bytes_less(std::string_view left, std::string_view right) {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](char lhs, char rhs) {
            return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs);
        });
}

bool append_string(std::string_view value, std::string& output, CanonicalJsonError& error) {
    if (!valid_utf8(value)) {
        error = CanonicalJsonError::InvalidUtf8;
        return false;
    }
    constexpr std::string_view digits = "0123456789abcdef";
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == static_cast<unsigned char>('"') || byte == static_cast<unsigned char>('\\')) {
            output.push_back('\\');
            output.push_back(character);
        } else if (byte <= 0x1fU) {
            output += "\\u00";
            output.push_back(digits.at(byte >> 4U));
            output.push_back(digits.at(byte & 0x0fU));
        } else {
            output.push_back(character);
        }
    }
    output.push_back('"');
    return true;
}

bool append_value(const json& value, std::string& output, CanonicalJsonError& error);

// NOLINTNEXTLINE(misc-no-recursion): traversal follows JSON nesting; callers impose size bounds.
bool append_array(const json& value, std::string& output, CanonicalJsonError& error) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value) {
        if (!std::exchange(first, false)) {
            output.push_back(',');
        }
        if (!append_value(item, output, error)) {
            return false;
        }
    }
    output.push_back(']');
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): traversal follows JSON nesting; callers impose size bounds.
bool append_object(const json& value, std::string& output, CanonicalJsonError& error) {
    std::vector<std::pair<std::string_view, const json*>> members;
    members.reserve(value.size());
    for (auto item = value.cbegin(); item != value.cend(); ++item) {
        members.emplace_back(item.key(), &item.value());
    }
    std::ranges::sort(members, [](const auto& left, const auto& right) {
        return unsigned_bytes_less(left.first, right.first);
    });
    output.push_back('{');
    bool first = true;
    for (const auto& [key, item] : members) {
        if (!std::exchange(first, false)) {
            output.push_back(',');
        }
        if (!append_string(key, output, error)) {
            return false;
        }
        output.push_back(':');
        if (!append_value(*item, output, error)) {
            return false;
        }
    }
    output.push_back('}');
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): traversal follows JSON nesting; callers impose size bounds.
bool append_value(const json& value, std::string& output, CanonicalJsonError& error) {
    if (value.is_null()) {
        output += "null";
        return true;
    }
    if (value.is_boolean()) {
        output += value.get<bool>() ? "true" : "false";
        return true;
    }
    if (value.is_number_unsigned()) {
        output += std::to_string(value.get<std::uint64_t>());
        return true;
    }
    if (value.is_number_integer()) {
        output += std::to_string(value.get<std::int64_t>());
        return true;
    }
    if (value.is_string()) {
        return append_string(value.get_ref<const std::string&>(), output, error);
    }
    if (value.is_array()) {
        return append_array(value, output, error);
    }
    if (value.is_object()) {
        return append_object(value, output, error);
    }
    error = CanonicalJsonError::UnsupportedType;
    return false;
}

} // namespace

CanonicalJsonResult canonical_json(const nlohmann::json& value) {
    std::string output;
    CanonicalJsonError error = CanonicalJsonError::UnsupportedType;
    if (!append_value(value, output, error)) {
        return error;
    }
    return output;
}

} // namespace tgcli::common
