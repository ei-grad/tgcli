#include "daemon/m6_domain.hpp"

#include "common/utf8.hpp"
#include "daemon/local_selector.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string>

namespace tgcli::daemon {

namespace {

constexpr std::array<std::string_view, 30> kFolderIcons{
    "all",   "unread", "unmuted", "bots",     "channels", "groups",  "private", "custom",
    "setup", "cat",    "crown",   "favorite", "flower",   "game",    "home",    "love",
    "mask",  "party",  "sport",   "study",    "trade",    "travel",  "work",    "airplane",
    "book",  "light",  "like",    "money",    "note",     "palette",
};

constexpr std::array<std::string_view, 6> kTopicColors{
    "blue", "yellow", "purple", "green", "pink", "red",
};

constexpr std::array<std::string_view, 16> kAdminRights{
    "change-info",     "post-messages",          "edit-messages", "delete-messages",
    "invite-users",    "restrict-members",       "pin-messages",  "manage-topics",
    "promote-members", "manage-video-chats",     "post-stories",  "edit-stories",
    "delete-stories",  "manage-direct-messages", "manage-tags",   "anonymous",
};

constexpr std::array<std::string_view, 16> kChatPermissions{
    "send-basic-messages", "send-audios",       "send-documents",    "send-photos",
    "send-videos",         "send-video-notes",  "send-voice-notes",  "send-polls",
    "send-other-messages", "add-link-previews", "react-to-messages", "edit-tag",
    "change-info",         "invite-users",      "pin-messages",      "create-topics",
};

template <typename Enum, std::size_t Size>
std::optional<Enum> parse_closed(std::string_view value,
                                 const std::array<std::string_view, Size>& names) noexcept {
    const auto found = std::ranges::find(names, value);
    if (found == names.end()) {
        return std::nullopt;
    }
    return static_cast<Enum>(std::distance(names.begin(), found));
}

template <typename Enum, std::size_t Size>
std::string_view closed_name(Enum value, const std::array<std::string_view, Size>& names) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names.at(index) : std::string_view{};
}

bool utf8_first(unsigned char byte) {
    return (byte & 0xC0U) != 0x80U;
}

void replace_all(std::string& value, std::string_view needle, std::string_view replacement) {
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
}

bool clean_input(std::string& value) {
    if (!common::valid_utf8(value)) {
        return false;
    }
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte == '\r') {
            continue;
        }
        if (byte <= 0x1FU && byte != '\n') {
            result.push_back(' ');
            continue;
        }
        if (byte == 0xE2U && index + 2 < value.size() &&
            static_cast<unsigned char>(value[index + 1]) == 0x80U) {
            const auto third = static_cast<unsigned char>(value[index + 2]);
            if (third >= 0xA8U && third <= 0xAEU) {
                index += 2;
                continue;
            }
        }
        if (byte == 0xCCU && index + 1 < value.size()) {
            const auto second = static_cast<unsigned char>(value[index + 1]);
            if (second == 0xB3U || second == 0xBFU || second == 0x8AU) {
                ++index;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    value = std::move(result);
    return true;
}

void normalize_unicode_spaces(std::string& value) {
    constexpr std::array<std::string_view, 20> spaces{
        "\xC2\xA0",     "\xE1\x9A\x80", "\xE1\xA0\x8E", "\xE2\x80\x80", "\xE2\x80\x81",
        "\xE2\x80\x82", "\xE2\x80\x83", "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86",
        "\xE2\x80\x87", "\xE2\x80\x88", "\xE2\x80\x89", "\xE2\x80\x8A", "\xE2\x80\xAF",
        "\xE2\x81\x9F", "\xE2\xA0\x80", "\xE3\x80\x80", "\xEF\xBF\xBC", "\xF3\xA0\x80\x80",
    };
    for (const auto space : spaces) {
        replace_all(value, space, " ");
    }
}

void trim_space_and_lf(std::string& value) {
    const auto whitespace = [](char character) { return character == ' ' || character == '\n'; };
    const auto first = std::ranges::find_if_not(value, whitespace);
    const auto last = std::ranges::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    if (first >= last) {
        value.clear();
        return;
    }
    value = std::string(first, last);
}

void truncate_scalars(std::string& value, std::size_t maximum) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < value.size()) {
        if (utf8_first(static_cast<unsigned char>(value[offset])) && count++ == maximum) {
            value.resize(offset);
            return;
        }
        ++offset;
    }
}

