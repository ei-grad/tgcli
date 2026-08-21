#include "daemon/write_domain.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>

namespace tgcli::daemon {

namespace {

bool digits(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](char character) {
        return character >= '0' && character <= '9';
    });
}

std::optional<int> decimal(std::string_view value) {
    if (!digits(value)) {
        return std::nullopt;
    }
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month) {
    constexpr std::array days{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    return month == 2 && leap_year(year) ? 29 : days.at(static_cast<std::size_t>(month));
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= static_cast<int>(month <= 2);
    const auto era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const auto adjusted_month = month > 2 ? month - 3U : month + 9U;
    const auto day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const auto day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): strict timestamp grammar and bounds.
std::optional<std::int32_t> parse_rfc3339(std::string_view value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        return std::nullopt;
    }
    const auto year = decimal(value.substr(0, 4));
    const auto month = decimal(value.substr(5, 2));
    const auto day = decimal(value.substr(8, 2));
    const auto hour = decimal(value.substr(11, 2));
    const auto minute = decimal(value.substr(14, 2));
    const auto second = decimal(value.substr(17, 2));
    if (!year || !month || !day || !hour || !minute || !second || *year < 1 || *month < 1 ||
        *month > 12 || *day < 1 || *day > days_in_month(*year, *month) || *hour > 23 ||
        *minute > 59 || *second > 59) {
        return std::nullopt;
    }

    std::size_t offset = 19;
    bool fractional_nonzero = false;
    if (offset < value.size() && value[offset] == '.') {
        const auto fraction_start = ++offset;
        while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') {
            fractional_nonzero = fractional_nonzero || value[offset] != '0';
            ++offset;
        }
        if (offset == fraction_start) {
            return std::nullopt;
        }
    }

    std::int64_t zone_seconds = 0;
    if (offset < value.size() && value[offset] == 'Z') {
        ++offset;
    } else if (offset + 6 == value.size() && (value[offset] == '+' || value[offset] == '-') &&
               value[offset + 3] == ':') {
        const auto zone_hour = decimal(value.substr(offset + 1, 2));
        const auto zone_minute = decimal(value.substr(offset + 4, 2));
        if (!zone_hour || !zone_minute || *zone_hour > 23 || *zone_minute > 59) {
            return std::nullopt;
        }
        zone_seconds = static_cast<std::int64_t>(*zone_hour) * 3600 +
                       static_cast<std::int64_t>(*zone_minute) * 60;
        if (value[offset] == '-') {
            zone_seconds = -zone_seconds;
        }
        offset += 6;
    } else {
        return std::nullopt;
    }
    if (offset != value.size()) {
        return std::nullopt;
    }

    const auto unix_seconds =
        days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)) *
            86'400 +
        static_cast<std::int64_t>(*hour) * 3'600 + static_cast<std::int64_t>(*minute) * 60 +
        *second - zone_seconds + (fractional_nonzero ? 1 : 0);
    if (unix_seconds < 1 || unix_seconds > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(unix_seconds);
}

} // namespace

bool valid_send_text(std::string_view text) {
    if (text.empty() || text.find('\0') != std::string_view::npos || !common::valid_utf8(text)) {
        return false;
    }
    std::size_t scalars = 0;
    for (const auto byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xc0U) != 0x80U && ++scalars > 4096) {
            return false;
        }
    }
    return true;
}

bool valid_message_reaction(std::string_view reaction) {
    return !reaction.empty() && reaction.size() <= 64 &&
           reaction.find('\0') == std::string_view::npos && common::valid_utf8(reaction);
}

std::optional<std::int32_t> parse_mute_duration(std::string_view duration) {
    if (duration.size() < 2) {
        return std::nullopt;
    }
    std::int64_t multiplier = 0;
    switch (duration.back()) {
    case 's':
        multiplier = 1;
        break;
    case 'm':
        multiplier = 60;
        break;
    case 'h':
        multiplier = 3'600;
        break;
    case 'd':
        multiplier = 86'400;
        break;
    case 'w':
        multiplier = 604'800;
        break;
    default:
        return std::nullopt;
    }
    const auto count_text = duration.substr(0, duration.size() - 1);
    if (count_text.empty() || count_text.front() == '0' || !digits(count_text)) {
        return std::nullopt;
    }
    std::int64_t count = 0;
    const auto [end, error] =
        std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
    constexpr std::int64_t maximum = 31'622'400;
    if (error != std::errc{} || end != count_text.data() + count_text.size() || count <= 0 ||
        count > maximum / multiplier) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(count * multiplier);
}

std::optional<TopicRef> parse_send_topic(std::string_view value) {
    constexpr std::string_view forum = "forum:";
    if (value.starts_with(forum)) {
        value.remove_prefix(forum.size());
    } else if (value.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    if (value.size() > 1 && value.front() == '0') {
        return std::nullopt;
    }
    const auto parsed = decimal(value);
    if (!parsed || *parsed <= 0) {
        return std::nullopt;
    }
    return TopicRef{.kind = TopicKind::Forum, .id = *parsed};
}

std::optional<SendSchedule> parse_send_schedule(std::string_view value) {
    if (value == "online") {
        return SendSchedule{.kind = SendScheduleKind::Online, .send_date = 0};
    }
    const auto send_date = parse_rfc3339(value);
    if (!send_date) {
        return std::nullopt;
    }
    return SendSchedule{.kind = SendScheduleKind::At, .send_date = *send_date};
}

} // namespace tgcli::daemon
