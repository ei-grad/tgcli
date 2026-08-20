#include "daemon/fetch_commands.hpp"

#include "common/exit_codes.hpp"
#include "daemon/fetch_domain.hpp"
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

struct FetchRequest {
    std::string selector;
    FetchTarget target;
};

enum class FetchPhase { LocalScan, NetworkFill };

struct FetchRunState {
    std::int64_t chat_id = 0;
    FetchTarget target;
    FetchPhase phase = FetchPhase::LocalScan;
    std::optional<std::int64_t> since_cutoff_message_id;
    std::uint64_t cached_count = 0;
    std::optional<std::int64_t> oldest_message_id;
    bool numeric_latched = false;
    bool since_latched = false;
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

void usage(RequestSession& session, std::string_view message, const json& argument,
           std::string_view reason = "invalid_argument") {
    session.error("USAGE", std::string(message), {{"argument", argument}, {"reason", reason}},
                  kUsage);
}

void internal(RequestSession& session) {
    session.error("INTERNAL", "fetch returned an unexpected object",
                  {{"operation", "fetch"}, {"reason", "internal_error"}}, kGeneric);
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
                      {{"operation", "fetch"},
                       {"tdlib_code", 429},
                       {"retry_after", retry_after(error.message)}},
                      kRateLimited);
        return;
    }
    session.error("TDLIB_ERROR", "fetch TDLib request failed",
                  {{"operation", "fetch"}, {"tdlib_code", error.code}}, kGeneric);
}

json auth_state_json(core::TdClient& client) {
    const auto snapshot = client.auth_state();
    return snapshot ? json(core::auth_state_name(snapshot->data.state)) : json(nullptr);
}

json timeout_details(core::TdClient& client, const FetchRunState& state) {
    const auto boundary = state.oldest_message_id ? json(*state.oldest_message_id) : json(nullptr);
    return {{"operation", "fetch"},
            {"chat_id", state.chat_id},
            {"phase", state.phase == FetchPhase::LocalScan ? "local_scan" : "network_fill"},
            {"state", auth_state_json(client)},
            {"cached_count", state.cached_count},
            {"oldest_message_id", boundary},
            {"resume_from_message_id", boundary}};
}

