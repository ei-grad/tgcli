#pragma once

#include <string>
#include <variant>

#include <nlohmann/json_fwd.hpp>

namespace tgcli::common {

enum class CanonicalJsonError { UnsupportedType, InvalidUtf8 };

using CanonicalJsonResult = std::variant<std::string, CanonicalJsonError>;

[[nodiscard]] CanonicalJsonResult canonical_json(const nlohmann::json& value);

} // namespace tgcli::common
