#include "daemon/saved_commands.hpp"

#include "common/exit_codes.hpp"
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

enum class ReadyChangeKind { None, Advanced, Lost };

struct ReadyChange {
    ReadyChangeKind kind = ReadyChangeKind::None;
    std::shared_ptr<const AuthStateSnapshot> snapshot;
};

class AuthTracker {
  public:
    AuthTracker(core::TdClient& client, RequestSession& session)
        : client_(client), session_(session) {
        subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
                observe(snapshot);
            });
        observe(client_.auth_state());
    }

    ~AuthTracker() {
        client_.unsubscribe_auth_states(subscription_);
    }

    AuthTracker(const AuthTracker&) = delete;
    AuthTracker& operator=(const AuthTracker&) = delete;
    AuthTracker(AuthTracker&&) = delete;
    AuthTracker& operator=(AuthTracker&&) = delete;

    std::shared_ptr<const AuthStateSnapshot> current() const {
        const std::lock_guard lock(mutex_);
        return latest_;
    }

    ReadyChange ready_change_after(const AuthStateSnapshot& sent) {
        observe(client_.auth_state());
        const std::lock_guard lock(mutex_);
        const auto after_sent = [&](const auto& candidate) {
            return candidate && std::tie(candidate->client_generation, candidate->auth_sequence) >
                                    std::tie(sent.client_generation, sent.auth_sequence);
        };
        const auto lost = std::ranges::find_if(pending_, [&](const auto& candidate) {
            return after_sent(candidate) && candidate->data.state != AuthState::Ready;
        });
        if (lost != pending_.end()) {
            return {ReadyChangeKind::Lost, *lost};
        }
        if (after_sent(latest_) && latest_->data.state == AuthState::Ready) {
            return {ReadyChangeKind::Advanced, latest_};
        }
        return {};
    }

  private:
    bool record(const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
        const std::lock_guard lock(mutex_);
        if (!snapshot ||
            (latest_ && std::tie(snapshot->client_generation, snapshot->auth_sequence) <=
                            std::tie(latest_->client_generation, latest_->auth_sequence))) {
            return false;
        }
        latest_ = snapshot;
        pending_.push_back(snapshot);
        return true;
    }

    void observe(const std::shared_ptr<const AuthStateSnapshot>& snapshot) {
        if (record(snapshot)) {
            session_.supersede(snapshot->client_generation, snapshot->auth_sequence);
        }
    }

    core::TdClient& client_;
    RequestSession& session_;
    mutable std::mutex mutex_;
    std::shared_ptr<const AuthStateSnapshot> latest_;
    std::deque<std::shared_ptr<const AuthStateSnapshot>> pending_;
    std::uint64_t subscription_ = 0;
};

enum class WaitKind { Response, Updated, ReadyAdvanced, TimedOut, Cancelled, Failed };

struct WaitResult {
    WaitKind kind = WaitKind::Failed;
    core::TdValue value;
    std::optional<core::TdAuthorizationFailure> authorization_failure;
    std::shared_ptr<const AuthStateSnapshot> snapshot;
};

WaitResult consume_ready_response(std::future<core::TdValue>& response,
                                  const AuthStateSnapshot& sent, AuthTracker& tracker) {
    try {
        auto value = response.get();
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost && change.snapshot &&
            change.snapshot->receive_event_sequence != 0 &&
            (value.receive_event_sequence() == 0 ||
             change.snapshot->receive_event_sequence < value.receive_event_sequence())) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        return {WaitKind::Response, std::move(value), std::nullopt, nullptr};
    } catch (const core::TdAuthorizationError& error) {
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        if (change.kind == ReadyChangeKind::Advanced) {
            return {WaitKind::ReadyAdvanced, {}, std::nullopt, change.snapshot};
        }
        return {WaitKind::Failed, {}, error.failure(), nullptr};
    } catch (const std::exception&) {
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        if (change.kind == ReadyChangeKind::Advanced) {
            return {WaitKind::ReadyAdvanced, {}, std::nullopt, change.snapshot};
        }
        return {WaitKind::Failed, {}, std::nullopt, nullptr};
    }
}