bool handle_target_stop(const ReadyReadResult& result, core::TdClient& client,
                        std::string_view account, const FetchRunState& state,
                        RequestSession& session) {
    switch (result.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        if (!session.cancellation_requested()) {
            session.error("NOT_AUTHED", "fetch requires an authenticated account",
                          {{"account", account},
                           {"state", result.snapshot
                                         ? json(core::auth_state_name(result.snapshot->data.state))
                                         : json("unknown")},
                           {"reason", "authorization_lost"}},
                          kNotAuthed);
        }
        return true;
    case ReadyReadStatus::TimedOut:
        if (!session.cancellation_requested()) {
            session.error("TIMEOUT", "fetch request timed out", timeout_details(client, state),
                          kTimeout);
        }
        return true;
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
                                           std::string_view account, const FetchRunState& state,
                                           RequestSession& session, const ReadyReadStart& start) {
    auto result = resolver.read_target(start);
    if (handle_target_stop(result, client, account, state, session)) {
        return std::nullopt;
    }
    return result;
}

std::optional<FetchRequest> parse_request(const json& args,
                                          std::chrono::system_clock::time_point request_start,
                                          RequestSession& session) {
    if (!exact_fields(args, {"all", "chat", "limit", "since"}) || !args["all"].is_boolean() ||
        !args["chat"].is_string() ||
        !(args["limit"].is_null() || args["limit"].is_number_integer()) ||
        !(args["since"].is_null() || args["since"].is_string())) {
        usage(session, "fetch received malformed arguments", nullptr);
        return std::nullopt;
    }

    FetchRequest parsed;
    parsed.selector = args["chat"].get<std::string>();
    if (!valid_resolve_selector(parsed.selector)) {
        usage(session, "fetch selector is invalid", "selector");
        return std::nullopt;
    }
    parsed.target.all = args["all"].get<bool>();
    if (!args["limit"].is_null()) {
        const auto limit = integer64(args["limit"]);
        if (!limit || *limit < 1 || *limit > kMaximumFetchLimit) {
            usage(session, "fetch limit must be between 1 and 1000000", "--limit");
            return std::nullopt;
        }
        parsed.target.limit = static_cast<std::int32_t>(*limit);
    }
    if (parsed.target.all && parsed.target.limit) {
        usage(session, "--limit and --all are mutually exclusive", "--limit/--all",
              "mutually_exclusive");
        return std::nullopt;
    }
    if (!args["since"].is_null()) {
        parsed.target.since = parse_read_timestamp(args["since"].get_ref<const std::string&>(),
                                                   ReadTimestampBound::Since, request_start);
        if (!parsed.target.since) {
            usage(session, "invalid --since timestamp", "--since");
            return std::nullopt;
        }
    }
    if (!parsed.target.limit && !parsed.target.all && !parsed.target.since) {
        parsed.target.limit = kDefaultFetchLimit;
    }
    if (!valid_fetch_target(parsed.target) ||
        (parsed.target.since && !format_fetch_timestamp(*parsed.target.since))) {
        internal(session);
        return std::nullopt;
    }
    return parsed;
}

void emit_fetch_resolver_error(const ResolverError& error, core::TdClient& client,
                               RequestSession& session) {
    if (const auto* timeout = std::get_if<ResolverTimeoutError>(&error)) {
        session.error("TIMEOUT", "fetch request timed out",
                      {{"operation", "fetch"},
                       {"state", timeout->state ? json(core::auth_state_name(*timeout->state))
                                                : auth_state_json(client)}},
                      kTimeout);
        return;
    }
    emit_resolver_error(error, session, M2Operation::Fetch);
}

std::optional<ResolvedChatTarget> resolve_target(ResolverConsumer& resolver, std::string selector,
                                                 core::TdClient& client, RequestSession& session) {
    const auto outcome = resolver.resolve_chat(std::move(selector), ResolverScope::ActiveDialogs);
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_fetch_resolver_error(*error, client, session);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    const auto* target = std::get_if<ResolvedChatTarget>(&outcome);
    return target == nullptr ? std::nullopt : std::optional<ResolvedChatTarget>{*target};
}

enum class ProbeStatus { Message, Missing, Failed };

struct ProbeResult {
    ProbeStatus status = ProbeStatus::Failed;
    std::optional<std::int64_t> message_id;
};

ProbeResult date_probe(ResolverConsumer& resolver, core::TdClient& client, std::string_view account,
                       FetchRunState& state, std::int32_t requested_date, RequestSession& session) {
    const auto response =
        target_read(resolver, client, account, state, session, [&](const auto& current) {
            return client.get_chat_message_by_date(current, state.chat_id, requested_date);
        });
    if (!response) {
        return {};
    }
    if (const auto* error = response->value.get_if<core::TdError>()) {
        if (error->code == 404) {
            return {.status = ProbeStatus::Missing, .message_id = std::nullopt};
        }
        td_error(session, *error);
        return {};
    }
    const auto* raw = response->value.get_if<core::TdMessageSummary>();
    const auto materialized = raw == nullptr ? std::nullopt : materialize_message_summary(*raw);
    if (raw == nullptr || !materialized || materialized->chat_id != state.chat_id ||
        raw->date == 0 || raw->date > requested_date) {
        internal(session);
        return {};
    }
    return {.status = ProbeStatus::Message, .message_id = materialized->id};
}

std::optional<ReadyReadResult> history_page(ResolverConsumer& resolver, core::TdClient& client,
                                            std::string_view account, const FetchRunState& state,
                                            RequestSession& session) {
    const auto anchor = state.oldest_message_id.value_or(0);
    return target_read(resolver, client, account, state, session, [&](const auto& current) {
        return client.get_chat_history(current, state.chat_id, anchor, 0, kFetchPageLimit,
                                       state.phase == FetchPhase::LocalScan);
    });
}

bool emit_result(RequestSession& session, const FetchRunState& state, FetchStopReason reason,
                 bool terminal_page_advanced) {
    const auto result = make_fetch_result(
        state.chat_id, state.cached_count, state.oldest_message_id, state.target,
        {.stop_reason = reason,
         .numeric_latched = state.numeric_latched,
         .since_latched = state.since_latched,
         .local_boundary_sealed = state.phase == FetchPhase::NetworkFill || !terminal_page_advanced,
         .network_fill_started = state.phase == FetchPhase::NetworkFill,
         .terminal_page_advanced = terminal_page_advanced});
    if (!result) {
        internal(session);
        return false;
    }
    session.result(*result);
    return true;
}

enum class PageDisposition { Advanced, Idle, Failed };

PageDisposition incorporate_page(const core::TdMessages& page, FetchRunState& state,
                                 RequestSession& session) {
    const auto scanned =
        scan_fetch_page(page, {.chat_id = state.chat_id,
                               .exclusive_anchor = state.oldest_message_id,
                               .since_cutoff_message_id = state.since_cutoff_message_id,
                               .cached_count = state.cached_count});
    switch (scanned.error) {
    case FetchScanError::None:
        break;
    case FetchScanError::Internal:
    case FetchScanError::Overflow:
        internal(session);
        return PageDisposition::Failed;
    case FetchScanError::NonAdvancing:
        session.error("PAGINATION_INVALID", "fetch history did not advance",
                      {{"operation", "fetch"}, {"reason", "non_advancing_upstream"}}, kGeneric);
        return PageDisposition::Failed;
    }
    if (scanned.added_count == 0) {
        return PageDisposition::Idle;
    }
    state.cached_count += scanned.added_count;
    state.oldest_message_id = scanned.oldest_message_id;
    state.numeric_latched = state.numeric_latched ||
                            (state.target.limit &&
                             state.cached_count >= static_cast<std::uint64_t>(*state.target.limit));
    state.since_latched = state.since_latched || scanned.since_anchor_observed;
    session.progress(make_fetch_progress(state.chat_id, state.cached_count, state.oldest_message_id,
                                         state.target));
    return PageDisposition::Advanced;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void FetchCoordinator::fetch(const proto::Request& request, RequestSession& session) {
    const auto request_start = wall_clock_ ? wall_clock_() : std::chrono::system_clock::now();
    const auto parsed = parse_request(request.args, request_start, session);
    if (!parsed) {
        return;
    }

    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(M2Operation::Fetch);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_fetch_resolver_error(*error, client_.get(), session);
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (principal.is_bot) {
        session.error("BOT_UNSUPPORTED", "fetch requires a user account", {{"operation", "fetch"}},
                      kUsage);
        return;
    }

    const auto target = resolve_target(resolver, parsed->selector, client_.get(), session);
    if (!target) {
        return;
    }

    FetchRunState state{.chat_id = target->chat.id,
                        .target = parsed->target,
                        .phase = FetchPhase::LocalScan,
                        .since_cutoff_message_id = std::nullopt,
                        .cached_count = 0,
                        .oldest_message_id = std::nullopt,
                        .numeric_latched = false,
                        .since_latched = false};
    if (state.target.since && *state.target.since != std::numeric_limits<std::int32_t>::min()) {
        const auto requested = static_cast<std::int32_t>(*state.target.since - 1);
        const auto probe = date_probe(resolver, client_.get(), account_, state, requested, session);
        if (probe.status == ProbeStatus::Failed) {
            return;
        }
        if (probe.status == ProbeStatus::Message) {
            state.since_cutoff_message_id = probe.message_id;
        }
    }

    for (;;) {
        const auto response = history_page(resolver, client_.get(), account_, state, session);
        if (!response) {
            return;
        }
        if (const auto* error = response->value.get_if<core::TdError>()) {
            td_error(session, *error);
            return;
        }
        const auto* page = response->value.get_if<core::TdMessages>();
        if (page == nullptr) {
            internal(session);
            return;
        }
        const auto disposition = incorporate_page(*page, state, session);
        if (disposition == PageDisposition::Failed) {
            return;
        }
        if (disposition == PageDisposition::Advanced) {
            if (state.phase == FetchPhase::NetworkFill) {
                if (state.since_latched) {
                    static_cast<void>(
                        emit_result(session, state, FetchStopReason::SinceAnchorReached, true));
                    return;
                }
                if (state.numeric_latched) {
                    static_cast<void>(
                        emit_result(session, state, FetchStopReason::TargetReached, true));
                    return;
                }
            }
            continue;
        }

        if (state.phase == FetchPhase::LocalScan) {
            if (state.since_latched) {
                static_cast<void>(
                    emit_result(session, state, FetchStopReason::SinceAnchorReached, false));
                return;
            }
            if (state.numeric_latched) {
                static_cast<void>(
                    emit_result(session, state, FetchStopReason::TargetReached, false));
                return;
            }
            state.phase = FetchPhase::NetworkFill;
            continue;
        }
        static_cast<void>(emit_result(session, state, FetchStopReason::TdlibIdle, false));
        return;
    }
}

void register_fetch_command(Dispatcher& dispatcher, FetchCoordinator& coordinator) {
    dispatcher.register_command(
        "fetch", {Tier::Read,
                  [&coordinator](const proto::Request& request, RequestSession& session) {
                      coordinator.fetch(request, session);
                  },
                  false, std::nullopt, DeadlineDefault::Unlimited});
}

} // namespace tgcli::daemon
