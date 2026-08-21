#pragma once

#include <cstdint>
#include <string_view>

namespace tgcli::daemon {

[[nodiscard]] std::int32_t parse_retry_after_seconds(std::string_view message);

} // namespace tgcli::daemon
