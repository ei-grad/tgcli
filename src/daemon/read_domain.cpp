#include "daemon/read_domain.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool ascii_digit(char value) {
    return value >= '0' && value <= '9';
}

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_user_id(std::int64_t value) {
    return value > 0 && value <= kMaximumInt53;
}

std::optional<std::int64_t> integer64(const json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    if (value.is_number_unsigned()) {
        const auto parsed = value.get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(parsed);
    }
    return value.get<std::int64_t>();
}

bool exact_fields(const json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    return std::ranges::all_of(expected,
                               [&](const std::string& name) { return value.contains(name); });
}

std::optional<int> fixed_decimal(std::string_view value, std::size_t offset, std::size_t count) {
    if (offset + count > value.size()) {
        return std::nullopt;
    }
    int result = 0;
    for (const char digit : value.substr(offset, count)) {
        if (!ascii_digit(digit)) {
            return std::nullopt;
        }
        result = result * 10 + (digit - '0');
    }
    return result;
}

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int month_days(int year, int month) {
    constexpr std::array<int, 13> days{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && leap_year(year)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 0;
    }
    return days.at(static_cast<std::size_t>(month));
}

// Days since 1970-01-01 in the proleptic Gregorian calendar.
std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= static_cast<int>(month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153U * (month > 2 ? month - 3 : month + 9) + 2U) / 5U + day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

struct CalendarDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

std::optional<CalendarDate> parse_date(std::string_view value) {
    if (value.size() < 10 || value[4] != '-' || value[7] != '-') {
        return std::nullopt;
    }
    const auto year = fixed_decimal(value, 0, 4);
    const auto month = fixed_decimal(value, 5, 2);
    const auto day = fixed_decimal(value, 8, 2);
    if (!year || !month || !day || *year == 0 || *month < 1 || *month > 12 || *day < 1 ||
        *day > month_days(*year, *month)) {
        return std::nullopt;
    }
    return CalendarDate{*year, *month, *day};
}

std::optional<std::int64_t> checked_add(std::int64_t left, std::int64_t right) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return std::nullopt;
    }
    return left + right;
}

std::optional<std::int64_t> checked_multiply(std::int64_t left, std::int64_t right) {
    if (left < 0 || right <= 0 || left > std::numeric_limits<std::int64_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

std::optional<std::int64_t> relative_seconds(std::string_view value) {
    if (value.size() < 2 || value.front() < '1' || value.front() > '9') {
        return std::nullopt;
    }
    const char unit = value.back();
    std::int64_t multiplier = 0;
    if (unit == 'm') {
        multiplier = 60;
    } else if (unit == 'h') {
        multiplier = 3600;
    } else if (unit == 'd') {
        multiplier = 86400;
    } else {
        return std::nullopt;
    }
    const auto digits = value.substr(0, value.size() - 1);
    if (!std::ranges::all_of(digits, ascii_digit)) {
        return std::nullopt;
    }
    std::int64_t amount = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), amount);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return checked_multiply(amount, multiplier);
}

