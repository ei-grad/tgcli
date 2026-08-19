#pragma once

#include "core/td_runtime.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <sys/types.h>

namespace tgcli::core::detail {

TdValue convert_production_response_for_test(TdValue object);
TdValue convert_production_sessions_for_test(TdValue object);
TdValue make_production_get_active_sessions_for_test();
TdValue make_production_terminate_session_for_test(std::int64_t session_id);
TdValue make_production_get_messages_for_test(std::int64_t chat_id,
                                              std::vector<std::int64_t> message_ids);
TdValue make_production_get_message_link_for_test(std::int64_t chat_id, std::int64_t message_id,
                                                  std::int32_t media_timestamp,
                                                  std::int32_t checklist_task_id,
                                                  std::string poll_option_id, bool for_album,
                                                  bool in_message_thread);
bool production_function_matches_for_test(const TdValue& function, TdFunctionKind kind);
std::optional<std::int64_t> production_terminate_session_id_for_test(const TdValue& function);
bool production_get_messages_matches_for_test(const TdValue& function, std::int64_t chat_id,
                                              const std::vector<std::int64_t>& message_ids);
bool production_get_message_link_matches_for_test(const TdValue& function, std::int64_t chat_id,
                                                  std::int64_t message_id,
                                                  std::int32_t media_timestamp,
                                                  std::int32_t checklist_task_id,
                                                  std::string_view poll_option_id, bool for_album,
                                                  bool in_message_thread);

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
