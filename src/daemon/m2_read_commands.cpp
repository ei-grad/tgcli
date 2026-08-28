#include "daemon/m2_read_commands.hpp"

#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/chat_identity.hpp"
#include "daemon/chat_summary.hpp"
#include "daemon/m2_read_domain.hpp"
#include "daemon/message_summary.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

struct SearchState {
    std::string query;
    SearchScope scope = SearchScope::Global;
    std::optional<std::string> chat_selector;
    std::optional<std::string> sender_selector;
    std::optional<std::int64_t> sender_user_id;
    SearchType type = SearchType::Any;
    std::int32_t limit = kDefaultSearchLimit;
    std::optional<SearchCursor> cursor;
};

struct MembersState {
    std::string chat_selector;
    MembersFilter filter = MembersFilter::Recent;
    std::optional<std::string> query;
    std::int32_t limit = kDefaultMembersLimit;
    std::optional<MembersCursor> cursor;
};

struct MemberRow {
    core::TdMessageSender sender;
    bool is_bot = false;
    std::string display_name;
    std::vector<std::string> usernames;
    std::string status;
    std::string tag;
    std::optional<std::string> joined_at;
};

struct MemberKey {
    core::TdMessageSenderKind kind = core::TdMessageSenderKind::Unknown;
    std::int64_t id = 0;

    bool operator==(const MemberKey&) const = default;
};

struct MemberKeyHash {
    std::size_t operator()(const MemberKey& value) const noexcept {
        return std::hash<std::int64_t>{}(value.id) ^ (static_cast<std::size_t>(value.kind) << 1U);
    }
};

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

void usage(RequestSession& session, std::string_view message, const json& argument,
           std::string_view reason = "invalid_argument") {
    session.error("USAGE", std::string(message), {{"argument", argument}, {"reason", reason}},
                  kUsage);
}

void internal(RequestSession& session, std::string_view operation,
              std::string_view reason = "malformed_tdlib_response") {
    session.error("INTERNAL", std::string(operation) + " returned an unexpected object",
                  {{"operation", operation}, {"reason", reason}}, kGeneric);
}

void pagination(RequestSession& session, std::string_view operation, std::string_view reason) {
    session.error("PAGINATION_INVALID", std::string(operation) + " pagination is invalid",
                  {{"operation", operation}, {"reason", reason}}, kGeneric);
}

bool final_stop(core::TdClient& client, std::string_view operation, RequestSession& session) {
    if (session.cancellation_requested()) {
        return true;
    }
    if (!deadline_expired(session.deadline())) {
        return false;
    }
    const auto snapshot = client.auth_state();
    session.error(
        "TIMEOUT", std::string(operation) + " request timed out",
        {{"operation", operation},
         {"state", snapshot ? json(core::auth_state_name(snapshot->data.state)) : json(nullptr)}},
        kTimeout);
    return true;
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

void td_error(RequestSession& session, std::string_view operation, const core::TdError& error) {
    if (error.code == 429) {
        session.error("RATE_LIMITED", "Telegram rate limit",
                      {{"operation", operation},
                       {"tdlib_code", 429},
                       {"retry_after", retry_after(error.message)}},
                      kRateLimited);
        return;
    }
    session.error("TDLIB_ERROR", std::string(operation) + " TDLib request failed",
                  {{"operation", operation}, {"tdlib_code", error.code}}, kGeneric);
}

bool handle_read_stop(const ReadyReadResult& result, core::TdClient& client,
                      std::string_view account, std::string_view operation,
                      RequestSession& session) {
    switch (result.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        if (!session.cancellation_requested()) {
            session.error("NOT_AUTHED", std::string(operation) + " requires authentication",
                          {{"account", account},
                           {"state", result.snapshot
                                         ? json(core::auth_state_name(result.snapshot->data.state))
                                         : json("unknown")},
                           {"reason", "authorization_lost"}},
                          kNotAuthed);
        }
        return true;
    case ReadyReadStatus::TimedOut: {
        const auto snapshot = client.auth_state();
        if (!session.cancellation_requested()) {
            session.error("TIMEOUT", std::string(operation) + " request timed out",
                          {{"operation", operation},
                           {"state", snapshot ? json(core::auth_state_name(snapshot->data.state))
                                              : json(nullptr)}},
                          kTimeout);
        }
        return true;
    }
    case ReadyReadStatus::Failed:
        if (!session.cancellation_requested()) {
            internal(session, operation, "internal_error");
        }
        return true;
    case ReadyReadStatus::Cancelled:
        return true;
    }
    return true;
}

std::optional<ReadyReadResult> target_read(ResolverConsumer& resolver, core::TdClient& client,
                                           std::string_view account, std::string_view operation,
                                           RequestSession& session, const ReadyReadStart& start) {
    auto result = resolver.read_target(start);
    if (handle_read_stop(result, client, account, operation, session)) {
        return std::nullopt;
    }
    return result;
}

std::optional<ResolverPrincipal> bind_principal(ResolverConsumer& resolver, M2Operation operation,
                                                RequestSession& session) {
    const auto outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, operation);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    return std::get<ResolverPrincipal>(outcome);
}

std::optional<ResolvedChatTarget> resolve_chat(ResolverConsumer& resolver, std::string selector,
                                               M2Operation operation, RequestSession& session) {
    const auto outcome = resolver.resolve_chat(std::move(selector), ResolverScope::ActiveDialogs);
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, operation);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    return std::get<ResolvedChatTarget>(outcome);
}

