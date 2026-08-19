#include "daemon/local_selector.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;

bool ascii_alpha(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool ascii_digit(char value) {
    return value >= '0' && value <= '9';
}

bool ascii_alnum(char value) {
    return ascii_alpha(value) || ascii_digit(value);
}

bool decimal_syntax(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    std::size_t offset = 0;
    if (value.front() == '-' || value.front() == '+') {
        offset = 1;
    }
    return offset < value.size() && std::ranges::all_of(value.substr(offset), ascii_digit);
}

std::optional<std::int64_t> parse_chat_id(std::string_view value) {
    if (value.starts_with('+')) {
        value.remove_prefix(1);
    }
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
        parsed < -kMaximumInt53 || parsed > kMaximumInt53) {
        return std::nullopt;
    }
    return parsed;
}

char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool ascii_starts_with_case_insensitive(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           std::ranges::equal(value.substr(0, prefix.size()), prefix, {}, ascii_lower, ascii_lower);
}

bool url_like(std::string_view value) {
    if (!value.empty() && ascii_alpha(value.front())) {
        std::size_t offset = 1;
        while (offset < value.size() && (ascii_alnum(value[offset]) || value[offset] == '+' ||
                                         value[offset] == '.' || value[offset] == '-')) {
            ++offset;
        }
        if (value.substr(offset).starts_with("://")) {
            return true;
        }
    }
    for (const auto host : {std::string_view("t.me"), std::string_view("www.t.me")}) {
        if (!ascii_starts_with_case_insensitive(value, host)) {
            continue;
        }
        if (value.size() == host.size()) {
            return true;
        }
        const char next = value[host.size()];
        return next == '/' || next == ':' || next == '?' || next == '#';
    }
    return false;
}

bool username(std::string_view value) {
    if (value.empty() || value.size() > 32 || !ascii_alpha(value.front()) || value.back() == '_') {
        return false;
    }
    bool previous_underscore = false;
    for (const char character : value) {
        if (!ascii_alnum(character) && character != '_') {
            return false;
        }
        if (character == '_' && previous_underscore) {
            return false;
        }
        previous_underscore = character == '_';
    }
    return true;
}

bool component_character(char value) {
    return ascii_alnum(value) || value == '_' || value == '+' || value == '-';
}

bool query_character(char value) {
    return ascii_alnum(value) || value == '_' || value == '-';
}

bool base64url(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, query_character);
}

bool positive_decimal(std::string_view value) {
    return !value.empty() && value.front() >= '1' && value.front() <= '9' &&
           std::ranges::all_of(value, ascii_digit);
}

bool decimal_component(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, ascii_digit);
}

bool valid_query_envelope(std::string_view query) {
    if (query.empty()) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < query.size()) {
        const auto end = query.find('&', offset);
        const auto pair = query.substr(offset, end == std::string_view::npos ? query.size() - offset
                                                                             : end - offset);
        if (pair.empty()) {
            return false;
        }
        const auto equals = pair.find('=');
        const auto key = pair.substr(0, equals);
        const auto value =
            equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
        if (key.empty() || !std::ranges::all_of(key, query_character) ||
            !std::ranges::all_of(value, query_character)) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        offset = end + 1;
    }
    return false;
}

