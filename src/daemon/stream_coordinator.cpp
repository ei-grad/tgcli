#include "daemon/stream_coordinator.hpp"

#include "common/exit_codes.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "daemon/stream_commands.hpp"
#include "daemon/stream_service.hpp"
#include "daemon/stream_wait_scanner.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

void usage(RequestSession& session, const StreamArgumentError& error) {
    session.error("USAGE", error.message,
                  {{"argument", error.argument.empty() ? json(nullptr) : json(error.argument)},
                   {"reason", error.reason}},
                  kUsage);
}

void emit_frame(RequestSession& session, StreamTerminalFrame frame) {
    if (auto* result = std::get_if<StreamTerminalResultFrame>(&frame)) {
        static_cast<void>(session.result(std::move(result->data)));
        return;
    }
    if (auto* error = std::get_if<StreamTerminalErrorFrame>(&frame)) {
        static_cast<void>(session.error(std::move(error->code), std::move(error->message),
                                        std::move(error->details), error->exit_code));
        return;
    }
    static_cast<void>(session.error("INTERNAL", "stream setup failed",
                                    {{"operation", "listen"}, {"reason", "internal_error"}},
                                    kGeneric));
}

bool emit_resolver_stop(const ResolverPrincipalOutcome& outcome, RequestSession& session) {
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, M2Operation::Resolve);
        return true;
    }
    return std::holds_alternative<ResolverStop>(outcome);
}

bool emit_resolver_stop(const ResolverOutcome& outcome, RequestSession& session) {
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, M2Operation::Resolve);
        return true;
    }
    return std::holds_alternative<ResolverStop>(outcome);
}

bool emit_resolver_stop(const UserResolverOutcome& outcome, RequestSession& session) {
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, M2Operation::Resolve);
        return true;
    }
    return std::holds_alternative<ResolverStop>(outcome);
}

struct ResolvedStreamSetup {
    ResolverPrincipal principal;
    std::vector<std::int64_t> chat_ids;
    std::optional<core::TdChat> user_domain;
    std::optional<std::int64_t> sender_user_id;
    std::shared_ptr<const core::AuthStateSnapshot> authorization;
};

std::optional<ResolvedStreamSetup> resolve_setup(core::TdClient& client, std::string_view account,
                                                 RequestSession& session, StreamOperation operation,
                                                 const std::vector<std::string>& chat_selectors,
                                                 const std::optional<std::string>& from_selector,
                                                 bool reject_bot_after) {
    ResolverConsumer resolver(client, account, session);
    const auto principal = resolver.bind_principal(
        operation == StreamOperation::Listen ? M2Operation::Listen : M2Operation::WaitFor);
    if (emit_resolver_stop(principal, session)) {
        return std::nullopt;
    }
    ResolvedStreamSetup result{.principal = std::get<ResolverPrincipal>(principal),
                               .chat_ids = {},
                               .user_domain = std::nullopt,
                               .sender_user_id = std::nullopt,
                               .authorization = nullptr};
    if (reject_bot_after && result.principal.is_bot) {
        session.error("BOT_UNSUPPORTED", "wait-for --after requires a user account",
                      {{"operation", "wait_for"}}, kUsage);
        return std::nullopt;
    }

    for (const auto& selector : chat_selectors) {
        auto resolved = resolver.resolve_chat(selector, ResolverScope::ActiveDialogs);
        if (emit_resolver_stop(resolved, session)) {
            return std::nullopt;
        }
        auto target = std::get<ResolvedChatTarget>(std::move(resolved));
        result.chat_ids.push_back(target.chat.id);
        if (chat_selectors.size() == 1) {
            result.user_domain = std::move(target.observed_chat);
        }
    }
    std::ranges::sort(result.chat_ids);
    result.chat_ids.erase(std::unique(result.chat_ids.begin(), result.chat_ids.end()),
                          result.chat_ids.end());

    if (from_selector) {
        auto resolved = resolver.resolve_user(*from_selector, result.user_domain);
        if (emit_resolver_stop(resolved, session)) {
            return std::nullopt;
        }
        result.sender_user_id = std::get<UserIdentity>(std::move(resolved)).id;
    }
    result.authorization = resolver.bound_authorization();
    if (!result.authorization) {
        emit_resolver_error(ResolverError{ResolverInternalError{.operation = M2Operation::Resolve}},
                            session, M2Operation::Resolve);
        return std::nullopt;
    }
    return result;
}