std::optional<SearchState> parse_search_cursor(const json& args, RequestSession& session) {
    if (!args["chat"].is_null() || !args["from"].is_null() || args["global"].get<bool>() ||
        !args["limit"].is_null() || !args["query"].is_null() || !args["type"].is_null()) {
        usage(session, "search cursor cannot be combined with first-page arguments", "--cursor",
              "mutually_exclusive");
        return std::nullopt;
    }
    const auto cursor = decode_search_cursor(args["cursor"].get_ref<const std::string&>());
    if (!cursor) {
        usage(session, "invalid search cursor", "--cursor", "invalid_cursor");
        return std::nullopt;
    }
    return SearchState{.query = cursor->query,
                       .scope = cursor->scope,
                       .chat_selector =
                           cursor->chat_id
                               ? std::optional<std::string>{std::to_string(*cursor->chat_id)}
                               : std::nullopt,
                       .sender_selector = std::nullopt,
                       .sender_user_id = cursor->sender_user_id,
                       .type = cursor->type,
                       .limit = cursor->limit,
                       .cursor = cursor};
}

std::optional<SearchState> parse_search(const json& args, RequestSession& session) {
    if (!exact_fields(args, {"chat", "cursor", "from", "global", "limit", "query", "type"}) ||
        !(args["chat"].is_null() || args["chat"].is_string()) ||
        !(args["cursor"].is_null() || args["cursor"].is_string()) ||
        !(args["from"].is_null() || args["from"].is_string()) || !args["global"].is_boolean() ||
        !(args["limit"].is_null() || args["limit"].is_number_integer()) ||
        !(args["query"].is_null() || args["query"].is_string()) ||
        !(args["type"].is_null() || args["type"].is_string())) {
        usage(session, "search received malformed arguments", nullptr);
        return std::nullopt;
    }
    if (!args["cursor"].is_null()) {
        return parse_search_cursor(args, session);
    }
    if (!args["query"].is_string()) {
        usage(session, "search requires a query", "query", "missing_argument");
        return std::nullopt;
    }
    if (!args["type"].is_string()) {
        usage(session, "search type is invalid", "--type");
        return std::nullopt;
    }
    SearchState state;
    state.query = args["query"].get<std::string>();
    if (!pinned_search_input(state.query)) {
        usage(session, "search query is not in canonical TDLib form", "query");
        return std::nullopt;
    }
    if (!args["chat"].is_null() && args["global"].get<bool>()) {
        usage(session, "--chat and --global are mutually exclusive", "--chat",
              "mutually_exclusive");
        return std::nullopt;
    }
    if (!args["chat"].is_null()) {
        state.scope = SearchScope::Chat;
        state.chat_selector = args["chat"].get<std::string>();
        if (!valid_resolve_selector(*state.chat_selector)) {
            usage(session, "search chat selector is invalid", "--chat");
            return std::nullopt;
        }
    }
    if (!args["from"].is_null()) {
        state.sender_selector = args["from"].get<std::string>();
        if (!valid_resolve_selector(*state.sender_selector)) {
            usage(session, "search sender selector is invalid", "--from");
            return std::nullopt;
        }
    }
    const auto type = parse_search_type(args["type"].get_ref<const std::string&>());
    if (!type) {
        usage(session, "search type is invalid", "--type");
        return std::nullopt;
    }
    state.type = *type;
    if (!args["limit"].is_null()) {
        const auto limit = integer64(args["limit"]);
        if (!limit || *limit < 1 || *limit > kMaximumSearchLimit) {
            usage(session, "search limit must be between 1 and 100", "-n");
            return std::nullopt;
        }
        state.limit = static_cast<std::int32_t>(*limit);
    }
    return state;
}

std::optional<MembersState> parse_members(const json& args, RequestSession& session) {
    if (!exact_fields(args, {"admins", "bots", "chat", "cursor", "limit", "query"}) ||
        !args["admins"].is_boolean() || !args["bots"].is_boolean() ||
        !(args["chat"].is_null() || args["chat"].is_string()) ||
        !(args["cursor"].is_null() || args["cursor"].is_string()) ||
        !(args["limit"].is_null() || args["limit"].is_number_integer()) ||
        !(args["query"].is_null() || args["query"].is_string())) {
        usage(session, "chat members received malformed arguments", nullptr);
        return std::nullopt;
    }
    if (!args["cursor"].is_null()) {
        if (args["admins"].get<bool>() || args["bots"].get<bool>() || !args["chat"].is_null() ||
            !args["limit"].is_null() || !args["query"].is_null()) {
            usage(session, "chat members cursor cannot be combined with first-page arguments",
                  "--cursor", "mutually_exclusive");
            return std::nullopt;
        }
        const auto cursor = decode_members_cursor(args["cursor"].get_ref<const std::string&>());
        if (!cursor) {
            usage(session, "invalid chat members cursor", "--cursor", "invalid_cursor");
            return std::nullopt;
        }
        return MembersState{.chat_selector = std::to_string(cursor->chat_id),
                            .filter = cursor->filter,
                            .query = cursor->query,
                            .limit = cursor->limit,
                            .cursor = cursor};
    }
    if (!args["chat"].is_string()) {
        usage(session, "chat members requires a chat selector", "chat", "missing_argument");
        return std::nullopt;
    }
    const auto filters = static_cast<int>(args["admins"].get<bool>()) +
                         static_cast<int>(args["bots"].get<bool>()) +
                         static_cast<int>(!args["query"].is_null());
    if (filters > 1) {
        usage(session, "chat member filters are mutually exclusive", "filter",
              "mutually_exclusive");
        return std::nullopt;
    }
    MembersState state;
    state.chat_selector = args["chat"].get<std::string>();
    if (!valid_resolve_selector(state.chat_selector)) {
        usage(session, "chat members selector is invalid", "chat");
        return std::nullopt;
    }
    if (args["admins"].get<bool>()) {
        state.filter = MembersFilter::Administrators;
    } else if (args["bots"].get<bool>()) {
        state.filter = MembersFilter::Bots;
    } else if (!args["query"].is_null()) {
        state.filter = MembersFilter::Query;
        state.query = args["query"].get<std::string>();
        if (state.query->size() > 256 || !pinned_search_input(*state.query)) {
            usage(session, "chat members query is invalid", "--query");
            return std::nullopt;
        }
    }
    if (!args["limit"].is_null()) {
        const auto limit = integer64(args["limit"]);
        if (!limit || *limit < 1 || *limit > kMaximumMembersLimit) {
            usage(session, "chat members limit must be between 1 and 200", "-n");
            return std::nullopt;
        }
        state.limit = static_cast<std::int32_t>(*limit);
    }
    return state;
}

