#include "daemon/m2_read_domain.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool exact_fields(const json& value, const std::set<std::string>& expected) {
    return value.is_object() && value.size() == expected.size() &&
           std::ranges::all_of(expected,
                               [&](const std::string& name) { return value.contains(name); });
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

bool valid_user_id(std::int64_t value) {
    return value > 0 && value <= core::kTdInt53Max;
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

std::string_view scope_name(SearchScope scope) {
    return scope == SearchScope::Chat ? "chat" : "global";
}

std::optional<SearchScope> parse_scope(std::string_view value) {
    if (value == "chat") {
        return SearchScope::Chat;
    }
    if (value == "global") {
        return SearchScope::Global;
    }
    return std::nullopt;
}

std::string_view chat_type_name(MembersChatType type) {
    switch (type) {
    case MembersChatType::BasicGroup:
        return "basic_group";
    case MembersChatType::Supergroup:
        return "supergroup";
    case MembersChatType::Channel:
        return "channel";
    }
    return "basic_group";
}

std::optional<MembersChatType> parse_chat_type(std::string_view value) {
    if (value == "basic_group") {
        return MembersChatType::BasicGroup;
    }
    if (value == "supergroup") {
        return MembersChatType::Supergroup;
    }
    if (value == "channel") {
        return MembersChatType::Channel;
    }
    return std::nullopt;
}

std::optional<SearchRawOrder> parse_raw_order(const json& value) {
    if (!exact_fields(value, {"chat_id", "date", "message_id"})) {
        return std::nullopt;
    }
    const auto date = integer64(value["date"]);
    const auto chat_id = integer64(value["chat_id"]);
    const auto message_id = integer64(value["message_id"]);
    if (!date || !chat_id || !message_id || *date < 0 ||
        *date > std::numeric_limits<std::int32_t>::max() || !core::valid_td_chat_id(*chat_id) ||
        !core::valid_td_chat_id(*message_id)) {
        return std::nullopt;
    }
    return SearchRawOrder{
        .date = static_cast<std::int32_t>(*date), .chat_id = *chat_id, .message_id = *message_id};
}

std::string clean_pinned_input(std::string value) {
    constexpr std::size_t kLengthLimit = 35'000;
    std::size_t output_size = 0;
    for (std::size_t position = 0; position < value.size(); ++position) {
        const auto byte = static_cast<unsigned char>(value[position]);
        if (byte == '\r') {
            continue;
        }
        if (byte <= 32 && byte != '\n') {
            value[output_size++] = ' ';
        } else if (byte == 0xe2 && position + 2 < value.size() &&
                   static_cast<unsigned char>(value[position + 1]) == 0x80 &&
                   static_cast<unsigned char>(value[position + 2]) >= 0xa8 &&
                   static_cast<unsigned char>(value[position + 2]) <= 0xae) {
            position += 2;
        } else if (byte == 0xcc && position + 1 < value.size() &&
                   (static_cast<unsigned char>(value[position + 1]) == 0xb3 ||
                    static_cast<unsigned char>(value[position + 1]) == 0xbf ||
                    static_cast<unsigned char>(value[position + 1]) == 0x8a)) {
            ++position;
        } else {
            value[output_size++] = value[position];
        }
        if (output_size >= kLengthLimit - 3 &&
            (static_cast<unsigned char>(value[output_size - 1]) & 0xc0U) != 0x80U) {
            --output_size;
            break;
        }
    }
    value.resize(output_size);

    auto* bytes = reinterpret_cast<unsigned char*>(value.data());
    for (std::size_t position = 0; position + 2 < value.size(); ++position) {
        if (bytes[position] != 0xe2 || bytes[position + 1] != 0x80 ||
            (bytes[position + 2] != 0x8e && bytes[position + 2] != 0x8f)) {
            continue;
        }
        while (position + 5 < value.size() && bytes[position + 3] == 0xe2 &&
               bytes[position + 4] == 0x80 &&
               (bytes[position + 5] == 0x8e || bytes[position + 5] == 0x8f)) {
            bytes[position + 2] = 0x8c;
            position += 3;
        }
        position += 2;
    }
    return value;
}

json raw_order_json(const std::optional<SearchRawOrder>& order) {
    if (!order) {
        return nullptr;
    }
    return {{"chat_id", order->chat_id}, {"date", order->date}, {"message_id", order->message_id}};
}

} // namespace