std::optional<std::vector<std::string_view>> path_components(std::string_view path) {
    if (path.empty() || path.back() == '/') {
        return std::nullopt;
    }
    std::vector<std::string_view> result;
    std::size_t offset = 0;
    while (offset < path.size()) {
        const auto end = path.find('/', offset);
        const auto component = path.substr(
            offset, end == std::string_view::npos ? path.size() - offset : end - offset);
        if (component.empty() || !std::ranges::all_of(component, component_character)) {
            return std::nullopt;
        }
        result.push_back(component);
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

LocalSelector classified(LocalSelectorKind kind, std::string_view value = {}) {
    return {.kind = kind, .chat_id = 0, .value = std::string(value)};
}

LocalSelector classify_query_link(const std::vector<std::string_view>& components,
                                  std::string_view query) {
    if (components.size() != 1) {
        return classified(LocalSelectorKind::UnsupportedLink);
    }
    const auto name = components.front();
    if (!username(name)) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    if (query == "direct") {
        return classified(LocalSelectorKind::DirectMessagesChatLink, name);
    }
    if (query.starts_with("start")) {
        if (!query.starts_with("start=") ||
            !std::ranges::all_of(query.substr(6), query_character)) {
            return classified(LocalSelectorKind::InvalidLink);
        }
        return classified(LocalSelectorKind::BotStartLink, name);
    }
    return classified(LocalSelectorKind::UnsupportedLink);
}

LocalSelector classify_single_path(std::string_view value) {
    if (value.starts_with('+')) {
        if (base64url(value.substr(1))) {
            return classified(LocalSelectorKind::ChatInviteLink);
        }
        return classified(LocalSelectorKind::InvalidLink);
    }
    return username(value) ? classified(LocalSelectorKind::PublicChatLink, value)
                           : classified(LocalSelectorKind::InvalidLink);
}

LocalSelector classify_double_path(std::string_view first, std::string_view second) {
    if (first == "joinchat") {
        return base64url(second) ? classified(LocalSelectorKind::ChatInviteLink)
                                 : classified(LocalSelectorKind::InvalidLink);
    }
    const bool valid_name = username(first);
    if (!valid_name) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    if (positive_decimal(second)) {
        return classified(LocalSelectorKind::MessageLink);
    }
    if (decimal_component(second)) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    return classified(LocalSelectorKind::UnsupportedLink);
}

LocalSelector classify_private_path(std::string_view first, std::string_view second,
                                    std::string_view third) {
    if (first != "c") {
        return classified(LocalSelectorKind::UnsupportedLink);
    }
    if (positive_decimal(second) && positive_decimal(third)) {
        return classified(LocalSelectorKind::MessageLink);
    }
    return classified(LocalSelectorKind::InvalidLink);
}

LocalSelector classify_path_link(const std::vector<std::string_view>& components) {
    switch (components.size()) {
    case 1:
        return classify_single_path(components[0]);
    case 2:
        return classify_double_path(components[0], components[1]);
    case 3:
        return classify_private_path(components[0], components[1], components[2]);
    default:
        break;
    }
    return classified(LocalSelectorKind::UnsupportedLink);
}

LocalSelector classify_local_link(std::string_view link) {
    constexpr std::string_view secure_prefix = "https://t.me/";
    constexpr std::string_view bare_prefix = "t.me/";
    link.remove_prefix(link.starts_with(secure_prefix) ? secure_prefix.size() : bare_prefix.size());
    if (link.empty() || std::ranges::any_of(link, [](char value) {
            const auto byte = static_cast<unsigned char>(value);
            return byte > 0x7f || value == '%' || value == '#';
        })) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    const auto question = link.find('?');
    if (question != std::string_view::npos &&
        link.find('?', question + 1) != std::string_view::npos) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    const auto path = link.substr(0, question);
    const auto query = question == std::string_view::npos ? std::optional<std::string_view>{}
                                                          : link.substr(question + 1);
    const auto components = path_components(path);
    if (!components) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    if (query && !valid_query_envelope(query.value())) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    const auto& parsed_components = components.value();
    if (query) {
        return classify_query_link(parsed_components, query.value());
    }
    return classify_path_link(parsed_components);
}

} // namespace

std::optional<LocalSelector> classify_local_selector(std::string_view selector) {
    if (selector.empty() || !common::valid_utf8(selector)) {
        return std::nullopt;
    }
    if (decimal_syntax(selector)) {
        const auto id = parse_chat_id(selector);
        return id ? std::optional<LocalSelector>{LocalSelector{.kind = LocalSelectorKind::Numeric,
                                                               .chat_id = *id,
                                                               .value = std::string(selector)}}
                  : std::nullopt;
    }
    if (selector.starts_with('@')) {
        return selector.size() == 1 ? std::nullopt
                                    : std::optional<LocalSelector>{
                                          LocalSelector{.kind = LocalSelectorKind::Username,
                                                        .chat_id = 0,
                                                        .value = std::string(selector.substr(1))}};
    }
    if (selector.starts_with("https://t.me/") || selector.starts_with("t.me/")) {
        return classify_local_link(selector);
    }
    if (url_like(selector)) {
        return classified(LocalSelectorKind::InvalidLink);
    }
    return classified(LocalSelectorKind::Title, selector);
}

} // namespace tgcli::daemon
