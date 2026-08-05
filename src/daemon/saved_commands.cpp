#include "daemon/saved_commands.hpp"

#include "common/exit_codes.hpp"
#include "daemon/ready_read.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;
using core::AuthState;
using core::AuthStateSnapshot;
using nlohmann::json;

constexpr std::string_view kCursorPrefix = "tgcli.saved.v1.";
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

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

bool exact_fields(const json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    return std::ranges::all_of(expected,
                               [&](const std::string& name) { return value.contains(name); });
}

std::int32_t retry_after(std::string_view message) {
    static const std::regex pattern(
        R"((?:^|[^[:alnum:]_])(?:retry[[:space:]]+after[[:space:]]*|FLOOD_WAIT_)([0-9]+))",
        std::regex::icase);
    std::cmatch match;
    if (!std::regex_search(message.begin(), message.end(), match, pattern) || match.size() != 2) {
        return 0;
    }
    std::int32_t result = 0;
    for (const char character : match[1].str()) {
        const auto digit = static_cast<std::int32_t>(character - '0');
        constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
        result = result > (maximum - digit) / 10 ? maximum : result * 10 + digit;
    }
    return result;
}

void usage(RequestSession& session, std::string_view message, const json& argument) {
    session.error("USAGE", std::string(message),
                  {{"argument", argument}, {"reason", "invalid_argument"}}, kUsage);
}

void not_authed(RequestSession& session, std::string_view account,
                const std::shared_ptr<const AuthStateSnapshot>& snapshot, std::string_view reason) {
    session.error(
        "NOT_AUTHED", "saved commands require an authenticated account",
        {{"account", account},
         {"state", snapshot ? json(core::auth_state_name(snapshot->data.state)) : json("unknown")},
         {"reason", reason}},
        kNotAuthed);
}

void timeout(RequestSession& session, std::string_view operation,
             const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
    session.error(
        "TIMEOUT", "Saved Messages request timed out",
        {{"operation", operation},
         {"state", snapshot ? json(core::auth_state_name(snapshot->data.state)) : json(nullptr)}},
        kTimeout);
}

void td_error(RequestSession& session, std::string_view operation, const core::TdError& error) {
    if (error.code == 429) {
        session.error("RATE_LIMITED", "Telegram rate limit",
                      {{"operation", operation},
                       {"tdlib_code", 429},
                       {"retry_after", retry_after(error.message)}},
                      kRateLimited);
        return;
    }
    session.error("TDLIB_ERROR", "Saved Messages TDLib request failed",
                  {{"operation", operation}, {"tdlib_code", error.code}}, kGeneric);
}

void unexpected_response(RequestSession& session, std::string_view operation) {
    session.error("INTERNAL", "Saved Messages request returned an unexpected object",
                  {{"operation", operation}, {"reason", "internal_error"}}, kGeneric);
}

void malformed_reaction(RequestSession& session, std::string_view operation,
                        const core::TdReactionType& reaction) {
    json details{{"operation", operation}, {"tdlib_type_id", reaction.tdlib_type_id}};
    if (reaction.kind == core::TdReactionKind::CustomEmoji && reaction.custom_emoji_id <= 0) {
        details["custom_emoji_id"] = reaction.custom_emoji_id;
    }
    session.error("TDLIB_ERROR", "TDLib returned an unsupported Saved Messages tag",
                  std::move(details), kGeneric);
}

std::optional<std::shared_ptr<const AuthStateSnapshot>>
user_preflight(core::TdClient& client, std::string_view account, std::string_view operation,
               ReadyReadSession& reads, RequestSession& session) {
    auto snapshot = reads.current();
    if (!snapshot || snapshot->data.state != AuthState::Ready) {
        not_authed(session, account, snapshot, "not_ready");
        return std::nullopt;
    }
    auto waited = reads.read([&](const auto& current) { return client.get_me(current); }, snapshot);
    if (waited.status == ReadyReadStatus::AuthorizationLost) {
        if (!session.cancellation_requested()) {
            not_authed(session, account, waited.snapshot, "authorization_lost");
        }
        return std::nullopt;
    }
    if (waited.status == ReadyReadStatus::TimedOut) {
        if (!session.cancellation_requested()) {
            timeout(session, operation, reads.current());
        }
        return std::nullopt;
    }
    if (waited.status != ReadyReadStatus::Response || session.cancellation_requested()) {
        if (waited.status == ReadyReadStatus::Failed && !session.cancellation_requested()) {
            unexpected_response(session, operation);
        }
        return std::nullopt;
    }
    if (const auto* error = waited.value.get_if<core::TdError>()) {
        td_error(session, operation, *error);
        return std::nullopt;
    }
    const auto* user = waited.value.get_if<core::TdUserSummary>();
    if (user == nullptr) {
        unexpected_response(session, operation);
        return std::nullopt;
    }
    if (user->is_bot) {
        session.error("BOT_UNSUPPORTED", "saved commands require a user account", json::object(),
                      kUsage);
        return std::nullopt;
    }
    return snapshot;
}