// The closed timestamp grammar keeps validation and exact bound rounding in one place.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<std::int64_t> parse_absolute_timestamp(std::string_view value,
                                                     ReadTimestampBound bound) {
    const auto date = parse_date(value);
    if (!date) {
        return std::nullopt;
    }
    const auto day_start = days_from_civil(date->year, static_cast<unsigned>(date->month),
                                           static_cast<unsigned>(date->day)) *
                           86400;
    if (value.size() == 10) {
        return bound == ReadTimestampBound::Since ? day_start : day_start + 86399;
    }
    if (value.size() < 20 || value[10] != 'T' || value[13] != ':' || value[16] != ':') {
        return std::nullopt;
    }
    const auto hour = fixed_decimal(value, 11, 2);
    const auto minute = fixed_decimal(value, 14, 2);
    const auto second = fixed_decimal(value, 17, 2);
    if (!hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    std::size_t offset = 19;
    bool nonzero_fraction = false;
    if (offset < value.size() && value[offset] == '.') {
        ++offset;
        const auto fraction_start = offset;
        while (offset < value.size() && ascii_digit(value[offset])) {
            nonzero_fraction = nonzero_fraction || value[offset] != '0';
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
        const auto zone_hour = fixed_decimal(value, offset + 1, 2);
        const auto zone_minute = fixed_decimal(value, offset + 4, 2);
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
    const auto local_seconds = day_start + static_cast<std::int64_t>(*hour) * 3600 +
                               static_cast<std::int64_t>(*minute) * 60 + *second;
    auto base = checked_add(local_seconds, -zone_seconds);
    if (!base) {
        return std::nullopt;
    }
    if (bound == ReadTimestampBound::Since && nonzero_fraction) {
        base = checked_add(*base, 1);
    }
    return base;
}

std::string base64url_encode(std::string_view input) {
    std::string output;
    output.reserve((input.size() * 4 + 2) / 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const unsigned char byte : input) {
        accumulator = (accumulator << 8U) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(kBase64Alphabet[(accumulator >> bits) & 0x3FU]);
        }
    }
    if (bits != 0) {
        output.push_back(kBase64Alphabet[(accumulator << (6 - bits)) & 0x3FU]);
    }
    return output;
}

std::optional<std::string> base64url_decode(std::string_view input) {
    if (input.empty() || input.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string output;
    output.reserve(input.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char character : input) {
        const auto found = kBase64Alphabet.find(character);
        if (found == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(found);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    if (bits != 0 && (accumulator & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }
    return output;
}

std::optional<TopicRef> topic_from_json(const json& value) {
    if (!exact_fields(value, {"id", "kind"}) || !value["kind"].is_string()) {
        return std::nullopt;
    }
    const auto id = integer64(value["id"]);
    if (!id || !valid_user_id(*id)) {
        return std::nullopt;
    }
    TopicKind kind = TopicKind::Forum;
    const auto& name = value["kind"].get_ref<const std::string&>();
    if (name == "forum") {
        if (*id > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
    } else if (name == "thread") {
        kind = TopicKind::Thread;
    } else if (name == "direct") {
        kind = TopicKind::Direct;
    } else if (name == "saved") {
        kind = TopicKind::Saved;
    } else {
        return std::nullopt;
    }
    return TopicRef{kind, *id};
}

bool topic_matches(const std::optional<TopicRef>& actual, const std::optional<TopicRef>& expected) {
    return !expected || (actual && *actual == *expected);
}

} // namespace

std::optional<std::int32_t>
parse_read_timestamp(std::string_view value, ReadTimestampBound bound,
                     std::chrono::system_clock::time_point request_start) {
    std::optional<std::int64_t> parsed;
    if (const auto delta = relative_seconds(value)) {
        const auto elapsed = request_start.time_since_epoch();
        const auto floor_seconds = std::chrono::floor<std::chrono::seconds>(elapsed);
        const bool has_fraction = elapsed != floor_seconds;
        parsed = checked_add(floor_seconds.count(), -*delta);
        if (parsed && bound == ReadTimestampBound::Since && has_fraction) {
            parsed = checked_add(*parsed, 1);
        }
    } else {
        parsed = parse_absolute_timestamp(value, bound);
    }
    if (!parsed || *parsed < std::numeric_limits<std::int32_t>::min() ||
        *parsed > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*parsed);
}

std::optional<TopicRef> parse_read_topic(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    TopicKind kind = TopicKind::Forum;
    auto digits = value;
    const auto colon = value.find(':');
    if (colon != std::string_view::npos) {
        const auto prefix = value.substr(0, colon);
        digits = value.substr(colon + 1);
        if (prefix == "forum") {
            kind = TopicKind::Forum;
        } else if (prefix == "thread") {
            kind = TopicKind::Thread;
        } else if (prefix == "direct") {
            kind = TopicKind::Direct;
        } else if (prefix == "saved") {
            kind = TopicKind::Saved;
        } else {
            return std::nullopt;
        }
    }
    if (digits.empty() || digits.front() < '1' || digits.front() > '9' ||
        !std::ranges::all_of(digits, ascii_digit)) {
        return std::nullopt;
    }
    std::int64_t id = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (error != std::errc{} || end != digits.data() + digits.size() || !valid_user_id(id) ||
        (kind == TopicKind::Forum && id > std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    return TopicRef{kind, id};
}

std::string encode_read_cursor(const ReadCursor& cursor) {
    const json value{
        {"account", cursor.account},
        {"chat_id", cursor.chat_id},
        {"from_message_id", cursor.from_message_id},
        {"history_chat_id", cursor.history_chat_id},
        {"limit", cursor.limit},
        {"local", cursor.local},
        {"operation", cursor.operation},
        {"since", cursor.since ? json(*cursor.since) : json(nullptr)},
        {"since_cutoff_message_id",
         cursor.since_cutoff_message_id ? json(*cursor.since_cutoff_message_id) : json(nullptr)},
        {"topic", cursor.topic ? topic_ref_json(*cursor.topic) : json(nullptr)},
        {"until", cursor.until ? json(*cursor.until) : json(nullptr)},
        {"user_id", cursor.user_id},
        {"version", cursor.version},
    };
    return base64url_encode(value.dump());
}

// Cursor acceptance is intentionally a single closed structural and semantic check.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<ReadCursor> decode_read_cursor(std::string_view token) {
    const auto decoded = base64url_decode(token);
    if (!decoded) {
        return std::nullopt;
    }
    const auto value = json::parse(*decoded, nullptr, false);
    if (value.is_discarded() ||
        !exact_fields(value, {"account", "chat_id", "from_message_id", "history_chat_id", "limit",
                              "local", "operation", "since", "since_cutoff_message_id", "topic",
                              "until", "user_id", "version"}) ||
        !value["account"].is_string() || !value["operation"].is_string() ||
        !value["local"].is_boolean()) {
        return std::nullopt;
    }
    const auto version = integer64(value["version"]);
    const auto user_id = integer64(value["user_id"]);
    const auto limit = integer64(value["limit"]);
    const auto chat_id = integer64(value["chat_id"]);
    const auto history_chat_id = integer64(value["history_chat_id"]);
    const auto from_message_id = integer64(value["from_message_id"]);
    if (!version || !user_id || !limit || !chat_id || !history_chat_id || !from_message_id ||
        *version < std::numeric_limits<std::int32_t>::min() ||
        *version > std::numeric_limits<std::int32_t>::max() ||
        *limit < std::numeric_limits<std::int32_t>::min() ||
        *limit > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    ReadCursor cursor;
    cursor.version = static_cast<std::int32_t>(*version);
    cursor.operation = value["operation"].get<std::string>();
    cursor.account = value["account"].get<std::string>();
    cursor.user_id = *user_id;
    cursor.limit = static_cast<std::int32_t>(*limit);
    cursor.chat_id = *chat_id;
    cursor.history_chat_id = *history_chat_id;
    cursor.local = value["local"].get<bool>();
    cursor.from_message_id = *from_message_id;
    if (!value["topic"].is_null()) {
        cursor.topic = topic_from_json(value["topic"]);
        if (!cursor.topic) {
            return std::nullopt;
        }
    }
    const auto parse_optional_int32 = [&](std::string_view name,
                                          std::optional<std::int32_t>& output) {
        const auto& field = value[std::string(name)];
        if (field.is_null()) {
            return true;
        }
        const auto parsed = integer64(field);
        if (!parsed || *parsed < std::numeric_limits<std::int32_t>::min() ||
            *parsed > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        output = static_cast<std::int32_t>(*parsed);
        return true;
    };
    if (!parse_optional_int32("since", cursor.since) ||
        !parse_optional_int32("until", cursor.until)) {
        return std::nullopt;
    }
    if (!value["since_cutoff_message_id"].is_null()) {
        cursor.since_cutoff_message_id = integer64(value["since_cutoff_message_id"]);
        if (!cursor.since_cutoff_message_id || !valid_int53(*cursor.since_cutoff_message_id)) {
            return std::nullopt;
        }
    }
    const bool thread = cursor.topic && cursor.topic->kind == TopicKind::Thread;
    if (cursor.version != 1 || cursor.operation.empty() || cursor.account.empty() ||
        !valid_user_id(cursor.user_id) || cursor.limit < 1 || cursor.limit > kMaximumReadLimit ||
        !valid_int53(cursor.chat_id) || !valid_int53(cursor.history_chat_id) ||
        !valid_int53(cursor.from_message_id) ||
        (cursor.since && cursor.until && *cursor.since > *cursor.until) ||
        (!cursor.since && cursor.since_cutoff_message_id) ||
        (cursor.local && cursor.since_cutoff_message_id) ||
        ((!thread || cursor.local) && cursor.history_chat_id != cursor.chat_id) ||
        encode_read_cursor(cursor) != token) {
        return std::nullopt;
    }
    return cursor;
}

// Raw-page integrity, filtering, and progress must share one ordered scan.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ReadScanResult scan_read_page(const core::TdMessages& page, const ReadScanInput& input) {
    ReadScanResult result;
    if (!valid_int53(input.history_chat_id) ||
        (input.exclusive_anchor && !valid_int53(*input.exclusive_anchor)) ||
        (input.since_cutoff_message_id && !valid_int53(*input.since_cutoff_message_id)) ||
        input.remaining <= 0 || input.remaining > kMaximumReadLimit || page.total_count < 0 ||
        static_cast<std::size_t>(page.total_count) < page.messages.size()) {
        result.error = ReadScanError::Internal;
        return result;
    }
    std::size_t begin = 0;
    if (input.exclusive_anchor && !page.messages.empty() && page.messages.front() &&
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
        result.error = ReadScanError::Internal;
        return result;
    }
    if (!has_message) {
        return result;
    }

    std::optional<std::int64_t> previous = input.exclusive_anchor;
    for (std::size_t index = begin; index < page.messages.size(); ++index) {
        const auto& raw = *page.messages[index];
        const auto message = materialize_message_summary(raw);
        if (!message || message->chat_id != input.history_chat_id) {
            result.error = ReadScanError::Internal;
            return result;
        }
        if (previous && message->id >= *previous) {
            result.error = ReadScanError::NonAdvancing;
            return result;
        }
        previous = message->id;
        if (static_cast<std::int32_t>(result.items.size()) >= input.remaining) {
            continue;
        }
        result.last_consumed_message_id = message->id;
        if (input.since_cutoff_message_id && message->id == *input.since_cutoff_message_id) {
            result.reached_time_anchor = true;
            break;
        }
        if (!topic_matches(message->topic, input.topic)) {
            continue;
        }
        if ((input.since || input.until) && raw.date == 0) {
            continue;
        }
        if (input.since && raw.date < *input.since) {
            continue;
        }
        if (input.until && raw.date > *input.until) {
            continue;
        }
        result.items.push_back(*message);
    }
    return result;
}

} // namespace tgcli::daemon
