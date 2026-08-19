#pragma once

#include "core/td_runtime.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <sys/types.h>

namespace tgcli::core::detail {

TdValue convert_production_response_for_test(TdValue object);

std::optional<AuthStateData>
convert_production_authorization_state_for_test(const TdValue& object,
                                                bool authorization_state_response);

std::string_view process_log_failure_message_for_test(bool json);
void reset_process_log_failure_for_test(bool json);
void report_process_log_failure_for_test();
using ProcessLogWriteFunction = ssize_t (*)(void* context, int fd, const void* bytes,
                                            std::size_t size) noexcept;
void report_process_log_failure_with_writer_for_test(ProcessLogWriteFunction writer, void* context);

} // namespace tgcli::core::detail
