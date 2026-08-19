#include "daemon/read_commands.hpp"

#include "common/exit_codes.hpp"
#include "daemon/local_selector.hpp"
#include "daemon/message_summary.hpp"
#include "daemon/read_domain.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;

struct ReadState {
    std::string selector;
    std::int32_t limit = kDefaultReadLimit;
    std::optional<std::int64_t> before;
    std::optional<std::int32_t> since;
    std::optional<std::int32_t> until;
    std::optional<TopicRef> topic;
    bool local = false;
    std::optional<ReadCursor> cursor;
};

bool exact_fields(const json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    return std::ranges::all_of(expected,
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

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

void usage(RequestSession& session, std::string_view message, const json& argument,
           std::string_view reason = "invalid_argument") {
    session.error("USAGE", std::string(message), {{"argument", argument}, {"reason", reason}},
                  kUsage);
}

void internal(RequestSession& session) {
    session.error("INTERNAL", "read returned an unexpected object",
                  {{"operation", "read"}, {"reason", "internal_error"}}, kGeneric);
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

void td_error(RequestSession& session, const core::TdError& error) {
    if (error.code == 429) {
        session.error("RATE_LIMITED", "Telegram rate limit",
                      {{"operation", "read"},
                       {"tdlib_code", 429},
                       {"retry_after", retry_after(error.message)}},
                      kRateLimited);
        return;
    }
    session.error("TDLIB_ERROR", "read TDLib request failed",
                  {{"operation", "read"}, {"tdlib_code", error.code}}, kGeneric);
}

void topic_not_found(RequestSession& session, std::int64_t chat_id, const TopicRef& topic) {
    session.error("NOT_FOUND", "topic was not found",
                  {{"chat_id", chat_id}, {"topic", topic_ref_json(topic)}}, kNotFound);
}

bool handle_target_stop(const ReadyReadResult& result, core::TdClient& client,
                        std::string_view account, RequestSession& session) {
    switch (result.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        if (!session.cancellation_requested()) {
            session.error("NOT_AUTHED", "read requires an authenticated account",
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
            session.error("TIMEOUT", "read request timed out",
                          {{"operation", "read"},
                           {"state", snapshot ? json(core::auth_state_name(snapshot->data.state))
                                              : json(nullptr)}},
                          kTimeout);
        }
        return true;
    }
    case ReadyReadStatus::Failed:
        if (!session.cancellation_requested()) {
            internal(session);
        }
        return true;
    case ReadyReadStatus::Cancelled:
        return true;
    }
    return true;
}

std::optional<ReadyReadResult> target_read(ResolverConsumer& resolver, core::TdClient& client,
                                           std::string_view account, RequestSession& session,
                                           const ReadyReadStart& start) {
    auto result = resolver.read_target(start);
    if (handle_target_stop(result, client, account, session)) {
        return std::nullopt;
    }
    return result;
}

// Request validation mirrors the closed first-page and continuation forms.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<ReadState> parse_request(const json& args,
                                       std::chrono::system_clock::time_point request_start,
                                       RequestSession& session) {
    if (!exact_fields(args,
                      {"before", "chat", "cursor", "limit", "local", "since", "topic", "until"}) ||
        !args["local"].is_boolean() || !(args["chat"].is_null() || args["chat"].is_string()) ||
        !(args["cursor"].is_null() || args["cursor"].is_string()) ||
        !(args["before"].is_null() || args["before"].is_number_integer()) ||
        !(args["limit"].is_null() || args["limit"].is_number_integer()) ||
        !(args["since"].is_null() || args["since"].is_string()) ||
        !(args["until"].is_null() || args["until"].is_string()) ||
        !(args["topic"].is_null() || args["topic"].is_string())) {
        usage(session, "read received malformed arguments", nullptr);
        return std::nullopt;
    }

    if (!args["cursor"].is_null()) {
        if (!args["chat"].is_null() || !args["before"].is_null() || !args["limit"].is_null() ||
            args["local"].get<bool>() || !args["since"].is_null() || !args["until"].is_null() ||
            !args["topic"].is_null()) {
            usage(session, "read cursor cannot be combined with first-page arguments", "--cursor",
                  "mutually_exclusive");
            return std::nullopt;
        }
        const auto cursor = decode_read_cursor(args["cursor"].get_ref<const std::string&>());
        if (!cursor) {
            usage(session, "invalid read cursor", "--cursor", "invalid_cursor");
            return std::nullopt;
        }
        return ReadState{.selector = std::to_string(cursor->chat_id),
                         .limit = cursor->limit,
                         .before = std::nullopt,
                         .since = cursor->since,
                         .until = cursor->until,
                         .topic = cursor->topic,
                         .local = cursor->local,
                         .cursor = cursor};
    }

    if (!args["chat"].is_string()) {
        usage(session, "read requires a chat selector", "chat", "missing_argument");
        return std::nullopt;
    }
    ReadState state;
    state.selector = args["chat"].get<std::string>();
    state.local = args["local"].get<bool>();
    if (state.local) {
        const auto selector = classify_local_selector(state.selector);
        if (!selector || selector->kind == LocalSelectorKind::InvalidLink) {
            usage(session, "read selector is invalid", "selector");
            return std::nullopt;
        }
        if (selector->kind == LocalSelectorKind::UnsupportedLink) {
            usage(session, "read selector has an unsupported link type", "selector",
                  "unsupported_link_type");
            return std::nullopt;
        }
    } else if (!valid_resolve_selector(state.selector)) {
        usage(session, "read selector is invalid", "selector");
        return std::nullopt;
    }
    if (!args["limit"].is_null()) {
        const auto limit = integer64(args["limit"]);
        if (!limit || *limit < 1 || *limit > kMaximumReadLimit) {
            usage(session, "read limit must be between 1 and 100", "-n");
            return std::nullopt;
        }
        state.limit = static_cast<std::int32_t>(*limit);
    }
    if (!args["before"].is_null()) {
        state.before = integer64(args["before"]);
        if (!state.before || !valid_int53(*state.before)) {
            usage(session, "--before must be a nonzero int53 message id", "--before");
            return std::nullopt;
        }
    }
    if (!args["topic"].is_null()) {
        state.topic = parse_read_topic(args["topic"].get_ref<const std::string&>());
        if (!state.topic) {
            usage(session, "invalid read topic", "--topic");
            return std::nullopt;
        }
    }
    if (!args["since"].is_null()) {
        state.since = parse_read_timestamp(args["since"].get_ref<const std::string&>(),
                                           ReadTimestampBound::Since, request_start);
        if (!state.since) {
            usage(session, "invalid --since timestamp", "--since");
            return std::nullopt;
        }
    }
    if (!args["until"].is_null()) {
        state.until = parse_read_timestamp(args["until"].get_ref<const std::string&>(),
                                           ReadTimestampBound::Until, request_start);
        if (!state.until) {
            usage(session, "invalid --until timestamp", "--until");
            return std::nullopt;
        }
    }
    if (state.since && state.until && *state.since > *state.until) {
        usage(session, "--since must not be later than --until", "--since/--until");
        return std::nullopt;
    }
    return state;
}

std::optional<ResolvedChatTarget> resolve_target(ResolverConsumer& resolver, const ReadState& state,
                                                 RequestSession& session) {
    const auto outcome =
        resolver.resolve_chat(state.selector, state.local ? ResolverScope::LocalMaterialized
                                                          : ResolverScope::ActiveDialogs);
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, M2Operation::Read);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    const auto* target = std::get_if<ResolvedChatTarget>(&outcome);
    return target == nullptr ? std::nullopt : std::optional<ResolvedChatTarget>{*target};
}

bool validate_saved_chat(const core::TdChat& chat, const ResolvedChatTarget& target,
                         const ResolverPrincipal& principal) {
    return valid_int53(chat.id) && chat.id == target.chat.id &&
           chat.kind == core::TdChatKind::Private && chat.related_id == principal.id;
}

bool check_saved_ownership(ResolverConsumer& resolver, core::TdClient& client,
                           std::string_view account, const ResolvedChatTarget& target,
                           const ResolverPrincipal& principal, RequestSession& session) {
    std::optional<core::TdChat> chat;
    if (target.link_type == ResolvedLinkType::SavedMessages) {
        chat = resolver.cached_saved_messages_chat();
        if (!chat) {
            internal(session);
            return false;
        }
    } else {
        const auto response =
            target_read(resolver, client, account, session, [&](const auto& current) {
                return client.create_private_chat(current, principal.id, false);
            });
        if (!response) {
            return false;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(session, *error);
            return false;
        }
        const auto* returned = response->value.get_if<core::TdChat>();
        if (returned == nullptr) {
            internal(session);
            return false;
        }
        chat = *returned;
    }
    if (!validate_saved_chat(*chat, target, principal)) {
        if (chat->kind == core::TdChatKind::Private && chat->related_id == principal.id &&
            valid_int53(chat->id)) {
            usage(session, "saved topic requires the current Saved Messages chat", "--topic");
        } else {
            internal(session);
        }
        return false;
    }
    return true;
}

std::optional<std::int64_t> validate_thread_info(const core::TdMessageThreadInfo& info,
                                                 const ResolvedChatTarget& target,
                                                 std::int64_t requested_thread_id) {
    if (!valid_int53(info.history_chat_id) || !valid_int53(info.history_thread_id) ||
        info.starting_messages.empty()) {
        return std::nullopt;
    }
    std::optional<std::int64_t> previous;
    std::int64_t last = 0;
    for (const auto& raw : info.starting_messages) {
        if (!raw) {
            return std::nullopt;
        }
        const auto message = materialize_message_summary(*raw);
        if (!message || message->chat_id != info.history_chat_id ||
            (previous && message->id >= *previous)) {
            return std::nullopt;
        }
        previous = message->id;
        last = message->id;
    }
    if (last != info.history_thread_id) {
        return std::nullopt;
    }
    if (info.history_chat_id == target.chat.id) {
        if (info.history_thread_id != requested_thread_id) {
            return std::nullopt;
        }
    } else if (target.chat.type != "channel") {
        return std::nullopt;
    }
    return info.history_chat_id;
}

std::optional<std::int64_t> history_chat_id(ResolverConsumer& resolver, core::TdClient& client,
                                            std::string_view account, const ReadState& state,
                                            const ResolvedChatTarget& target,
                                            RequestSession& session) {
    if (!state.topic || state.topic->kind != TopicKind::Thread) {
        return target.chat.id;
    }
    if (state.local) {
        if (target.chat.type == "supergroup") {
            return target.chat.id;
        }
        usage(session, "local thread history is unavailable for this chat", "--topic",
              target.chat.type == "channel" ? "unsupported_mode" : "invalid_argument");
        return std::nullopt;
    }
    const auto response = target_read(resolver, client, account, session, [&](const auto& current) {
        return client.get_message_thread(current, target.chat.id, state.topic->id);
    });
    if (!response) {
        return std::nullopt;
    }
    if (const auto* error = response->value.get_if<core::TdError>()) {
        if (error->code == 404) {
            topic_not_found(session, target.chat.id, *state.topic);
        } else {
            td_error(session, *error);
        }
        return std::nullopt;
    }
    const auto* info = response->value.get_if<core::TdMessageThreadInfo>();
    if (info == nullptr) {
        internal(session);
        return std::nullopt;
    }
    const auto result = validate_thread_info(*info, target, state.topic->id);
    if (!result) {
        internal(session);
    }
    return result;
}

enum class ProbeStatus { Message, Missing, Failed };

struct ProbeResult {
    ProbeStatus status = ProbeStatus::Failed;
    std::optional<core::TdMessageSummary> message;
};

ProbeResult date_probe(ResolverConsumer& resolver, core::TdClient& client, std::string_view account,
                       std::int64_t history_chat, std::int32_t requested_date,
                       RequestSession& session) {
    const auto response = target_read(resolver, client, account, session, [&](const auto& current) {
        return client.get_chat_message_by_date(current, history_chat, requested_date);
    });
    if (!response) {
        return {};
    }
    if (const auto* error = response->value.get_if<core::TdError>()) {
        if (error->code == 404) {
            return {.status = ProbeStatus::Missing, .message = std::nullopt};
        }
        td_error(session, *error);
        return {};
    }
    const auto* raw = response->value.get_if<core::TdMessageSummary>();
    const auto materialized = raw == nullptr ? std::nullopt : materialize_message_summary(*raw);
    if (raw == nullptr || !materialized || materialized->chat_id != history_chat ||
        raw->date == 0 || raw->date > requested_date) {
        internal(session);
        return {};
    }
    return {.status = ProbeStatus::Message, .message = *raw};
}

std::optional<ReadyReadResult> history_page(ResolverConsumer& resolver, core::TdClient& client,
                                            std::string_view account, const ReadState& state,
                                            const ResolvedChatTarget& target,
                                            std::int64_t history_chat, std::int64_t from_message_id,
                                            std::int32_t limit, RequestSession& session) {
    return target_read(resolver, client, account, session, [&](const auto& current) {
        if (state.local || !state.topic) {
            return client.get_chat_history(current, history_chat, from_message_id, 0, limit,
                                           state.local);
        }
        switch (state.topic->kind) {
        case TopicKind::Forum:
            return client.get_forum_topic_history(current, target.chat.id,
                                                  static_cast<std::int32_t>(state.topic->id),
                                                  from_message_id, 0, limit);
        case TopicKind::Thread:
            return client.get_message_thread_history(current, target.chat.id, state.topic->id,
                                                     from_message_id, 0, limit);
        case TopicKind::Direct:
            return client.get_direct_messages_chat_topic_history(
                current, target.chat.id, state.topic->id, from_message_id, 0, limit);
        case TopicKind::Saved:
            return client.get_saved_messages_topic_history(current, state.topic->id,
                                                           from_message_id, 0, limit);
        }
        return client.get_chat_history(current, history_chat, from_message_id, 0, limit, false);
    });
}

json read_result(const std::vector<MessageSummary>& messages,
                 const std::optional<std::string>& next, std::string_view boundary) {
    json items = json::array();
    for (const auto& message : messages) {
        items.push_back(message_summary_json(message));
    }
    return {{"items", std::move(items)},
            {"next", next ? json(*next) : json(nullptr)},
            {"boundary", boundary}};
}

} // namespace

