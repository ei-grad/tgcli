#pragma once

#include "core/td_runtime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

inline constexpr std::int32_t kDefaultFetchLimit = 100;
inline constexpr std::int32_t kMaximumFetchLimit = 1'000'000;
inline constexpr std::int32_t kFetchPageLimit = 100;

struct FetchTarget {
    std::optional<std::int32_t> limit;
    bool all = false;
    std::optional<std::int32_t> since;

    bool operator==(const FetchTarget&) const = default;
};

bool valid_fetch_target(const FetchTarget& target);
bool finite_fetch_target(const FetchTarget& target);
std::optional<std::string> format_fetch_timestamp(std::int32_t seconds);
nlohmann::json fetch_target_json(const FetchTarget& target);

enum class FetchScanError { None, Internal, NonAdvancing, Overflow };

struct FetchScanInput {
    std::int64_t chat_id = 0;
    std::optional<std::int64_t> exclusive_anchor;
    std::optional<std::int64_t> since_cutoff_message_id;
    std::uint64_t cached_count = 0;
};

struct FetchScanResult {
    FetchScanError error = FetchScanError::None;
    std::uint64_t added_count = 0;
    std::optional<std::int64_t> oldest_message_id;
    bool since_anchor_observed = false;
};

FetchScanResult scan_fetch_page(const core::TdMessages& page, const FetchScanInput& input);

enum class FetchStopReason { TargetReached, SinceAnchorReached, TdlibIdle };

std::string_view fetch_stop_reason_name(FetchStopReason reason);

struct FetchCompletion {
    FetchStopReason stop_reason = FetchStopReason::TdlibIdle;
    bool numeric_latched = false;
    bool since_latched = false;
    bool local_boundary_sealed = false;
    bool network_fill_started = false;
    bool terminal_page_advanced = false;
};

std::optional<nlohmann::json> make_fetch_result(std::int64_t chat_id, std::uint64_t cached_count,
                                                std::optional<std::int64_t> oldest_message_id,
                                                const FetchTarget& target,
                                                const FetchCompletion& completion);

bool fetch_result_matches_runtime(const nlohmann::json& candidate, std::int64_t chat_id,
                                  std::uint64_t cached_count,
                                  std::optional<std::int64_t> oldest_message_id,
                                  const FetchTarget& target, const FetchCompletion& completion);

nlohmann::json make_fetch_progress(std::int64_t chat_id, std::uint64_t cached_count,
                                   std::optional<std::int64_t> oldest_message_id,
                                   const FetchTarget& target);

} // namespace tgcli::daemon
