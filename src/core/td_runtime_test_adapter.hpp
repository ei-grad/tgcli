#pragma once

#include "core/td_runtime.hpp"

#include <optional>
#include <string_view>

namespace tgcli::core::detail {

std::optional<AuthStateData>
convert_production_authorization_state_for_test(const TdValue& object,
                                                bool authorization_state_response);

std::string_view process_log_failure_message_for_test(bool json);
void reset_process_log_failure_for_test(bool json);
void report_process_log_failure_for_test();

} // namespace tgcli::core::detail