// The ordered resolver, probe, and pagination state machine shares one deadline-bound consumer.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ReadCoordinator::read(const proto::Request& request, RequestSession& session) {
    const auto request_start = wall_clock_ ? wall_clock_() : std::chrono::system_clock::now();
    const auto parsed = parse_request(request.args, request_start, session);
    if (!parsed) {
        return;
    }
    ReadState state = *parsed;
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(M2Operation::Read);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_resolver_error(*error, session, M2Operation::Read);
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (principal.is_bot) {
        session.error("BOT_UNSUPPORTED", "read requires a user account", {{"operation", "read"}},
                      kUsage);
        return;
    }
    if (state.cursor && (state.cursor->operation != "read" || state.cursor->account != account_ ||
                         state.cursor->user_id != principal.id)) {
        usage(session, "read cursor scope does not match this request", "--cursor",
              "cursor_scope_mismatch");
        return;
    }
    const auto target = resolve_target(resolver, state, session);
    if (!target) {
        return;
    }
    if (state.cursor && target->chat.id != state.cursor->chat_id) {
        usage(session, "read cursor scope does not match this request", "--cursor",
              "cursor_scope_mismatch");
        return;
    }
    if (state.topic && state.topic->kind == TopicKind::Saved &&
        !check_saved_ownership(resolver, client_.get(), account_, *target, principal, session)) {
        return;
    }
    const auto history_chat =
        history_chat_id(resolver, client_.get(), account_, state, *target, session);
    if (!history_chat) {
        return;
    }
    if (state.cursor && *history_chat != state.cursor->history_chat_id) {
        usage(session, "read cursor scope does not match current thread metadata", "--cursor",
              "cursor_scope_mismatch");
        return;
    }

    std::optional<std::int64_t> since_cutoff =
        state.cursor ? state.cursor->since_cutoff_message_id : std::nullopt;
    std::int64_t from_message_id =
        state.cursor ? state.cursor->from_message_id : state.before.value_or(0);
    bool exclusive = state.cursor.has_value() || state.before.has_value();

    if (!state.cursor && !state.local && state.until) {
        const auto probe =
            date_probe(resolver, client_.get(), account_, *history_chat, *state.until, session);
        if (probe.status == ProbeStatus::Failed) {
            return;
        }
        if (probe.status == ProbeStatus::Missing) {
            session.result(read_result({}, std::nullopt, "empty_before_until"));
            return;
        }
        if (!state.before) {
            from_message_id = probe.message->id;
            exclusive = false;
        }
    }
    if (!state.cursor && !state.local && state.since &&
        *state.since != std::numeric_limits<std::int32_t>::min()) {
        const auto requested = static_cast<std::int32_t>(*state.since - 1);
        const auto probe =
            date_probe(resolver, client_.get(), account_, *history_chat, requested, session);
        if (probe.status == ProbeStatus::Failed) {
            return;
        }
        if (probe.status == ProbeStatus::Message) {
            since_cutoff = probe.message->id;
        }
    }

    std::vector<MessageSummary> items;
    std::optional<std::int64_t> last_consumed;
    for (;;) {
        const auto remaining = state.limit - static_cast<std::int32_t>(items.size());
        const auto request_limit = remaining + (exclusive ? 1 : 0);
        const auto response = history_page(resolver, client_.get(), account_, state, *target,
                                           *history_chat, from_message_id, request_limit, session);
        if (!response) {
            return;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            if (state.topic && error->code == 404) {
                topic_not_found(session, target->chat.id, *state.topic);
            } else {
                td_error(session, *error);
            }
            return;
        }
        const auto* page = response->value.get_if<core::TdMessages>();
        if (page == nullptr) {
            internal(session);
            return;
        }
        const auto scanned = scan_read_page(
            *page, {.history_chat_id = *history_chat,
                    .topic = state.local ? state.topic : std::nullopt,
                    .since = state.since,
                    .until = state.until,
                    .since_cutoff_message_id = since_cutoff,
                    .exclusive_anchor =
                        exclusive ? std::optional<std::int64_t>{from_message_id} : std::nullopt,
                    .remaining = remaining});
        if (scanned.error == ReadScanError::Internal) {
            internal(session);
            return;
        }
        if (scanned.error == ReadScanError::NonAdvancing) {
            session.error("PAGINATION_INVALID", "read history did not advance",
                          {{"operation", "read"}, {"reason", "non_advancing_upstream"}}, kGeneric);
            return;
        }
        items.insert(items.end(), scanned.items.begin(), scanned.items.end());
        if (scanned.last_consumed_message_id) {
            last_consumed = scanned.last_consumed_message_id;
            from_message_id = *last_consumed;
            exclusive = true;
        }
        if (scanned.reached_time_anchor) {
            session.result(read_result(items, std::nullopt, "time_anchor"));
            return;
        }
        if (static_cast<std::int32_t>(items.size()) == state.limit) {
            const ReadCursor cursor{.version = 1,
                                    .operation = "read",
                                    .account = account_,
                                    .user_id = principal.id,
                                    .limit = state.limit,
                                    .chat_id = target->chat.id,
                                    .history_chat_id = *history_chat,
                                    .topic = state.topic,
                                    .local = state.local,
                                    .since = state.since,
                                    .until = state.until,
                                    .since_cutoff_message_id = since_cutoff,
                                    .from_message_id = *last_consumed};
            session.result(read_result(items, encode_read_cursor(cursor), "page"));
            return;
        }
        if (!scanned.last_consumed_message_id) {
            if (!last_consumed) {
                session.result(read_result(items, std::nullopt,
                                           state.local ? "local_boundary" : "tdlib_idle"));
                return;
            }
            const ReadCursor cursor{.version = 1,
                                    .operation = "read",
                                    .account = account_,
                                    .user_id = principal.id,
                                    .limit = state.limit,
                                    .chat_id = target->chat.id,
                                    .history_chat_id = *history_chat,
                                    .topic = state.topic,
                                    .local = state.local,
                                    .since = state.since,
                                    .until = state.until,
                                    .since_cutoff_message_id = since_cutoff,
                                    .from_message_id = *last_consumed};
            session.result(read_result(items, encode_read_cursor(cursor), "page"));
            return;
        }
    }
}

void register_read_command(Dispatcher& dispatcher, ReadCoordinator& coordinator) {
    dispatcher.register_command("read", {Tier::Read, [&coordinator](const proto::Request& request,
                                                                    RequestSession& session) {
                                             coordinator.read(request, session);
                                         }});
}

} // namespace tgcli::daemon
