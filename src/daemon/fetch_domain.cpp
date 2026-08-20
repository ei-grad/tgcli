#include "daemon/fetch_domain.hpp"

#include "daemon/message_summary.hpp"

#include <array>
#include <ctime>
#include <limits>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

} // namespace

bool valid_fetch_target(const FetchTarget& target) {
    return (!target.limit || (*target.limit >= 1 && *target.limit <= kMaximumFetchLimit)) &&
           !(target.all && target.limit) && (target.limit || target.since || target.all);
}

bool finite_fetch_target(const FetchTarget& target) {
    return target.limit.has_value() || target.since.has_value();
}

std::optional<std::string> format_fetch_timestamp(std::int32_t seconds) {
    const std::time_t value = seconds;
    std::tm utc{};
    if (gmtime_r(&value, &utc) == nullptr) {
        return std::nullopt;
    }
    std::array<char, 21> rendered{};
    if (std::strftime(rendered.data(), rendered.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return std::nullopt;
    }
    return std::string(rendered.data());
}

nlohmann::json fetch_target_json(const FetchTarget& target) {
    const auto since = target.since ? format_fetch_timestamp(*target.since) : std::nullopt;
    return {{"limit", target.limit ? nlohmann::json(*target.limit) : nlohmann::json(nullptr)},
            {"all", target.all},
            {"since", since ? nlohmann::json(*since) : nlohmann::json(nullptr)}};
}

FetchScanResult scan_fetch_page(const core::TdMessages& page, const FetchScanInput& input) {
    FetchScanResult result;
    if (!valid_int53(input.chat_id) ||
        (input.exclusive_anchor && !valid_int53(*input.exclusive_anchor)) ||
        (input.since_cutoff_message_id && !valid_int53(*input.since_cutoff_message_id)) ||
        page.total_count < 0 || static_cast<std::size_t>(page.total_count) < page.messages.size()) {
        result.error = FetchScanError::Internal;
        return result;
    }

    std::size_t begin = 0;
    if (input.exclusive_anchor && !page.messages.empty() && page.messages.front() &&
        // Both optionals are established by this condition before their values are inspected.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        page.messages.front()->id == *input.exclusive_anchor) {
        begin = 1;
    }

    bool has_null = false;
    bool has_message = false;
    for (std::size_t index = begin; index < page.messages.size(); ++index) {
        has_null = has_null || !page.messages[index].has_value();
        has_message = has_message || page.messages[index].has_value();
    }
    if (has_null && has_message) {
        result.error = FetchScanError::Internal;
        return result;
    }
    if (!has_message) {
        return result;
    }

    bool has_previous = input.exclusive_anchor.has_value();
    std::int64_t previous = input.exclusive_anchor.value_or(0);
    for (std::size_t index = begin; index < page.messages.size(); ++index) {
        // The page-wide null classification above proves every scanned position is populated.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const auto& raw = *page.messages[index];
        const auto message = materialize_message_summary(raw);
        if (!message || message->chat_id != input.chat_id) {
            result.error = FetchScanError::Internal;
            return result;
        }
        if (has_previous && message->id >= previous) {
            result.error = FetchScanError::NonAdvancing;
            return result;
        }
        previous = message->id;
        has_previous = true;
        result.oldest_message_id = message->id;
        result.since_anchor_observed =
            result.since_anchor_observed ||
            (input.since_cutoff_message_id && message->id == *input.since_cutoff_message_id);
        ++result.added_count;
    }
    if (input.cached_count > std::numeric_limits<std::uint64_t>::max() - result.added_count) {
        result = {};
        result.error = FetchScanError::Overflow;
    }
    return result;
}

std::string_view fetch_stop_reason_name(FetchStopReason reason) {
    switch (reason) {
    case FetchStopReason::TargetReached:
        return "target_reached";
    case FetchStopReason::SinceAnchorReached:
        return "since_anchor_reached";
    case FetchStopReason::TdlibIdle:
        return "tdlib_idle";
    }
    return {};
}

std::optional<nlohmann::json> make_fetch_result(std::int64_t chat_id, std::uint64_t cached_count,
                                                std::optional<std::int64_t> oldest_message_id,
                                                const FetchTarget& target,
                                                const FetchCompletion& completion) {
    if (!valid_int53(chat_id) || !valid_fetch_target(target) ||
        (target.since && !format_fetch_timestamp(*target.since)) ||
        ((cached_count == 0) != !oldest_message_id) ||
        (oldest_message_id && !valid_int53(*oldest_message_id)) ||
        (completion.numeric_latched !=
         (target.limit && cached_count >= static_cast<std::uint64_t>(*target.limit))) ||
        (completion.since_latched && !target.since) || !completion.local_boundary_sealed) {
        return std::nullopt;
    }

    std::optional<bool> target_reached;
    switch (completion.stop_reason) {
    case FetchStopReason::TargetReached:
        if (cached_count == 0 || !oldest_message_id || !completion.numeric_latched ||
            completion.since_latched ||
            (completion.network_fill_started != completion.terminal_page_advanced)) {
            return std::nullopt;
        }
        target_reached = true;
        break;
    case FetchStopReason::SinceAnchorReached:
        if (cached_count == 0 || !oldest_message_id || !completion.since_latched ||
            (completion.network_fill_started != completion.terminal_page_advanced)) {
            return std::nullopt;
        }
        target_reached = true;
        break;
    case FetchStopReason::TdlibIdle:
        if (completion.numeric_latched || completion.since_latched ||
            !completion.network_fill_started || completion.terminal_page_advanced) {
            return std::nullopt;
        }
        if (finite_fetch_target(target)) {
            target_reached = false;
        }
        break;
    }

    const auto boundary =
        oldest_message_id ? nlohmann::json(*oldest_message_id) : nlohmann::json(nullptr);
    return nlohmann::json{
        {"chat_id", chat_id},
        {"cached_count", cached_count},
        {"oldest_message_id", boundary},
        {"target", fetch_target_json(target)},
        {"target_reached",
         target_reached ? nlohmann::json(*target_reached) : nlohmann::json(nullptr)},
        {"stop_reason", fetch_stop_reason_name(completion.stop_reason)},
        {"resume_from_message_id", boundary},
    };
}

bool fetch_result_matches_runtime(const nlohmann::json& candidate, std::int64_t chat_id,
                                  std::uint64_t cached_count,
                                  std::optional<std::int64_t> oldest_message_id,
                                  const FetchTarget& target, const FetchCompletion& completion) {
    const auto expected =
        make_fetch_result(chat_id, cached_count, oldest_message_id, target, completion);
    return expected && candidate == *expected;
}

nlohmann::json make_fetch_progress(std::int64_t chat_id, std::uint64_t cached_count,
                                   std::optional<std::int64_t> oldest_message_id,
                                   const FetchTarget& target) {
    return {{"operation", "fetch"},
            {"chat_id", chat_id},
            {"cached", cached_count},
            {"target", target.limit ? nlohmann::json(*target.limit) : nlohmann::json(nullptr)},
            {"oldest_message_id",
             oldest_message_id ? nlohmann::json(*oldest_message_id) : nlohmann::json(nullptr)}};
}

} // namespace tgcli::daemon
