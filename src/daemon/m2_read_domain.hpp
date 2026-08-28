#pragma once

#include "core/td_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tgcli::daemon {

inline constexpr std::int32_t kDefaultSearchLimit = 20;
inline constexpr std::int32_t kMaximumSearchLimit = 100;
inline constexpr std::int32_t kDefaultMembersLimit = 50;
inline constexpr std::int32_t kMaximumMembersLimit = 200;
inline constexpr std::size_t kMaximumSearchRawScannedItems = 4'096;
inline constexpr std::size_t kMaximumSearchCursorMarkerBytes = 1'048'576;

enum class SearchScope { Chat, Global };
enum class SearchType { Any, Text, Photo, Video, Document, Link, Voice };

struct SearchRawOrder {
    std::int32_t date = 0;
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;

    bool operator==(const SearchRawOrder&) const = default;
};

struct SearchCursor {
    std::int32_t version = 1;
    std::string operation = "search";
    std::string account;
    std::int64_t user_id = 0;
    std::int32_t limit = kDefaultSearchLimit;
    std::string query;
    SearchScope scope = SearchScope::Global;
    std::optional<std::int64_t> chat_id;
    std::optional<std::int64_t> sender_user_id;
    SearchType type = SearchType::Any;
    std::optional<std::int64_t> next_offset_message_id;
    std::optional<std::string> next_offset;
    std::optional<std::int64_t> last_raw_message_id;
    std::optional<SearchRawOrder> last_raw_order;

    bool operator==(const SearchCursor&) const = default;
};

enum class MembersChatType { BasicGroup, Supergroup, Channel };
enum class MembersFilter { Recent, Administrators, Bots, Query };

struct MembersCursor {
    std::int32_t version = 1;
    std::string operation = "chat_members";
    std::string account;
    std::int64_t user_id = 0;
    std::int32_t limit = kDefaultMembersLimit;
    std::int64_t chat_id = 0;
    MembersChatType chat_type = MembersChatType::BasicGroup;
    std::int64_t source_id = 0;
    MembersFilter filter = MembersFilter::Recent;
    std::optional<std::string> query;
    std::int32_t offset = 0;
    std::optional<std::int32_t> source_count;

    bool operator==(const MembersCursor&) const = default;
};

std::optional<SearchType> parse_search_type(std::string_view value);
std::string_view search_type_name(SearchType value);
core::TdSearchMessagesFilter td_search_filter(SearchType value);
bool search_postfilter(SearchType type, core::TdMessageContentKind content);

std::optional<MembersFilter> parse_members_filter(std::string_view value);
std::string_view members_filter_name(MembersFilter value);
std::optional<core::TdSupergroupMembersFilter> td_members_filter(MembersFilter value);

bool pinned_search_input(std::string_view value);

std::string encode_search_cursor(const SearchCursor& cursor);
std::optional<SearchCursor> decode_search_cursor(std::string_view token);
std::string encode_members_cursor(const MembersCursor& cursor);
std::optional<MembersCursor> decode_members_cursor(std::string_view token);

} // namespace tgcli::daemon
