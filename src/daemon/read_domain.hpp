#pragma once

#include "core/td_runtime.hpp"
#include "daemon/message_summary.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tgcli::daemon {

inline constexpr std::int32_t kDefaultReadLimit = 20;
inline constexpr std::int32_t kMaximumReadLimit = 100;

enum class ReadTimestampBound { Since, Until };

std::optional<std::int32_t>
parse_read_timestamp(std::string_view value, ReadTimestampBound bound,
                     std::chrono::system_clock::time_point request_start);
std::optional<TopicRef> parse_read_topic(std::string_view value);

struct ReadCursor {
    std::int32_t version = 1;
    std::string operation = "read";
    std::string account;
    std::int64_t user_id = 0;
    std::int32_t limit = kDefaultReadLimit;
    std::int64_t chat_id = 0;
    std::int64_t history_chat_id = 0;
    std::optional<TopicRef> topic;
    bool local = false;
    std::optional<std::int32_t> since;
    std::optional<std::int32_t> until;
    std::optional<std::int64_t> since_cutoff_message_id;
    std::int64_t from_message_id = 0;

    bool operator==(const ReadCursor&) const = default;
};

std::string encode_read_cursor(const ReadCursor& cursor);
std::optional<ReadCursor> decode_read_cursor(std::string_view token);

enum class ReadScanError { None, Internal, NonAdvancing };

struct ReadScanInput {
    std::int64_t history_chat_id = 0;
    std::optional<TopicRef> topic;
    std::optional<std::int32_t> since;
    std::optional<std::int32_t> until;
    std::optional<std::int64_t> since_cutoff_message_id;
    std::optional<std::int64_t> exclusive_anchor;
    std::int32_t remaining = 0;
};

struct ReadScanResult {
    ReadScanError error = ReadScanError::None;
    std::vector<MessageSummary> items;
    std::optional<std::int64_t> last_consumed_message_id;
    bool reached_time_anchor = false;
};

ReadScanResult scan_read_page(const core::TdMessages& page, const ReadScanInput& input);

} // namespace tgcli::daemon
