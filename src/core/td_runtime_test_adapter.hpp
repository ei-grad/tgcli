#pragma once

#include "core/td_runtime.hpp"

#include <optional>

namespace tgcli::core::detail {

std::optional<AuthStateData>
convert_production_authorization_state_for_test(const TdValue& object,
                                                bool authorization_state_response);

} // namespace tgcli::core::detail