std::optional<std::string> timestamp(std::int32_t seconds) {
    if (seconds == 0) {
        return std::nullopt;
    }
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

std::optional<std::string_view> member_status(const core::TdChatMemberStatus& status) {
    switch (status.kind) {
    case core::TdChatMemberStatusKind::Creator:
        return "creator";
    case core::TdChatMemberStatusKind::Administrator:
        return "administrator";
    case core::TdChatMemberStatusKind::Member:
        return "member";
    case core::TdChatMemberStatusKind::Restricted:
        return "restricted";
    case core::TdChatMemberStatusKind::Left:
        return "left";
    case core::TdChatMemberStatusKind::Banned:
        return "banned";
    case core::TdChatMemberStatusKind::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

bool valid_member_structure(const core::TdChatMember& member) {
    const auto status = member_status(member.status);
    if (!status || member.status.unsupported_tdlib_type_id || member.inviter_user_id < 0 ||
        member.inviter_user_id > core::kTdInt53Max || member.joined_chat_date < 0 ||
        !common::valid_utf8(member.tag)) {
        return false;
    }
    switch (member.status.kind) {
    case core::TdChatMemberStatusKind::Creator:
    case core::TdChatMemberStatusKind::Administrator:
    case core::TdChatMemberStatusKind::Member:
        if (!member.status.is_member) {
            return false;
        }
        break;
    case core::TdChatMemberStatusKind::Left:
    case core::TdChatMemberStatusKind::Banned:
        if (member.status.is_member) {
            return false;
        }
        break;
    case core::TdChatMemberStatusKind::Restricted:
        break;
    case core::TdChatMemberStatusKind::Unknown:
        return false;
    }
    return member.member.kind == core::TdMessageSenderKind::User
               ? member.member.id > 0 && member.member.id <= core::kTdInt53Max
               : member.member.kind == core::TdMessageSenderKind::Chat &&
                     core::valid_td_chat_id(member.member.id);
}

bool valid_user(const core::TdUserSummary& user, std::int64_t expected_id) {
    return user.id == expected_id && user.id > 0 && user.id <= core::kTdInt53Max &&
           common::valid_utf8(user.first_name) && common::valid_utf8(user.last_name) &&
           std::ranges::all_of(user.usernames, [](const std::string& username) {
               return !username.empty() && common::valid_utf8(username);
           });
}

std::string display_name(const core::TdUserSummary& user) {
    if (user.last_name.empty()) {
        return user.first_name;
    }
    return user.first_name + " " + user.last_name;
}

std::optional<MemberRow>
enrich_user_member(ResolverConsumer& resolver, core::TdClient& client, std::string_view account,
                   const core::TdChatMember& member, std::string_view status,
                   const std::optional<std::string>& joined, RequestSession& session) {
    const auto response =
        target_read(resolver, client, account, "chat_members", session, [&](const auto& current) {
            return client.get_user(current, member.member.id);
        });
    if (!response) {
        return std::nullopt;
    }
    if (const auto* error = response->value.get_if<core::TdError>()) {
        td_error(session, "chat_members", *error);
        return std::nullopt;
    }
    const auto* user = response->value.get_if<core::TdUserSummary>();
    if (user == nullptr || !valid_user(*user, member.member.id) || user->usernames.size() > 100) {
        return std::nullopt;
    }
    return MemberRow{.sender = member.member,
                     .is_bot = user->is_bot,
                     .display_name = display_name(*user),
                     .usernames = user->usernames,
                     .status = std::string(status),
                     .tag = member.tag,
                     .joined_at = joined};
}

std::optional<MemberRow>
enrich_chat_member(ResolverConsumer& resolver, core::TdClient& client, std::string_view account,
                   const core::TdChatMember& member, std::string_view status,
                   const std::optional<std::string>& joined, RequestSession& session) {
    const auto response =
        target_read(resolver, client, account, "chat_members", session, [&](const auto& current) {
            return client.get_chat(current, member.member.id);
        });
    if (!response) {
        return std::nullopt;
    }
    if (const auto* error = response->value.get_if<core::TdError>()) {
        td_error(session, "chat_members", *error);
        return std::nullopt;
    }
    const auto* chat = response->value.get_if<core::TdChat>();
    if (chat == nullptr || chat->id != member.member.id) {
        return std::nullopt;
    }
    const auto identity =
        materialize_chat_identity(client, *chat, [&](const ReadyReadStart& start) {
            return target_read(resolver, client, account, "chat_members", session, start);
        });
    if (identity.status != ChatIdentityStatus::Success || !identity.identity) {
        if (identity.status == ChatIdentityStatus::TdError && identity.error) {
            td_error(session, "chat_members", *identity.error);
        }
        return std::nullopt;
    }
    if (identity.identity->usernames.size() > 100) {
        return std::nullopt;
    }
    return MemberRow{.sender = member.member,
                     .is_bot = false,
                     .display_name = identity.identity->title,
                     .usernames = identity.identity->usernames,
                     .status = std::string(status),
                     .tag = member.tag,
                     .joined_at = joined};
}

std::optional<MemberRow> enrich_member(ResolverConsumer& resolver, core::TdClient& client,
                                       std::string_view account, const core::TdChatMember& member,
                                       RequestSession& session) {
    const auto status = member_status(member.status);
    const auto joined = timestamp(member.joined_chat_date);
    if (!valid_member_structure(member) || !status || (member.joined_chat_date != 0 && !joined)) {
        return std::nullopt;
    }
    if (member.member.kind == core::TdMessageSenderKind::User) {
        return enrich_user_member(resolver, client, account, member, *status, joined, session);
    }
    if (member.member.kind == core::TdMessageSenderKind::Chat) {
        return enrich_chat_member(resolver, client, account, member, *status, joined, session);
    }
    return std::nullopt;
}

json member_json(const MemberRow& member) {
    return {{"sender",
             {{"type", member.sender.kind == core::TdMessageSenderKind::User ? "user" : "chat"},
              {"id", member.sender.id}}},
            {"is_bot", member.is_bot},
            {"display_name", member.display_name},
            {"usernames", member.usernames},
            {"status", member.status},
            {"tag", member.tag},
            {"joined_at", member.joined_at ? json(*member.joined_at) : json(nullptr)}};
}

bool selected_member(const MemberRow& member, MembersFilter filter,
                     const std::optional<std::string>& query) {
    switch (filter) {
    case MembersFilter::Recent:
        return true;
    case MembersFilter::Administrators:
        return member.status == "creator" || member.status == "administrator";
    case MembersFilter::Bots:
        return member.sender.kind == core::TdMessageSenderKind::User && member.is_bot;
    case MembersFilter::Query:
        return query && (member.display_name.find(*query) != std::string::npos ||
                         std::ranges::any_of(member.usernames, [&](const std::string& username) {
                             return username.find(*query) != std::string::npos;
                         }));
    }
    return false;
}

std::optional<std::vector<MemberRow>>
enrich_members(ResolverConsumer& resolver, core::TdClient& client, std::string_view account,
               const std::vector<core::TdChatMember>& source, RequestSession& session) {
    std::vector<MemberRow> rows;
    rows.reserve(source.size());
    std::unordered_set<MemberKey, MemberKeyHash> seen;
    for (const auto& member : source) {
        const MemberKey key{.kind = member.member.kind, .id = member.member.id};
        if (!valid_member_structure(member) || !seen.insert(key).second) {
            return std::nullopt;
        }
    }
    for (const auto& member : source) {
        const auto row = enrich_member(resolver, client, account, member, session);
        if (!row) {
            return std::nullopt;
        }
        rows.push_back(*row);
    }
    return rows;
}

json members_result(const std::vector<MemberRow>& rows, const std::optional<std::string>& next) {
    json items = json::array();
    for (const auto& row : rows) {
        items.push_back(member_json(row));
    }
    return {{"items", std::move(items)}, {"next", next ? json(*next) : json(nullptr)}};
}

std::optional<MembersChatType> members_chat_type(const ResolvedChatTarget& target) {
    if (!target.observed_chat) {
        return std::nullopt;
    }
    switch (target.observed_chat->kind) {
    case core::TdChatKind::BasicGroup:
        return MembersChatType::BasicGroup;
    case core::TdChatKind::Supergroup:
        return MembersChatType::Supergroup;
    case core::TdChatKind::Channel:
        return MembersChatType::Channel;
    case core::TdChatKind::Private:
    case core::TdChatKind::Secret:
    case core::TdChatKind::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

bool archived(const ChatSummary& summary) {
    return summary.is_archived;
}

bool native_search_filter_matches(SearchType type, core::TdMessageContentKind content) {
    switch (type) {
    case SearchType::Any:
    case SearchType::Text:
    case SearchType::Link:
        return true;
    case SearchType::Photo:
        return content == core::TdMessageContentKind::Photo;
    case SearchType::Video:
        return content == core::TdMessageContentKind::Video;
    case SearchType::Document:
        return content == core::TdMessageContentKind::Document;
    case SearchType::Voice:
        return content == core::TdMessageContentKind::Voice;
    }
    return false;
}

json chat_info_base(const ChatSummary& summary) {
    return {{"id", summary.identity.id},
            {"title", summary.identity.title},
            {"type", summary.identity.type},
            {"is_bot", summary.identity.is_bot},
            {"usernames", summary.identity.usernames},
            {"is_archived", archived(summary)},
            {"folder_ids", summary.folder_ids},
            {"is_marked_unread", summary.is_marked_unread},
            {"unread_count", summary.unread_count},
            {"unread_mention_count", summary.unread_mention_count},
            {"unread_reaction_count", summary.unread_reaction_count},
            {"unread_poll_vote_count", summary.unread_poll_vote_count}};
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed two-scope pager.
void M2ReadCoordinator::search(const proto::Request& request, RequestSession& session) {
    auto state = parse_search(request.args, session);
    if (!state) {
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal = bind_principal(resolver, M2Operation::Search, session);
    if (!principal) {
        return;
    }
    if (principal->is_bot) {
        session.error("BOT_UNSUPPORTED", "search requires a user account",
                      {{"operation", "search"}}, kUsage);
        return;
    }
    if (state->cursor &&
        (state->cursor->account != account_ || state->cursor->user_id != principal->id)) {
        usage(session, "search cursor scope does not match this account", "--cursor",
              "cursor_scope_mismatch");
        return;
    }
    std::optional<ResolvedChatTarget> target;
    if (state->scope == SearchScope::Chat) {
        target = resolve_chat(resolver, *state->chat_selector, M2Operation::Search, session);
        if (!target) {
            return;
        }
        if (!target->observed_chat || target->observed_chat->kind == core::TdChatKind::Secret ||
            target->observed_chat->kind == core::TdChatKind::Unknown) {
            usage(session, "search does not support this chat type", "--chat",
                  "unsupported_chat_type");
            return;
        }
        if (state->cursor && target->chat.id != *state->cursor->chat_id) {
            pagination(session, "search", "scope_changed");
            return;
        }
    }
    if (state->sender_selector) {
        const auto sender = resolver.resolve_user(
            *state->sender_selector,
            target && target->observed_chat ? target->observed_chat : std::nullopt);
        if (const auto* error = std::get_if<ResolverError>(&sender)) {
            emit_resolver_error(*error, session, M2Operation::Search);
            return;
        }
        if (std::holds_alternative<ResolverStop>(sender)) {
            return;
        }
        state->sender_user_id = std::get<UserIdentity>(sender).id;
    }

    std::vector<MessageSummary> items;
    std::set<std::pair<std::int64_t, std::int64_t>> seen_messages;
    std::set<std::string> seen_offsets;
    std::int64_t chat_marker = state->cursor ? *state->cursor->next_offset_message_id : 0;
    std::string global_offset = state->cursor ? *state->cursor->next_offset : "";
    std::optional<std::int64_t> last_chat_id =
        state->cursor ? state->cursor->last_raw_message_id : std::nullopt;
    std::optional<SearchRawOrder> last_global =
        state->cursor ? state->cursor->last_raw_order : std::nullopt;
    if (!global_offset.empty()) {
        seen_offsets.insert(global_offset);
    }

    for (;;) {
        const auto remaining = state->limit - static_cast<std::int32_t>(items.size());
        const auto request_limit = std::min(kMaximumSearchLimit, remaining);
        std::optional<ReadyReadResult> response;
        if (state->scope == SearchScope::Chat) {
            response = target_read(resolver, client_.get(), account_, "search", session,
                                   [&](const auto& current) {
                                       return client_.get().search_chat_messages(
                                           current, {.chat_id = target->chat.id,
                                                     .query = state->query,
                                                     .sender_user_id = state->sender_user_id,
                                                     .from_message_id = chat_marker,
                                                     .limit = request_limit,
                                                     .filter = td_search_filter(state->type)});
                                   });
        } else {
            response = target_read(resolver, client_.get(), account_, "search", session,
                                   [&](const auto& current) {
                                       return client_.get().search_messages(
                                           current, {.query = state->query,
                                                     .offset = global_offset,
                                                     .limit = request_limit,
                                                     .filter = td_search_filter(state->type)});
                                   });
        }
        if (!response) {
            return;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(session, "search", *error);
            return;
        }

        const std::vector<std::optional<core::TdMessageSummary>>* raw_messages = nullptr;
        std::int32_t total_count = -2;
        std::int64_t next_chat_marker = 0;
        std::string next_global_offset;
        if (state->scope == SearchScope::Chat) {
            const auto* page = response->value.get_if<core::TdFoundChatMessages>();
            if (page != nullptr) {
                raw_messages = &page->messages;
                total_count = page->total_count;
                next_chat_marker = page->next_from_message_id;
            }
        } else {
            const auto* page = response->value.get_if<core::TdFoundMessages>();
            if (page != nullptr) {
                raw_messages = &page->messages;
                total_count = page->total_count;
                next_global_offset = page->next_offset;
            }
        }
        if (raw_messages == nullptr || total_count < -1 ||
            raw_messages->size() > static_cast<std::size_t>(request_limit)) {
            internal(session, "search");
            return;
        }

        std::vector<MessageSummary> page_items;
        page_items.reserve(raw_messages->size());
        auto page_last_chat = last_chat_id;
        auto page_last_global = last_global;
        for (const auto& raw : *raw_messages) {
            if (!raw) {
                internal(session, "search");
                return;
            }
            const auto summary = materialize_message_summary(*raw);
            const auto key = std::pair{raw->chat_id, raw->id};
            if (!summary || !native_search_filter_matches(state->type, raw->content_kind) ||
                !seen_messages.insert(key).second) {
                internal(session, "search");
                return;
            }
            if (state->scope == SearchScope::Chat) {
                if (raw->chat_id != target->chat.id ||
                    (state->sender_user_id &&
                     (raw->sender.kind != core::TdMessageSenderKind::User ||
                      raw->sender.id != *state->sender_user_id)) ||
                    (page_last_chat && raw->id >= *page_last_chat)) {
                    internal(session, "search");
                    return;
                }
                page_last_chat = raw->id;
            } else {
                if (raw->date < 0) {
                    internal(session, "search");
                    return;
                }
                const SearchRawOrder order{
                    .date = raw->date, .chat_id = raw->chat_id, .message_id = raw->id};
                if (page_last_global &&
                    std::tie(order.date, order.chat_id, order.message_id) >=
                        std::tie(page_last_global->date, page_last_global->chat_id,
                                 page_last_global->message_id)) {
                    internal(session, "search");
                    return;
                }
                page_last_global = order;
            }
            if (search_postfilter(state->type, raw->content_kind) &&
                (!state->sender_user_id || (raw->sender.kind == core::TdMessageSenderKind::User &&
                                            raw->sender.id == *state->sender_user_id))) {
                page_items.push_back(*summary);
            }
        }

        if (state->scope == SearchScope::Chat) {
            if (next_chat_marker < 0 || next_chat_marker > core::kTdInt53Max ||
                (chat_marker > 0 && next_chat_marker >= chat_marker) ||
                (next_chat_marker > 0 &&
                 (!page_last_chat || next_chat_marker >= *page_last_chat))) {
                pagination(session, "search", "marker_not_advancing");
                return;
            }
        } else if (!next_global_offset.empty()) {
            if (!pinned_search_input(next_global_offset)) {
                pagination(session, "search", "page_invalid");
                return;
            }
            if (next_global_offset == global_offset ||
                !seen_offsets.insert(next_global_offset).second) {
                pagination(session, "search", "marker_not_advancing");
                return;
            }
        }
        items.insert(items.end(), page_items.begin(), page_items.end());
        last_chat_id = page_last_chat;
        last_global = page_last_global;
        const bool exhausted =
            state->scope == SearchScope::Chat ? next_chat_marker == 0 : next_global_offset.empty();
        if (static_cast<std::int32_t>(items.size()) >= state->limit || exhausted) {
            std::optional<std::string> next;
            if (!exhausted) {
                const SearchCursor cursor{
                    .account = account_,
                    .user_id = principal->id,
                    .limit = state->limit,
                    .query = state->query,
                    .scope = state->scope,
                    .chat_id = target ? std::optional<std::int64_t>{target->chat.id} : std::nullopt,
                    .sender_user_id = state->sender_user_id,
                    .type = state->type,
                    .next_offset_message_id = state->scope == SearchScope::Chat
                                                  ? std::optional<std::int64_t>{next_chat_marker}
                                                  : std::nullopt,
                    .next_offset = state->scope == SearchScope::Global
                                       ? std::optional<std::string>{next_global_offset}
                                       : std::nullopt,
                    .last_raw_message_id =
                        state->scope == SearchScope::Chat ? last_chat_id : std::nullopt,
                    .last_raw_order =
                        state->scope == SearchScope::Global ? last_global : std::nullopt};
                next = encode_search_cursor(cursor);
            }
            json output = json::array();
            for (const auto& item : items) {
                output.push_back(message_summary_json(item));
            }
            if (!final_stop(client_.get(), "search", session)) {
                session.result(
                    {{"items", std::move(output)}, {"next", next ? json(*next) : json(nullptr)}});
            }
            return;
        }
        chat_marker = next_chat_marker;
        global_offset = std::move(next_global_offset);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed type-specific source map.
void M2ReadCoordinator::chat_info(const proto::Request& request, RequestSession& session) {
    if (!exact_fields(request.args, {"chat"}) || !request.args["chat"].is_string()) {
        usage(session, "chat info requires a chat selector", "chat", "missing_argument");
        return;
    }
    const auto selector = request.args["chat"].get<std::string>();
    if (!valid_resolve_selector(selector)) {
        usage(session, "chat info selector is invalid", "chat");
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    if (!bind_principal(resolver, M2Operation::ChatInfo, session)) {
        return;
    }
    const auto target = resolve_chat(resolver, selector, M2Operation::ChatInfo, session);
    if (!target) {
        return;
    }
    if (!target->observed_chat || target->observed_chat->kind == core::TdChatKind::Secret ||
        target->observed_chat->kind == core::TdChatKind::Unknown) {
        usage(session, "chat info does not support this chat type", "chat",
              "unsupported_chat_type");
        return;
    }
    const auto summary = materialize_chat_summary(*target->observed_chat, target->chat);
    if (!summary || summary->identity.usernames.size() > 100 || summary->folder_ids.size() > 100) {
        internal(session, "chat_info");
        return;
    }
    auto result = chat_info_base(*summary);
    switch (target->observed_chat->kind) {
    case core::TdChatKind::Private: {
        if (!target->observed_user || !target->private_user_id ||
            target->observed_user->id != *target->private_user_id ||
            !valid_user(*target->observed_user, *target->private_user_id)) {
            internal(session, "chat_info");
            return;
        }
        const auto response = target_read(
            resolver, client_.get(), account_, "chat_info", session, [&](const auto& current) {
                return client_.get().get_user_full_info(current, *target->private_user_id);
            });
        if (!response) {
            return;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(session, "chat_info", *error);
            return;
        }
        const auto* full = response->value.get_if<core::TdUserFullInfo>();
        if (full == nullptr || !common::valid_utf8(full->description)) {
            internal(session, "chat_info");
            return;
        }
        result.update({{"description", full->description},
                       {"member_count", nullptr},
                       {"is_forum", false},
                       {"linked_chat_id", nullptr}});
        break;
    }
    case core::TdChatKind::BasicGroup: {
        const auto group = target_read(
            resolver, client_.get(), account_, "chat_info", session, [&](const auto& current) {
                return client_.get().get_basic_group(current, target->observed_chat->related_id);
            });
        if (!group) {
            return;
        }
        if (const auto* error = group->value.get_if<core::TdError>()) {
            td_error(session, "chat_info", *error);
            return;
        }
        const auto* basic = group->value.get_if<core::TdBasicGroup>();
        if (basic == nullptr || basic->id != target->observed_chat->related_id ||
            basic->member_count < 0) {
            internal(session, "chat_info");
            return;
        }
        const auto full_response = target_read(
            resolver, client_.get(), account_, "chat_info", session, [&](const auto& current) {
                return client_.get().get_basic_group_full_info(current, basic->id);
            });
        if (!full_response) {
            return;
        }
        if (const auto* error = full_response->value.get_if<core::TdError>()) {
            td_error(session, "chat_info", *error);
            return;
        }
        const auto* full = full_response->value.get_if<core::TdBasicGroupFullInfo>();
        if (full == nullptr || !common::valid_utf8(full->description)) {
            internal(session, "chat_info");
            return;
        }
        result.update({{"description", full->description},
                       {"member_count", basic->member_count},
                       {"is_forum", false},
                       {"linked_chat_id", nullptr}});
        break;
    }
    case core::TdChatKind::Supergroup:
    case core::TdChatKind::Channel: {
        if (!target->observed_supergroup ||
            target->observed_supergroup->id != target->observed_chat->related_id ||
            target->observed_supergroup->is_channel !=
                (target->observed_chat->kind == core::TdChatKind::Channel)) {
            internal(session, "chat_info");
            return;
        }
        const auto response = target_read(resolver, client_.get(), account_, "chat_info", session,
                                          [&](const auto& current) {
                                              return client_.get().get_supergroup_full_info(
                                                  current, target->observed_supergroup->id);
                                          });
        if (!response) {
            return;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(session, "chat_info", *error);
            return;
        }
        const auto* full = response->value.get_if<core::TdSupergroupFullInfo>();
        if (full == nullptr || full->member_count < 0 || !common::valid_utf8(full->description) ||
            (full->linked_chat_id != 0 && !core::valid_td_chat_id(full->linked_chat_id)) ||
            (full->direct_messages_chat_id != 0 &&
             !core::valid_td_chat_id(full->direct_messages_chat_id))) {
            internal(session, "chat_info");
            return;
        }
        result.update({{"description", full->description},
                       {"member_count", full->member_count},
                       {"is_forum", target->observed_supergroup->is_forum},
                       {"linked_chat_id",
                        full->linked_chat_id == 0 ? json(nullptr) : json(full->linked_chat_id)}});
        break;
    }
    case core::TdChatKind::Secret:
    case core::TdChatKind::Unknown:
        internal(session, "chat_info");
        return;
    }
    if (!final_stop(client_.get(), "chat_info", session)) {
        session.result(std::move(result));
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed two-source pager.
void M2ReadCoordinator::chat_members(const proto::Request& request, RequestSession& session) {
    const auto state = parse_members(request.args, session);
    if (!state) {
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal = bind_principal(resolver, M2Operation::ChatMembers, session);
    if (!principal) {
        return;
    }
    if (state->cursor &&
        (state->cursor->account != account_ || state->cursor->user_id != principal->id)) {
        usage(session, "chat members cursor scope does not match this account", "--cursor",
              "cursor_scope_mismatch");
        return;
    }
    const auto target =
        resolve_chat(resolver, state->chat_selector, M2Operation::ChatMembers, session);
    if (!target || !target->observed_chat) {
        return;
    }
    const auto chat_type = members_chat_type(*target);
    if (!chat_type) {
        usage(session, "chat members does not support this chat type", "chat",
              "unsupported_chat_type");
        return;
    }
    const auto source_id = target->observed_chat->related_id;
    if (source_id <= 0 || source_id > core::kTdInt53Max) {
        internal(session, "chat_members");
        return;
    }
    if (state->cursor &&
        (state->cursor->chat_id != target->chat.id || state->cursor->chat_type != *chat_type ||
         state->cursor->source_id != source_id)) {
        pagination(session, "chat_members", "source_changed");
        return;
    }

    if (*chat_type == MembersChatType::BasicGroup) {
        const auto response = target_read(
            resolver, client_.get(), account_, "chat_members", session, [&](const auto& current) {
                return client_.get().get_basic_group_full_info(current, source_id);
            });
        if (!response) {
            return;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(session, "chat_members", *error);
            return;
        }
        const auto* full = response->value.get_if<core::TdBasicGroupFullInfo>();
        if (full == nullptr) {
            internal(session, "chat_members");
            return;
        }
        auto rows = enrich_members(resolver, client_.get(), account_, full->members, session);
        if (!rows) {
            if (!session.has_terminal() && !session.cancellation_requested()) {
                internal(session, "chat_members");
            }
            return;
        }
        std::erase_if(*rows, [&](const MemberRow& row) {
            return !selected_member(row, state->filter, state->query);
        });
        if (rows->size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            internal(session, "chat_members");
            return;
        }
        const auto source_count = static_cast<std::int32_t>(rows->size());
        if (state->cursor && state->cursor->source_count != source_count) {
            pagination(session, "chat_members", "source_changed");
            return;
        }
        const auto offset = state->cursor ? state->cursor->offset : 0;
        if (offset > source_count) {
            pagination(session, "chat_members", "source_changed");
            return;
        }
        const auto finish = offset + std::min(state->limit, source_count - offset);
        const std::vector<MemberRow> selected(rows->begin() + offset, rows->begin() + finish);
        std::optional<std::string> next;
        if (finish < source_count) {
            next = encode_members_cursor({.account = account_,
                                          .user_id = principal->id,
                                          .limit = state->limit,
                                          .chat_id = target->chat.id,
                                          .chat_type = *chat_type,
                                          .source_id = source_id,
                                          .filter = state->filter,
                                          .query = state->query,
                                          .offset = finish,
                                          .source_count = source_count});
        }
        if (!final_stop(client_.get(), "chat_members", session)) {
            session.result(members_result(selected, next));
        }
        return;
    }

    std::vector<MemberRow> items;
    auto offset = state->cursor ? state->cursor->offset : 0;
    std::unordered_set<MemberKey, MemberKeyHash> seen;
    for (;;) {
        const auto request_limit = state->limit - static_cast<std::int32_t>(items.size());
        const auto response = target_read(
            resolver, client_.get(), account_, "chat_members", session, [&](const auto& current) {
                return client_.get().get_supergroup_members(
                    current, source_id, *td_members_filter(state->filter),
                    state->query.value_or(""), offset, request_limit);
            });
        if (!response) {
            return;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(session, "chat_members", *error);
            return;
        }
        const auto* page = response->value.get_if<core::TdChatMembers>();
        if (page == nullptr || page->total_count < 0 ||
            (page->total_count >= 0 &&
             static_cast<std::size_t>(page->total_count) < page->members.size()) ||
            page->members.size() > static_cast<std::size_t>(request_limit)) {
            internal(session, "chat_members");
            return;
        }
        if (page->members.empty()) {
            if (!final_stop(client_.get(), "chat_members", session)) {
                session.result(members_result(items, std::nullopt));
            }
            return;
        }
        for (const auto& member : page->members) {
            const MemberKey key{.kind = member.member.kind, .id = member.member.id};
            if (!seen.insert(key).second) {
                pagination(session, "chat_members", "page_invalid");
                return;
            }
        }
        auto rows = enrich_members(resolver, client_.get(), account_, page->members, session);
        if (!rows) {
            if (!session.has_terminal() && !session.cancellation_requested()) {
                internal(session, "chat_members");
            }
            return;
        }
        items.insert(items.end(), rows->begin(), rows->end());
        if (page->members.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() - offset)) {
            pagination(session, "chat_members", "marker_not_advancing");
            return;
        }
        offset += static_cast<std::int32_t>(page->members.size());
        if (static_cast<std::int32_t>(items.size()) == state->limit) {
            const auto next = encode_members_cursor({.account = account_,
                                                     .user_id = principal->id,
                                                     .limit = state->limit,
                                                     .chat_id = target->chat.id,
                                                     .chat_type = *chat_type,
                                                     .source_id = source_id,
                                                     .filter = state->filter,
                                                     .query = state->query,
                                                     .offset = offset,
                                                     .source_count = std::nullopt});
            if (!final_stop(client_.get(), "chat_members", session)) {
                session.result(members_result(items, next));
            }
            return;
        }
    }
}

void register_search_command(Dispatcher& dispatcher, M2ReadCoordinator& coordinator) {
    dispatcher.register_command(
        "search", {.tier = Tier::Read,
                   .handler =
                       [&coordinator](const proto::Request& request, RequestSession& session) {
                           coordinator.search(request, session);
                       },
                   .config_admission = true});
}

void register_chat_info_command(Dispatcher& dispatcher, M2ReadCoordinator& coordinator) {
    dispatcher.register_command(
        "chat info", {.tier = Tier::Read,
                      .handler =
                          [&coordinator](const proto::Request& request, RequestSession& session) {
                              coordinator.chat_info(request, session);
                          },
                      .config_admission = true});
}

void register_chat_members_command(Dispatcher& dispatcher, M2ReadCoordinator& coordinator) {
    dispatcher.register_command("chat members", {.tier = Tier::Read,
                                                 .handler =
                                                     [&coordinator](const proto::Request& request,
                                                                    RequestSession& session) {
                                                         coordinator.chat_members(request, session);
                                                     },
                                                 .config_admission = true});
}

} // namespace tgcli::daemon