bool same_ready_authorization(const std::shared_ptr<const core::AuthStateSnapshot>& bound,
                              const std::shared_ptr<const core::AuthStateSnapshot>& current) {
    return bound && current && current->data.state == core::AuthState::Ready &&
           current->client_id == bound->client_id &&
           current->client_generation == bound->client_generation &&
           current->auth_sequence == bound->auth_sequence;
}

void emit_setup_authorization_lost(RequestSession& session, std::string_view command,
                                   std::string_view account,
                                   const std::shared_ptr<const core::AuthStateSnapshot>& current) {
    session.error(
        "NOT_AUTHED", std::string(command) + " requires an authenticated account",
        {{"account", account},
         {"state", current ? json(core::auth_state_name(current->data.state)) : json("unknown")},
         {"reason", "authorization_lost"}},
        kNotAuthed);
}

StreamIngressRequest ingress_request(const core::AuthStateSnapshot& snapshot,
                                     StreamOperation operation, std::uint8_t type_mask,
                                     const std::vector<std::int64_t>& chat_ids) {
    StreamIngressRequest result{.client_id = snapshot.client_id,
                                .generation = snapshot.client_generation,
                                .operation = operation,
                                .mode = operation == StreamOperation::Listen ? StreamMode::Items
                                                                             : StreamMode::Match,
                                .type_mask = type_mask};
    std::ranges::copy(chat_ids, result.chat_ids.begin());
    result.chat_count = static_cast<std::uint8_t>(chat_ids.size());
    return result;
}

bool activate(StreamService& service, RequestSession& session, StreamActivityMode activity_mode,
              const StreamIngressRequest& request, std::string_view account) {
    const auto activation =
        session.activate_stream_subscription(service.ingress_hub_handle(), request, activity_mode);
    if (std::holds_alternative<StreamSubscriptionActivated>(activation)) {
        return true;
    }
    if (const auto* failure = std::get_if<StreamIngressAdmissionFailure>(&activation)) {
        auto frame = stream_admission_error(*failure, request.operation);
        static_cast<void>(session.error(std::move(frame.code), std::move(frame.message),
                                        std::move(frame.details), frame.exit_code));
        return false;
    }
    if (const auto* terminal = std::get_if<StreamSubscriptionTerminalClaimed>(&activation)) {
        emit_frame(session, stream_terminal_frame(terminal->terminal, 0, account));
        return false;
    }
    if (const auto* failure = std::get_if<StreamSubscriptionActivationFailure>(&activation);
        failure != nullptr && *failure == StreamSubscriptionActivationFailure::RequestClosed) {
        return false;
    }
    session.error(
        "INTERNAL", "stream activation failed",
        {{"operation", request.operation == StreamOperation::Listen ? "listen" : "wait_for"},
         {"reason", "internal_error"}},
        kGeneric);
    return false;
}

StreamTerminalBuilder terminal_builder(std::string_view account) {
    return [account](const StreamTerminalPayload& terminal, std::uint64_t delivered) {
        return stream_terminal_frame(terminal, delivered, account);
    };
}

std::optional<nlohmann::json> live_match(const StreamCopiedItem& item,
                                         const StreamMessageMatcher& matcher) {
    const auto message = parse_stream_message_item(item);
    return message && matcher.matches(*message)
               ? std::optional<nlohmann::json>{message_summary_json(*message)}
               : std::nullopt;
}

void notify(const testing::StreamCoordinatorProbe& probe,
            testing::StreamCoordinatorProbePoint point) noexcept {
    if (probe.hook != nullptr) {
        probe.hook(probe.context, point);
    }
}

} // namespace

void StreamCoordinator::listen(const proto::Request& request, RequestSession& session) {
    if (const auto error = validate_stream_timeout(request.context.timeout_seconds)) {
        usage(session, *error);
        return;
    }
    auto parsed = parse_listen_arguments(request.args);
    if (const auto* error = std::get_if<StreamArgumentError>(&parsed)) {
        usage(session, *error);
        return;
    }
    auto arguments = std::get<ListenArguments>(std::move(parsed));
    auto setup = resolve_setup(client_.get(), account_, session, StreamOperation::Listen,
                               arguments.chat_selectors, std::nullopt, false);
    if (!setup) {
        return;
    }
    notify(probe_, testing::StreamCoordinatorProbePoint::AfterResolve);
    const auto current = client_.get().auth_state();
    if (!same_ready_authorization(setup->authorization, current)) {
        emit_setup_authorization_lost(session, "listen", account_, current);
        return;
    }
    const auto ingress = ingress_request(*setup->authorization, StreamOperation::Listen,
                                         arguments.type_mask, setup->chat_ids);
    if (!activate(service_.get(), session, activity_mode_, ingress, account_)) {
        return;
    }
    static_cast<void>(run_stream_delivery(session, {.count = arguments.count,
                                                    .terminal_builder = terminal_builder(account_),
                                                    .hooks = nullptr}));
}

