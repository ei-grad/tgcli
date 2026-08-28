#pragma once

#include <optional>
#include <string_view>

namespace tgcli::cli {

[[nodiscard]] std::optional<std::string_view> completion_asset(std::string_view shell) noexcept;

} // namespace tgcli::cli