WaitResult wait_ready_response(std::future<core::TdValue>& response, const AuthStateSnapshot& sent,
                               AuthTracker& tracker, RequestSession& session) {
    for (;;) {
        if (response.wait_for(0ms) == std::future_status::ready) {
            return consume_ready_response(response, sent, tracker);
        }
        const auto change = tracker.ready_change_after(sent);
        if (change.kind == ReadyChangeKind::Lost) {
            return {WaitKind::Updated, {}, std::nullopt, change.snapshot};
        }
        if (RequestSession::Clock::now() >= session.deadline()) {
            return {WaitKind::TimedOut, {}, std::nullopt, nullptr};
        }
        if (session.cancellation_requested() &&
            session.in_flight_state() != InFlightState::Orphaned) {
            return {WaitKind::Cancelled, {}, std::nullopt, nullptr};
        }
        std::this_thread::sleep_for(1ms);
    }
}

using ReadyRead =
    std::function<std::future<core::TdValue>(const std::shared_ptr<const AuthStateSnapshot>&)>;

WaitResult wait_ready_read(const ReadyRead& start, AuthTracker& tracker, RequestSession& session,
                           std::shared_ptr<const AuthStateSnapshot>& snapshot) {
    for (;;) {
        if (RequestSession::Clock::now() >= session.deadline()) {
            return {WaitKind::TimedOut, {}, std::nullopt, nullptr};
        }
        if (!session.reserve_direct_in_flight()) {
            return {WaitKind::Cancelled, {}, std::nullopt, nullptr};
        }
        if (RequestSession::Clock::now() >= session.deadline()) {
            session.settle_in_flight();
            return {WaitKind::TimedOut, {}, std::nullopt, nullptr};
        }
        auto response = start(snapshot);
        auto waited = wait_ready_response(response, *snapshot, tracker, session);
        session.settle_in_flight();
        if (waited.kind != WaitKind::ReadyAdvanced) {
            return waited;
        }
        if (session.cancellation_requested()) {
            return {WaitKind::Cancelled, {}, std::nullopt, nullptr};
        }
        if (!waited.snapshot || waited.snapshot->data.state != AuthState::Ready) {
            return {WaitKind::Failed, {}, std::nullopt, nullptr};
        }
        snapshot = std::move(waited.snapshot);
    }
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
               AuthTracker& tracker, RequestSession& session) {
    auto snapshot = tracker.current();
    if (!snapshot || snapshot->data.state != AuthState::Ready) {
        not_authed(session, account, snapshot, "not_ready");
        return std::nullopt;
    }
    auto waited = wait_ready_read([&](const auto& current) { return client.get_me(current); },
                                  tracker, session, snapshot);
    if (waited.kind == WaitKind::Updated) {
        if (!session.cancellation_requested()) {
            not_authed(session, account, waited.snapshot, "authorization_lost");
        }
        return std::nullopt;
    }
    if (waited.kind == WaitKind::TimedOut) {
        if (!session.cancellation_requested()) {
            timeout(session, operation, tracker.current());
        }
        return std::nullopt;
    }
    if (waited.kind != WaitKind::Response || session.cancellation_requested()) {
        if (waited.kind == WaitKind::Failed && !session.cancellation_requested()) {
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

std::optional<WaitResult> send_and_wait(const ReadyRead& start,
                                        std::shared_ptr<const AuthStateSnapshot>& snapshot,
                                        std::string_view account, std::string_view operation,
                                        AuthTracker& tracker, RequestSession& session) {
    auto waited = wait_ready_read(start, tracker, session, snapshot);
    if (waited.kind == WaitKind::Updated) {
        if (!session.cancellation_requested()) {
            not_authed(session, account, waited.snapshot, "authorization_lost");
        }
        return std::nullopt;
    }
    if (waited.kind == WaitKind::TimedOut) {
        if (!session.cancellation_requested()) {
            timeout(session, operation, tracker.current());
        }
        return std::nullopt;
    }
    if (waited.kind != WaitKind::Response || session.cancellation_requested()) {
        if (waited.kind == WaitKind::Failed && !session.cancellation_requested()) {
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

bool valid_utf8(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7F) {
            length = 1;
            codepoint = first;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return false;
        }
        index += length;
    }
    return true;
}

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
    AuthTracker tracker(client_.get(), session);
    auto snapshot = user_preflight(client_.get(), account_, operation, tracker, session);
    if (!snapshot) {
        return;
    }
    auto waited = send_and_wait(
        [&](const auto& current) { return client_.get().get_saved_messages_tags(current, 0); },
        *snapshot, account_, operation, tracker, session);
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
    AuthTracker tracker(client_.get(), session);
    auto snapshot = user_preflight(client_.get(), account_, operation, tracker, session);
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
        *snapshot, account_, operation, tracker, session);
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