void StreamCoordinator::wait_for(const proto::Request& request, RequestSession& session) {
    if (const auto error = validate_stream_timeout(request.context.timeout_seconds)) {
        usage(session, *error);
        return;
    }
    auto parsed = parse_wait_for_arguments(request.args);
    if (const auto* error = std::get_if<StreamArgumentError>(&parsed)) {
        usage(session, *error);
        return;
    }
    auto arguments = std::get<WaitForArguments>(std::move(parsed));
    std::shared_ptr<const StreamRegex> regex;
    if (arguments.regex_pattern) {
        auto compiled = compile_stream_regex(*arguments.regex_pattern);
        if (const auto* error = std::get_if<StreamArgumentError>(&compiled)) {
            usage(session, *error);
            return;
        }
        regex = std::make_shared<StreamRegex>(std::move(std::get<StreamRegex>(compiled)));
    }
    std::vector<std::string> chats;
    if (arguments.chat_selector) {
        chats.push_back(*arguments.chat_selector);
    }
    auto setup = resolve_setup(client_.get(), account_, session, StreamOperation::WaitFor, chats,
                               arguments.from_selector, arguments.after.has_value());
    if (!setup) {
        return;
    }
    notify(probe_, testing::StreamCoordinatorProbePoint::AfterResolve);
    const auto current = client_.get().auth_state();
    if (!same_ready_authorization(setup->authorization, current)) {
        emit_setup_authorization_lost(session, "wait-for", account_, current);
        return;
    }
    const auto ingress =
        ingress_request(*setup->authorization, StreamOperation::WaitFor,
                        stream_event_mask(StreamEventClass::Message), setup->chat_ids);
    if (!activate(service_.get(), session, activity_mode_, ingress, account_)) {
        return;
    }
    const StreamMessageMatcher matcher{.sender_user_id = setup->sender_user_id,
                                       .regex = std::move(regex)};
    if (!arguments.after) {
        static_cast<void>(run_stream_match_delivery(
            session,
            {.initial_match = std::nullopt,
             .item_matcher =
                 [matcher](const StreamCopiedItem& item) { return live_match(item, matcher); },
             .terminal_builder = terminal_builder(account_),
             .hooks = nullptr}));
        return;
    }

    auto worker = session.stream_worker();
    auto scanned = scan_wait_history(
        session, worker,
        {.chat_id = setup->chat_ids.front(),
         .after = *arguments.after,
         .matcher = matcher,
         .start_history =
             [this, authorization = setup->authorization](const StreamHistoryRequest& history) {
                 return client_.get().get_chat_history(authorization, history.chat_id,
                                                       history.from_message_id, history.offset,
                                                       history.limit, history.only_local);
             },
         .hooks = nullptr});
    if (!scanned.state) {
        static_cast<void>(run_stream_match_delivery(
            session, {.initial_match = std::nullopt,
                      .item_matcher = [](const StreamCopiedItem&) -> std::optional<json> {
                          return std::nullopt;
                      },
                      .terminal_builder = terminal_builder(account_),
                      .hooks = nullptr}));
        return;
    }
    auto match_state = std::move(scanned.state);
    static_cast<void>(run_stream_match_delivery(
        session,
        {.initial_match = match_state->initial_match(),
         .item_matcher =
             [match_state](const StreamCopiedItem& item) { return match_state->match_live(item); },
         .terminal_builder = terminal_builder(account_),
         .hooks = nullptr}));
}

void register_stream_commands(Dispatcher& dispatcher, StreamCoordinator& coordinator) {
    dispatcher.register_command(
        "listen", {Tier::Read,
                   [&coordinator](const proto::Request& request, RequestSession& session) {
                       coordinator.listen(request, session);
                   },
                   false, std::nullopt, DeadlineDefault::Unlimited});
    dispatcher.register_command(
        "wait-for", {Tier::Read,
                     [&coordinator](const proto::Request& request, RequestSession& session) {
                         coordinator.wait_for(request, session);
                     },
                     false, std::nullopt, DeadlineDefault::Unlimited});
}

} // namespace tgcli::daemon