std::optional<ReadyReadResult> send_and_wait(const ReadyReadStart& start,
                                             std::shared_ptr<const AuthStateSnapshot>& snapshot,
                                             std::string_view account, std::string_view operation,
                                             ReadyReadSession& reads, RequestSession& session) {
    auto waited = reads.read(start, snapshot);
    if (waited.status == ReadyReadStatus::AuthorizationLost) {
        if (!session.cancellation_requested()) {
            not_authed(session, account, waited.snapshot, "authorization_lost");
        }
        return std::nullopt;
    }
    if (waited.status == ReadyReadStatus::TimedOut) {
        if (!session.cancellation_requested()) {
            timeout(session, operation, reads.current());
        }
        return std::nullopt;
    }
    if (waited.status != ReadyReadStatus::Response || session.cancellation_requested()) {
        if (waited.status == ReadyReadStatus::Failed && !session.cancellation_requested()) {
            unexpected_response(session, operation);
        }
        return std::nullopt;
    }
    return waited;
}

std::string reaction_selector(const core::TdReactionType& reaction) {
    if (reaction.kind == core::TdReactionKind::Emoji && !reaction.emoji.empty() &&
        valid_utf8(reaction.emoji)) {
        return reaction.emoji;
    }
    if (reaction.kind == core::TdReactionKind::CustomEmoji && reaction.custom_emoji_id > 0) {
        return "custom:" + std::to_string(reaction.custom_emoji_id);
    }
    return {};
}

std::string timestamp(std::int32_t seconds) {
    const auto value = static_cast<std::time_t>(seconds);
    std::tm utc{};
    if (::gmtime_r(&value, &utc) == nullptr) {
        return {};
    }
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
    }
    return buffer.data();
}

json message_json(const core::TdSavedMessageSummary& message) {
    return {{"id", message.id},
            {"chat_id", message.chat_id},
            {"date", timestamp(message.date)},
            {"text", message.text}};
}

bool saved_search_arguments_well_formed(const json& arguments) {
    static const std::set<std::string> fields{"query", "tag", "limit", "cursor"};
    return exact_fields(arguments, fields) &&
           (arguments["query"].is_null() || arguments["query"].is_string()) &&
           (arguments["tag"].is_null() || arguments["tag"].is_string()) &&
           (arguments["limit"].is_null() || arguments["limit"].is_number_integer()) &&
           (arguments["cursor"].is_null() || arguments["cursor"].is_string());
}

std::optional<std::string> optional_string(const json& arguments, std::string_view name) {
    const auto& value = arguments[std::string(name)];
    return value.is_string() ? std::optional<std::string>{value.get<std::string>()} : std::nullopt;
}

std::optional<SavedSearchCursor> first_saved_search_page(const json& arguments,
                                                         std::string_view account,
                                                         const std::optional<std::string>& query,
                                                         const std::optional<std::string>& tag,
                                                         RequestSession& session) {
    if (!tag) {
        usage(session, "saved search requires --tag on the first page", "--tag");
        return std::nullopt;
    }
    const auto selector = parse_saved_reaction_selector(*tag);
    if (!selector) {
        usage(session, "invalid Saved Messages reaction selector", "--tag");
        return std::nullopt;
    }
    SavedSearchCursor state;
    state.account = account;
    state.tag = selector->canonical;
    state.query = query.value_or("");
    if (!valid_utf8(state.query)) {
        usage(session, "saved search query must be valid UTF-8", "query");
        return std::nullopt;
    }
    if (!arguments["limit"].is_null()) {
        try {
            const auto limit = arguments["limit"].get<std::int64_t>();
            if (limit < 1 || limit > kMaximumSavedSearchLimit) {
                usage(session, "saved search limit must be between 1 and 100", "-n");
                return std::nullopt;
            }
            state.limit = static_cast<std::int32_t>(limit);
        } catch (const json::exception&) {
            usage(session, "saved search limit must be between 1 and 100", "-n");
            return std::nullopt;
        }
    }
    return state;
}