std::optional<SearchType> parse_search_type(std::string_view value) {
    if (value == "any") {
        return SearchType::Any;
    }
    if (value == "text") {
        return SearchType::Text;
    }
    if (value == "photo") {
        return SearchType::Photo;
    }
    if (value == "video") {
        return SearchType::Video;
    }
    if (value == "doc") {
        return SearchType::Document;
    }
    if (value == "link") {
        return SearchType::Link;
    }
    if (value == "voice") {
        return SearchType::Voice;
    }
    return std::nullopt;
}

std::string_view search_type_name(SearchType value) {
    switch (value) {
    case SearchType::Any:
        return "any";
    case SearchType::Text:
        return "text";
    case SearchType::Photo:
        return "photo";
    case SearchType::Video:
        return "video";
    case SearchType::Document:
        return "doc";
    case SearchType::Link:
        return "link";
    case SearchType::Voice:
        return "voice";
    }
    return "any";
}

core::TdSearchMessagesFilter td_search_filter(SearchType value) {
    switch (value) {
    case SearchType::Any:
    case SearchType::Text:
        return core::TdSearchMessagesFilter::Any;
    case SearchType::Photo:
        return core::TdSearchMessagesFilter::Photo;
    case SearchType::Video:
        return core::TdSearchMessagesFilter::Video;
    case SearchType::Document:
        return core::TdSearchMessagesFilter::Document;
    case SearchType::Link:
        return core::TdSearchMessagesFilter::Url;
    case SearchType::Voice:
        return core::TdSearchMessagesFilter::VoiceNote;
    }
    return core::TdSearchMessagesFilter::Any;
}

bool search_postfilter(SearchType type, core::TdMessageContentKind content) {
    return type != SearchType::Text || content == core::TdMessageContentKind::Text;
}

std::optional<MembersFilter> parse_members_filter(std::string_view value) {
    if (value == "recent") {
        return MembersFilter::Recent;
    }
    if (value == "admins") {
        return MembersFilter::Administrators;
    }
    if (value == "bots") {
        return MembersFilter::Bots;
    }
    if (value == "search") {
        return MembersFilter::Query;
    }
    return std::nullopt;
}

std::string_view members_filter_name(MembersFilter value) {
    switch (value) {
    case MembersFilter::Recent:
        return "recent";
    case MembersFilter::Administrators:
        return "admins";
    case MembersFilter::Bots:
        return "bots";
    case MembersFilter::Query:
        return "search";
    }
    return "recent";
}

std::optional<core::TdSupergroupMembersFilter> td_members_filter(MembersFilter value) {
    switch (value) {
    case MembersFilter::Recent:
        return core::TdSupergroupMembersFilter::Recent;
    case MembersFilter::Administrators:
        return core::TdSupergroupMembersFilter::Administrators;
    case MembersFilter::Bots:
        return core::TdSupergroupMembersFilter::Bots;
    case MembersFilter::Query:
        return core::TdSupergroupMembersFilter::Search;
    }
    return std::nullopt;
}

bool pinned_search_input(std::string_view value) {
    if (value.empty() || !common::valid_utf8(value)) {
        return false;
    }
    return clean_pinned_input(std::string(value)) == value;
}