std::string canonical_text(M6TextKind kind, std::string value) {
    if (!clean_input(value)) {
        return {};
    }
    normalize_unicode_spaces(value);
    trim_space_and_lf(value);
    std::size_t maximum = 128;
    switch (kind) {
    case M6TextKind::FolderName:
        maximum = 12;
        break;
    case M6TextKind::ChatDescription:
        maximum = 255;
        break;
    case M6TextKind::TopicName:
    case M6TextKind::ChatTitle:
        break;
    }
    truncate_scalars(value, maximum);
    trim_space_and_lf(value);
    if (kind == M6TextKind::ChatDescription) {
        return value;
    }

    std::string result;
    result.reserve(value.size());
    bool previous_space = false;
    for (const char character : value) {
        if (character == ' ' || character == '\n') {
            if (!previous_space) {
                result.push_back(' ');
                previous_space = true;
            }
            continue;
        }
        result.push_back(character);
        previous_space = false;
    }
    trim_space_and_lf(result);
    return result;
}

bool control_scalar(std::uint32_t codepoint) {
    return codepoint <= 0x1FU || (codepoint >= 0x7FU && codepoint <= 0x9FU);
}

bool contains_control_scalar(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 1;
        std::uint32_t codepoint = first;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0U) {
            length = 4;
            codepoint = first & 0x07U;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            codepoint = (codepoint << 6U) |
                        (static_cast<unsigned char>(value[index + continuation]) & 0x3FU);
        }
        if (control_scalar(codepoint)) {
            return true;
        }
        index += length;
    }
    return false;
}

} // namespace

std::span<const std::string_view> m6_folder_icon_names() noexcept {
    return kFolderIcons;
}

std::span<const std::string_view> m6_topic_color_names() noexcept {
    return kTopicColors;
}

std::span<const std::string_view> m6_admin_right_names() noexcept {
    return kAdminRights;
}

std::span<const std::string_view> m6_chat_permission_names() noexcept {
    return kChatPermissions;
}

std::optional<M6FolderIcon> parse_m6_folder_icon(std::string_view value) noexcept {
    return parse_closed<M6FolderIcon>(value, kFolderIcons);
}

std::optional<M6TopicColor> parse_m6_topic_color(std::string_view value) noexcept {
    return parse_closed<M6TopicColor>(value, kTopicColors);
}

std::optional<M6AdminRight> parse_m6_admin_right(std::string_view value) noexcept {
    return parse_closed<M6AdminRight>(value, kAdminRights);
}

std::optional<M6ChatPermission> parse_m6_chat_permission(std::string_view value) noexcept {
    return parse_closed<M6ChatPermission>(value, kChatPermissions);
}

std::string_view m6_folder_icon_name(M6FolderIcon value) noexcept {
    return closed_name(value, kFolderIcons);
}

std::string_view m6_topic_color_name(M6TopicColor value) noexcept {
    return closed_name(value, kTopicColors);
}

std::string_view m6_admin_right_name(M6AdminRight value) noexcept {
    return closed_name(value, kAdminRights);
}

std::string_view m6_chat_permission_name(M6ChatPermission value) noexcept {
    return closed_name(value, kChatPermissions);
}

bool valid_m6_canonical_text(M6TextKind kind, std::string_view value) {
    if (!common::valid_utf8(value) || value.find('\0') != std::string_view::npos) {
        return false;
    }
    const auto canonical = canonical_text(kind, std::string(value));
    const bool permits_empty = kind == M6TextKind::ChatDescription;
    return canonical == value && (permits_empty || !canonical.empty());
}

bool valid_m6_exact_selector(std::string_view value) {
    const auto selector = classify_local_selector(value);
    if (!selector) {
        return false;
    }
    return selector->kind == LocalSelectorKind::Numeric ||
           selector->kind == LocalSelectorKind::Username ||
           selector->kind == LocalSelectorKind::PublicChatLink;
}

std::optional<std::int32_t> parse_m6_positive_int32(std::string_view value) noexcept {
    if (value.empty() || value.front() < '1' || value.front() > '9' ||
        !std::ranges::all_of(value,
                             [](char character) { return character >= '0' && character <= '9'; })) {
        return std::nullopt;
    }
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(parsed);
}

bool valid_m6_contact_query(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 256 && value.find('\0') == std::string_view::npos &&
           common::valid_utf8(value);
}

bool valid_m6_invite_link(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 4'096 && value.find('\0') == std::string_view::npos &&
           common::valid_utf8(value) && !contains_control_scalar(value);
}

} // namespace tgcli::daemon