std::optional<SavedSearchCursor> continued_saved_search_page(
    const json& arguments, std::string_view account, const std::optional<std::string>& query,
    const std::optional<std::string>& tag, std::string_view cursor, RequestSession& session) {
    if (!arguments["limit"].is_null()) {
        usage(session, "-n is not accepted with a continuation cursor", "-n");
        return std::nullopt;
    }
    auto decoded = decode_saved_search_cursor(cursor);
    if (!decoded || decoded->account != account) {
        usage(session, "invalid Saved Messages search cursor", "--cursor");
        return std::nullopt;
    }
    if ((tag && *tag != decoded->tag) || (query && *query != decoded->query)) {
        usage(session, "cursor does not match supplied Saved Messages search filters", "--cursor");
        return std::nullopt;
    }
    return decoded;
}

std::optional<SavedSearchCursor> saved_search_state(const proto::Request& request,
                                                    std::string_view account,
                                                    RequestSession& session) {
    if (!saved_search_arguments_well_formed(request.args)) {
        usage(session, "saved search received malformed arguments", nullptr);
        return std::nullopt;
    }
    const auto query = optional_string(request.args, "query");
    const auto tag = optional_string(request.args, "tag");
    const auto cursor = optional_string(request.args, "cursor");
    if (cursor) {
        return continued_saved_search_page(request.args, account, query, tag, *cursor, session);
    }
    return first_saved_search_page(request.args, account, query, tag, session);
}

} // namespace

std::optional<SavedReactionSelector> parse_saved_reaction_selector(std::string_view selector) {
    constexpr std::string_view prefix = "custom:";
    if (selector.starts_with(prefix)) {
        const auto digits = selector.substr(prefix.size());
        if (digits.empty() || digits.front() == '0' ||
            !std::ranges::all_of(digits, [](const char character) {
                return character >= '0' && character <= '9';
            })) {
            return std::nullopt;
        }
        std::int64_t id = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
        if (error != std::errc{} || end != digits.data() + digits.size() || id <= 0) {
            return std::nullopt;
        }
        return SavedReactionSelector{std::string(selector),
                                     {.kind = core::TdReactionKind::CustomEmoji,
                                      .emoji = {},
                                      .custom_emoji_id = id,
                                      .tdlib_type_id = 0}};
    }
    if (selector.empty() || !valid_utf8(selector)) {
        return std::nullopt;
    }
    return SavedReactionSelector{std::string(selector),
                                 {.kind = core::TdReactionKind::Emoji,
                                  .emoji = std::string(selector),
                                  .custom_emoji_id = 0,
                                  .tdlib_type_id = 0}};
}

std::string encode_saved_search_cursor(const SavedSearchCursor& cursor) {
    const json value{{"version", 1},
                     {"operation", cursor.operation},
                     {"account", cursor.account},
                     {"saved_messages_topic_id", cursor.saved_messages_topic_id},
                     {"tag", cursor.tag},
                     {"query", cursor.query},
                     {"limit", cursor.limit},
                     {"from_message_id", cursor.from_message_id},
                     {"offset", cursor.offset}};
    return std::string(kCursorPrefix) + base64url_encode(value.dump());
}

std::optional<SavedSearchCursor> decode_saved_search_cursor(std::string_view token) {
    if (!token.starts_with(kCursorPrefix)) {
        return std::nullopt;
    }
    const auto decoded = base64url_decode(token.substr(kCursorPrefix.size()));
    if (!decoded || !valid_utf8(*decoded)) {
        return std::nullopt;
    }
    const json value = json::parse(*decoded, nullptr, false);
    static const std::set<std::string> fields{
        "version", "operation",       "account", "saved_messages_topic_id", "tag", "query",
        "limit",   "from_message_id", "offset"};
    if (value.is_discarded() || !exact_fields(value, fields) ||
        !value["version"].is_number_integer() || value["version"] != 1 ||
        !value["operation"].is_string() || !value["account"].is_string() ||
        !value["saved_messages_topic_id"].is_number_integer() || !value["tag"].is_string() ||
        !value["query"].is_string() || !value["limit"].is_number_integer() ||
        !value["from_message_id"].is_number_integer() || !value["offset"].is_number_integer()) {
        return std::nullopt;
    }
    SavedSearchCursor cursor;
    try {
        cursor.operation = value["operation"].get<std::string>();
        cursor.account = value["account"].get<std::string>();
        cursor.saved_messages_topic_id = value["saved_messages_topic_id"].get<std::int64_t>();
        cursor.tag = value["tag"].get<std::string>();
        cursor.query = value["query"].get<std::string>();
        cursor.limit = value["limit"].get<std::int32_t>();
        cursor.from_message_id = value["from_message_id"].get<std::int64_t>();
        cursor.offset = value["offset"].get<std::int32_t>();
    } catch (const json::exception&) {
        return std::nullopt;
    }
    const auto parsed_tag = parse_saved_reaction_selector(cursor.tag);
    if (cursor.operation != "saved.search" || cursor.saved_messages_topic_id != 0 || !parsed_tag ||
        parsed_tag->canonical != cursor.tag || !valid_utf8(cursor.query) || cursor.limit < 1 ||
        cursor.limit > kMaximumSavedSearchLimit || cursor.from_message_id <= 0 ||
        cursor.offset != 0 || encode_saved_search_cursor(cursor) != token) {
        return std::nullopt;
    }
    return cursor;
}

