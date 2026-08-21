#include "daemon/rate_limit.hpp"

#include <limits>
#include <regex>

namespace tgcli::daemon {

std::int32_t parse_retry_after_seconds(std::string_view message) {
    static const std::regex pattern(
        R"((?:^|[^[:alnum:]_])(?:retry[[:space:]]+after[[:space:]]*|FLOOD_WAIT_)([0-9]+))",
        std::regex::icase);
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(message.begin(), message.end(), match, pattern) || match.size() != 2) {
        return 0;
    }
    std::int32_t result = 0;
    for (const auto* character = match[1].first; character != match[1].second; ++character) {
        const auto digit = static_cast<std::int32_t>(*character - '0');
        constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
        result = result > (maximum - digit) / 10 ? maximum : result * 10 + digit;
    }
    return result;
}

} // namespace tgcli::daemon
