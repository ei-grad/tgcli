#include "daemon/message_commands.hpp"

#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/message_summary.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9007199254740991LL;
constexpr std::size_t kMaximumMessageIds = 100;

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

void usage(RequestSession& session, std::string_view message, const json& argument,
           std::string_view reason = "invalid_argument") {
    session.error("USAGE", std::string(message), {{"argument", argument}, {"reason", reason}},
                  kUsage);
}

void internal(RequestSession& session, M2Operation operation) {
    session.error(
        "INTERNAL", std::string(m2_operation_name(operation)) + " returned an unexpected object",
        {{"operation", m2_operation_name(operation)}, {"reason", "internal_error"}}, kGeneric);
}

void td_error(RequestSession& session, M2Operation operation, const core::TdError& error) {
    if (error.code == 429) {
        session.error("RATE_LIMITED", "Telegram rate limit",
                      {{"operation", m2_operation_name(operation)},
                       {"tdlib_code", 429},
                       {"retry_after", retry_after(error.message)}},
                      kRateLimited);
        return;
    }
    session.error(
        "TDLIB_ERROR", std::string(m2_operation_name(operation)) + " TDLib request failed",
        {{"operation", m2_operation_name(operation)}, {"tdlib_code", error.code}}, kGeneric);
}

bool handle_target_stop(const ReadyReadResult& result, core::TdClient& client,
                        std::string_view account, M2Operation operation, RequestSession& session) {
    switch (result.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        if (!session.cancellation_requested()) {
            session.error("NOT_AUTHED",
                          std::string(m2_operation_name(operation)) +
                              " requires an authenticated account",
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
            session.error("TIMEOUT",
                          std::string(m2_operation_name(operation)) + " request timed out",
                          {{"operation", m2_operation_name(operation)},
                           {"state", snapshot ? json(core::auth_state_name(snapshot->data.state))
                                              : json(nullptr)}},
                          kTimeout);
        }
        return true;
    }
    case ReadyReadStatus::Failed:
        if (!session.cancellation_requested()) {
            internal(session, operation);
        }
        return true;
    case ReadyReadStatus::Cancelled:
        return true;
    }
    return true;
}

std::optional<ResolvedChatTarget> resolve_target(ResolverConsumer& resolver, std::string selector,
                                                 M2Operation operation, RequestSession& session) {
    const auto principal = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal)) {
        emit_resolver_error(*error, session, operation);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(principal)) {
        return std::nullopt;
    }
    const auto outcome = resolver.resolve_chat(std::move(selector), ResolverScope::ActiveDialogs);
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, operation);
        return std::nullopt;
    }
    const auto* target = std::get_if<ResolvedChatTarget>(&outcome);
    return target == nullptr ? std::nullopt : std::optional<ResolvedChatTarget>{*target};
}

std::optional<std::vector<std::int64_t>> get_message_ids(const json& args,
                                                         RequestSession& session) {
    if (!exact_fields(args, {"chat", "message_ids"}) || !args["chat"].is_string() ||
        !args["message_ids"].is_array()) {
        usage(session, "msg get received malformed arguments", nullptr);
        return std::nullopt;
    }
    const auto& values = args["message_ids"];
    if (values.empty() || values.size() > kMaximumMessageIds) {
        usage(session, "msg get requires between 1 and 100 message ids", "id");
        return std::nullopt;
    }
    std::vector<std::int64_t> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        const auto id = integer64(value);
        if (!id || !valid_int53(*id)) {
            usage(session, "message ids must be nonzero int53 values", "id");
            return std::nullopt;
        }
        result.push_back(*id);
    }
    return result;
}

std::optional<std::int64_t> link_message_id(const json& args, RequestSession& session) {
    if (!exact_fields(args, {"chat", "message_id"}) || !args["chat"].is_string()) {
        usage(session, "msg link received malformed arguments", nullptr);
        return std::nullopt;
    }
    const auto id = integer64(args["message_id"]);
    if (!id || !valid_int53(*id)) {
        usage(session, "message id must be a nonzero int53 value", "id");
        return std::nullopt;
    }
    return id;
}

} // namespace