void SavedCoordinator::tags(const proto::Request& request, RequestSession& session) {
    if (!request.args.is_object() || !request.args.empty()) {
        usage(session, "saved tags does not accept arguments", nullptr);
        return;
    }
    constexpr std::string_view operation = "saved_tags";
    ReadyReadSession reads(client_.get(), session);
    auto snapshot = user_preflight(client_.get(), account_, operation, reads, session);
    if (!snapshot) {
        return;
    }
    auto waited = send_and_wait(
        [&](const auto& current) { return client_.get().get_saved_messages_tags(current, 0); },
        *snapshot, account_, operation, reads, session);
    if (!waited) {
        return;
    }
    if (const auto* error = waited->value.get_if<core::TdError>()) {
        td_error(session, operation, *error);
        return;
    }
    const auto* tags = waited->value.get_if<core::TdSavedMessagesTags>();
    if (tags == nullptr) {
        unexpected_response(session, operation);
        return;
    }
    json items = json::array();
    for (const auto& item : tags->tags) {
        const auto selector = reaction_selector(item.tag);
        if (selector.empty() || !valid_utf8(item.label) || item.count < 0) {
            malformed_reaction(session, operation, item.tag);
            return;
        }
        items.push_back({{"tag", selector}, {"label", item.label}, {"count", item.count}});
    }
    session.result({{"items", std::move(items)}, {"next", nullptr}});
}

void SavedCoordinator::search(const proto::Request& request, RequestSession& session) {
    auto state = saved_search_state(request, account_, session);
    if (!state) {
        return;
    }
    const auto selector = parse_saved_reaction_selector(state->tag);
    if (!selector) {
        usage(session, "invalid Saved Messages reaction selector", "--tag");
        return;
    }

    constexpr std::string_view operation = "saved_search";
    ReadyReadSession reads(client_.get(), session);
    auto snapshot = user_preflight(client_.get(), account_, operation, reads, session);
    if (!snapshot) {
        return;
    }
    core::TdSearchSavedMessagesRequest td_request{
        .saved_messages_topic_id = 0,
        .tag = selector->reaction,
        .query = state->query,
        .from_message_id = state->from_message_id,
        .offset = state->offset,
        .limit = state->limit,
    };
    auto waited = send_and_wait(
        [&](const auto& current) {
            return client_.get().search_saved_messages(current, td_request);
        },
        *snapshot, account_, operation, reads, session);
    if (!waited) {
        return;
    }
    if (const auto* error = waited->value.get_if<core::TdError>()) {
        td_error(session, operation, *error);
        return;
    }
    const auto* found = waited->value.get_if<core::TdFoundSavedMessages>();
    if (found == nullptr || found->next_from_message_id < 0) {
        unexpected_response(session, operation);
        return;
    }
    std::vector<core::TdSavedMessageSummary> messages = found->messages;
    std::stable_sort(messages.begin(), messages.end(),
                     [](const auto& left, const auto& right) { return left.id > right.id; });
    json items = json::array();
    for (const auto& message : messages) {
        if (message.id <= 0 || !valid_utf8(message.text) || timestamp(message.date).empty()) {
            unexpected_response(session, operation);
            return;
        }
        items.push_back(message_json(message));
    }
    json next = nullptr;
    if (found->next_from_message_id != 0) {
        state->from_message_id = found->next_from_message_id;
        next = encode_saved_search_cursor(*state);
    }
    session.result({{"items", std::move(items)}, {"next", std::move(next)}});
}

void register_saved_commands(Dispatcher& dispatcher, SavedCoordinator& coordinator) {
    dispatcher.register_command(
        "saved tags",
        {Tier::Read, [&coordinator](const proto::Request& request, RequestSession& session) {
             coordinator.tags(request, session);
         }});
    dispatcher.register_command(
        "saved search",
        {Tier::Read, [&coordinator](const proto::Request& request, RequestSession& session) {
             coordinator.search(request, session);
         }});
}

} // namespace tgcli::daemon