std::string encode_search_cursor(const SearchCursor& cursor) {
    const json value{
        {"account", cursor.account},
        {"chat_id", cursor.chat_id ? json(*cursor.chat_id) : json(nullptr)},
        {"last_raw_message_id",
         cursor.last_raw_message_id ? json(*cursor.last_raw_message_id) : json(nullptr)},
        {"last_raw_order", raw_order_json(cursor.last_raw_order)},
        {"limit", cursor.limit},
        {"next_offset", cursor.next_offset ? json(*cursor.next_offset) : json(nullptr)},
        {"next_offset_message_id",
         cursor.next_offset_message_id ? json(*cursor.next_offset_message_id) : json(nullptr)},
        {"operation", cursor.operation},
        {"query", cursor.query},
        {"scope", scope_name(cursor.scope)},
        {"sender_user_id", cursor.sender_user_id ? json(*cursor.sender_user_id) : json(nullptr)},
        {"type", search_type_name(cursor.type)},
        {"user_id", cursor.user_id},
        {"version", cursor.version},
    };
    return base64url_encode(value.dump());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed cursor grammar.
std::optional<SearchCursor> decode_search_cursor(std::string_view token) {
    const auto decoded = base64url_decode(token);
    if (!decoded || !common::valid_utf8(*decoded)) {
        return std::nullopt;
    }
    const auto value = json::parse(*decoded, nullptr, false);
    if (!exact_fields(value, {"account", "chat_id", "last_raw_message_id", "last_raw_order",
                              "limit", "next_offset", "next_offset_message_id", "operation",
                              "query", "scope", "sender_user_id", "type", "user_id", "version"}) ||
        !value["account"].is_string() || !value["operation"].is_string() ||
        !value["query"].is_string() || !value["scope"].is_string() || !value["type"].is_string()) {
        return std::nullopt;
    }
    const auto version = integer64(value["version"]);
    const auto user_id = integer64(value["user_id"]);
    const auto limit = integer64(value["limit"]);
    const auto scope = parse_scope(value["scope"].get_ref<const std::string&>());
    const auto type = parse_search_type(value["type"].get_ref<const std::string&>());
    if (!version || !user_id || !limit || !scope || !type || *version != 1 || *limit < 1 ||
        *limit > kMaximumSearchLimit || !valid_user_id(*user_id)) {
        return std::nullopt;
    }
    SearchCursor cursor;
    cursor.operation = value["operation"].get<std::string>();
    cursor.account = value["account"].get<std::string>();
    cursor.user_id = *user_id;
    cursor.limit = static_cast<std::int32_t>(*limit);
    cursor.query = value["query"].get<std::string>();
    cursor.scope = *scope;
    cursor.type = *type;
    const auto parse_optional_int = [&](std::string_view name,
                                        auto predicate) -> std::optional<std::int64_t> {
        const auto& field = value[std::string(name)];
        if (field.is_null()) {
            return std::nullopt;
        }
        const auto parsed = integer64(field);
        return parsed && predicate(*parsed) ? parsed : std::optional<std::int64_t>{};
    };
    if (!value["chat_id"].is_null()) {
        cursor.chat_id = parse_optional_int("chat_id", core::valid_td_chat_id);
        if (!cursor.chat_id) {
            return std::nullopt;
        }
    }
    if (!value["sender_user_id"].is_null()) {
        cursor.sender_user_id = parse_optional_int("sender_user_id", valid_user_id);
        if (!cursor.sender_user_id) {
            return std::nullopt;
        }
    }
    if (!value["next_offset_message_id"].is_null()) {
        cursor.next_offset_message_id = parse_optional_int(
            "next_offset_message_id", [](std::int64_t input) { return input > 0; });
        if (!cursor.next_offset_message_id || *cursor.next_offset_message_id > core::kTdInt53Max) {
            return std::nullopt;
        }
    }
    if (!value["last_raw_message_id"].is_null()) {
        cursor.last_raw_message_id =
            parse_optional_int("last_raw_message_id", core::valid_td_chat_id);
        if (!cursor.last_raw_message_id) {
            return std::nullopt;
        }
    }
    if (!value["next_offset"].is_null()) {
        if (!value["next_offset"].is_string()) {
            return std::nullopt;
        }
        cursor.next_offset = value["next_offset"].get<std::string>();
    }
    if (!value["last_raw_order"].is_null()) {
        cursor.last_raw_order = parse_raw_order(value["last_raw_order"]);
        if (!cursor.last_raw_order) {
            return std::nullopt;
        }
    }
    const bool chat_scope = cursor.scope == SearchScope::Chat;
    const bool shape_valid =
        cursor.operation == "search" && !cursor.account.empty() &&
        pinned_search_input(cursor.query) && (chat_scope == cursor.chat_id.has_value()) &&
        (chat_scope == cursor.next_offset_message_id.has_value()) &&
        (chat_scope == cursor.last_raw_message_id.has_value()) &&
        (chat_scope != cursor.next_offset.has_value()) &&
        (chat_scope != cursor.last_raw_order.has_value()) &&
        (chat_scope || (cursor.next_offset && pinned_search_input(*cursor.next_offset)));
    if (!shape_valid || encode_search_cursor(cursor) != token) {
        return std::nullopt;
    }
    return cursor;
}

std::string encode_members_cursor(const MembersCursor& cursor) {
    const json value{
        {"account", cursor.account},
        {"chat_id", cursor.chat_id},
        {"chat_type", chat_type_name(cursor.chat_type)},
        {"filter", members_filter_name(cursor.filter)},
        {"limit", cursor.limit},
        {"offset", cursor.offset},
        {"operation", cursor.operation},
        {"query", cursor.query ? json(*cursor.query) : json(nullptr)},
        {"source_count", cursor.source_count ? json(*cursor.source_count) : json(nullptr)},
        {"source_id", cursor.source_id},
        {"user_id", cursor.user_id},
        {"version", cursor.version}};
    return base64url_encode(value.dump());
}

std::optional<MembersCursor> decode_members_cursor(std::string_view token) {
    const auto decoded = base64url_decode(token);
    if (!decoded || !common::valid_utf8(*decoded)) {
        return std::nullopt;
    }
    const auto value = json::parse(*decoded, nullptr, false);
    if (!exact_fields(value,
                      {"account", "chat_id", "chat_type", "filter", "limit", "offset", "operation",
                       "query", "source_count", "source_id", "user_id", "version"}) ||
        !value["account"].is_string() || !value["chat_type"].is_string() ||
        !value["filter"].is_string() || !value["operation"].is_string()) {
        return std::nullopt;
    }
    const auto version = integer64(value["version"]);
    const auto user_id = integer64(value["user_id"]);
    const auto limit = integer64(value["limit"]);
    const auto chat_id = integer64(value["chat_id"]);
    const auto source_id = integer64(value["source_id"]);
    const auto offset = integer64(value["offset"]);
    const auto chat_type = parse_chat_type(value["chat_type"].get_ref<const std::string&>());
    const auto filter = parse_members_filter(value["filter"].get_ref<const std::string&>());
    if (!version || !user_id || !limit || !chat_id || !source_id || !offset || !chat_type ||
        !filter || *version != 1 || *limit < 1 || *limit > kMaximumMembersLimit ||
        !valid_user_id(*user_id) || !core::valid_td_chat_id(*chat_id) ||
        !valid_user_id(*source_id) || *offset < 0 ||
        *offset > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    MembersCursor cursor;
    cursor.operation = value["operation"].get<std::string>();
    cursor.account = value["account"].get<std::string>();
    cursor.user_id = *user_id;
    cursor.limit = static_cast<std::int32_t>(*limit);
    cursor.chat_id = *chat_id;
    cursor.chat_type = *chat_type;
    cursor.source_id = *source_id;
    cursor.filter = *filter;
    cursor.offset = static_cast<std::int32_t>(*offset);
    if (!value["query"].is_null()) {
        if (!value["query"].is_string()) {
            return std::nullopt;
        }
        cursor.query = value["query"].get<std::string>();
    }
    if (!value["source_count"].is_null()) {
        const auto source_count = integer64(value["source_count"]);
        if (!source_count || *source_count < 0 ||
            *source_count > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        cursor.source_count = static_cast<std::int32_t>(*source_count);
    }
    const bool is_basic = cursor.chat_type == MembersChatType::BasicGroup;
    const bool query_filter = cursor.filter == MembersFilter::Query;
    const bool shape_valid = cursor.operation == "chat_members" && !cursor.account.empty() &&
                             (is_basic == cursor.source_count.has_value()) &&
                             (!cursor.source_count || cursor.offset <= *cursor.source_count) &&
                             (query_filter == cursor.query.has_value()) &&
                             (!cursor.query || pinned_search_input(*cursor.query));
    if (!shape_valid || encode_members_cursor(cursor) != token) {
        return std::nullopt;
    }
    return cursor;
}

} // namespace tgcli::daemon