void MessageCoordinator::get(const proto::Request& request, RequestSession& session) {
    const auto message_ids = get_message_ids(request.args, session);
    if (!message_ids) {
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto target = resolve_target(resolver, request.args["chat"].get<std::string>(),
                                       M2Operation::MsgGet, session);
    if (!target) {
        return;
    }
    auto result = resolver.read_target([&](const auto& current) {
        return client_.get().get_messages(current, target->chat.id, *message_ids);
    });
    if (handle_target_stop(result, client_.get(), account_, M2Operation::MsgGet, session)) {
        return;
    }
    if (const auto* error = result.value.get_if<core::TdError>()) {
        td_error(session, M2Operation::MsgGet, *error);
        return;
    }
    const auto* messages = result.value.get_if<core::TdMessages>();
    if (messages == nullptr || messages->total_count < 0 ||
        messages->messages.size() != message_ids->size()) {
        internal(session, M2Operation::MsgGet);
        return;
    }
    std::vector<std::optional<MessageSummary>> converted;
    converted.reserve(messages->messages.size());
    bool invalid = false;
    for (std::size_t index = 0; index < messages->messages.size(); ++index) {
        const auto& source = messages->messages[index];
        if (!source) {
            converted.emplace_back(std::nullopt);
            continue;
        }
        auto message = materialize_message_summary(source.value());
        if (!message || message->chat_id != target->chat.id ||
            message->id != (*message_ids)[index]) {
            invalid = true;
        }
        converted.push_back(std::move(message));
    }
    if (invalid) {
        internal(session, M2Operation::MsgGet);
        return;
    }
    std::vector<std::int64_t> missing;
    std::unordered_set<std::int64_t> seen_missing;
    for (std::size_t index = 0; index < converted.size(); ++index) {
        if (!converted[index] && seen_missing.insert((*message_ids)[index]).second) {
            missing.push_back((*message_ids)[index]);
        }
    }
    if (!missing.empty()) {
        session.error("NOT_FOUND", "one or more messages were not found",
                      {{"chat_id", target->chat.id}, {"missing_ids", missing}}, kNotFound);
        return;
    }
    json items = json::array();
    for (const auto& message : converted) {
        if (!message) {
            internal(session, M2Operation::MsgGet);
            return;
        }
        items.push_back(message_summary_json(message.value()));
    }
    session.result({{"items", std::move(items)}, {"next", nullptr}});
}

void MessageCoordinator::link(const proto::Request& request, RequestSession& session) {
    const auto message_id = link_message_id(request.args, session);
    if (!message_id) {
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto target = resolve_target(resolver, request.args["chat"].get<std::string>(),
                                       M2Operation::MsgLink, session);
    if (!target) {
        return;
    }
    auto result = resolver.read_target([&](const auto& current) {
        return client_.get().get_message_link(current, target->chat.id, *message_id, 0, 0, "",
                                              false, false);
    });
    if (handle_target_stop(result, client_.get(), account_, M2Operation::MsgLink, session)) {
        return;
    }
    if (const auto* error = result.value.get_if<core::TdError>()) {
        if (error->code == 404) {
            session.error("NOT_FOUND", "message was not found",
                          {{"chat_id", target->chat.id}, {"message_id", *message_id}}, kNotFound);
        } else {
            td_error(session, M2Operation::MsgLink, *error);
        }
        return;
    }
    const auto* link = result.value.get_if<core::TdMessageLink>();
    if (link == nullptr || link->link.empty() || !common::valid_utf8(link->link)) {
        internal(session, M2Operation::MsgLink);
        return;
    }
    session.result({{"chat_id", target->chat.id},
                    {"message_id", *message_id},
                    {"link", link->link},
                    {"is_public", link->is_public}});
}

void register_message_commands(Dispatcher& dispatcher, MessageCoordinator& coordinator) {
    dispatcher.register_command(
        "msg get",
        {.tier = Tier::Read,
         .handler = [&coordinator](const proto::Request& request,
                                   RequestSession& session) { coordinator.get(request, session); },
         .config_admission = true});
    dispatcher.register_command(
        "msg link",
        {.tier = Tier::Read,
         .handler = [&coordinator](const proto::Request& request,
                                   RequestSession& session) { coordinator.link(request, session); },
         .config_admission = true});
}

} // namespace tgcli::daemon
