#include "daemon/m6_commands.hpp"

#include "common/exit_codes.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/m6_domain.hpp"
#include "daemon/m6_model.hpp"
#include "daemon/m6_topic_scan.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "daemon/write_commands.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields,
                               [&](std::string_view field) { return value.contains(field); });
}

std::string_view operation_name(proto::M6Operation operation) {
    const auto* identity = proto::m6_operation_identity(operation);
    return identity == nullptr ? std::string_view{} : identity->canonical_name;
}

std::string_view spool_reason_name(DurabilityReason reason) {
    switch (reason) {
    case DurabilityReason::PathInvalid:
        return "path_invalid";
    case DurabilityReason::WrongOwner:
        return "wrong_owner";
    case DurabilityReason::WrongType:
        return "wrong_type";
    case DurabilityReason::WrongMode:
        return "wrong_mode";
    case DurabilityReason::OpenFailed:
        return "open_failed";
    case DurabilityReason::ReadFailed:
        return "read_failed";
    default:
        return "contradiction";
    }
}

void usage(RequestSession& session, std::string_view message, const json& argument) {
    session.error("USAGE", std::string(message),
                  {{"argument", argument}, {"reason", "invalid_argument"}}, kUsage);
}

void internal(RequestSession& session, std::string_view operation) {
    session.error("INTERNAL", std::string(operation) + " returned an unexpected object",
                  {{"operation", operation}, {"reason", "malformed_tdlib_response"}}, kGeneric);
}

void capacity(RequestSession& session, std::string_view operation, std::string_view resource,
              std::size_t limit) {
    session.error("INTERNAL", std::string(operation) + " exceeded its bounded accumulator",
                  {{"operation", operation},
                   {"reason", "capacity_exhausted"},
                   {"resource", resource},
                   {"limit", limit}},
                  kGeneric);
}

std::int32_t retry_after(std::string_view message) {
    static const std::regex pattern(
        R"((?:^|[^[:alnum:]_])(?:retry[[:space:]]+after[[:space:]]*|FLOOD_WAIT_)([0-9]+))",
        std::regex::icase);
    std::cmatch match;
    if (!std::regex_search(message.begin(), message.end(), match, pattern) || match.size() != 2) {
        return 0;
    }
    std::int32_t value = 0;
    for (const char character : match[1].str()) {
        const auto digit = static_cast<std::int32_t>(character - '0');
        value = value > (std::numeric_limits<std::int32_t>::max() - digit) / 10
                    ? std::numeric_limits<std::int32_t>::max()
                    : value * 10 + digit;
    }
    return value;
}

void td_error(RequestSession& session, const ResolverCaller& caller, const core::TdError& error) {
    const auto operation = resolver_caller_name(caller);
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

std::optional<ResolverPrincipal> bind(ResolverConsumer& resolver, const ResolverCaller& caller,
                                      RequestSession& session, bool user_only) {
    const auto outcome = resolver.bind_principal(caller);
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, caller);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    const auto principal = std::get<ResolverPrincipal>(outcome);
    if (user_only && principal.is_bot) {
        session.error("BOT_UNSUPPORTED", "this command requires a user account",
                      {{"operation", resolver_caller_name(caller)}}, kUsage);
        return std::nullopt;
    }
    return principal;
}

bool stopped(const ReadyReadResult& result, core::TdClient& client, std::string_view account,
             const ResolverCaller& caller, RequestSession& session) {
    switch (result.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        emit_resolver_error(
            ResolverError{ResolverNotAuthenticatedError{
                .account = std::string(account),
                .state = result.snapshot ? result.snapshot->data.state : core::AuthState::Unknown,
                .reason = ResolverNotAuthedReason::AuthorizationLost}},
            session, caller);
        return true;
    case ReadyReadStatus::TimedOut: {
        const auto current = client.auth_state();
        emit_resolver_error(
            ResolverError{ResolverTimeoutError{.operation = caller,
                                               .state = current ? std::optional{current->data.state}
                                                                : std::nullopt}},
            session, caller);
        return true;
    }
    case ReadyReadStatus::Failed:
        emit_resolver_error(ResolverError{ResolverInternalError{.operation = caller}}, session,
                            caller);
        return true;
    case ReadyReadStatus::Cancelled:
        if (session.shutdown_requested()) {
            session.error("DAEMON_SHUTDOWN", "daemon is shutting down",
                          {{"reason", "daemon_shutdown"}}, kGeneric);
        }
        return true;
    }
    return true;
}

class UpdateSubscription final {
  public:
    UpdateSubscription(core::TdClient& client, core::TdClient::UpdateHandler handler)
        : client_(client), id_(client.subscribe_updates(std::move(handler))) {}
    ~UpdateSubscription() {
        client_.get().unsubscribe_updates(id_);
    }
    UpdateSubscription(const UpdateSubscription&) = delete;
    UpdateSubscription& operator=(const UpdateSubscription&) = delete;
    UpdateSubscription(UpdateSubscription&&) = delete;
    UpdateSubscription& operator=(UpdateSubscription&&) = delete;

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::uint64_t id_ = 0;
};

} // namespace

bool run_session_recovery_preflight(
    const std::shared_ptr<IdempotencyFoundation>& foundation, proto::SessionOperation operation,
    RequestSession& session, const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks) {
    const auto operation_name = proto::session_operation_name(operation);
    if (!foundation) {
        session.error(
            "AUDIT_UNAVAILABLE", "account audit log is unavailable",
            {{"account", session.request().account}, {"path", ""}, {"reason", "open_failed"}},
            kDenied);
        return false;
    }
    const AccountAuditScanControl scan_control{
        session.deadline(), [&session] { return session.cancellation_requested(); }};
    auto epoch_result = foundation->acquire_epoch(scan_control);
    if (auto* failure = std::get_if<AccountAuditFailure>(&epoch_result)) {
        if (failure->interruption == AccountAuditFailure::Interruption::Deadline) {
            if (operation == proto::SessionOperation::List) {
                session.error("TIMEOUT", "request timed out",
                              {{"operation", operation_name}, {"state", nullptr}}, kTimeout);
            } else {
                session.error("TIMEOUT", "request timed out",
                              {{"operation", operation_name},
                               {"phase", "preflight"},
                               {"state", nullptr},
                               {"outcome", "not_started"},
                               {"idempotency", "not_requested"}},
                              kTimeout);
            }
        }
        return false;
    }
    auto epoch = std::get<AccountAuditCoordinator::Guard>(std::move(epoch_result));
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const auto sampled_now = seconds < 0 ? std::numeric_limits<std::uint64_t>::max()
                                         : static_cast<std::uint64_t>(seconds);
    const FileSpoolControl spool_control{session.deadline().expires_at,
                                         session.cancellation_token(),
                                         [&session] { return session.cancellation_requested(); }};
    const auto gate =
        foundation->run_absent_by_policy_gate(epoch, sampled_now, {}, spool_control, spool_hooks);
    if (gate.status == IdempotencyCoreGateStatus::Clean) {
        return true;
    }
    if (gate.status == IdempotencyCoreGateStatus::Interrupted) {
        if (gate.audit_failure.interruption == AccountAuditFailure::Interruption::Deadline) {
            if (operation == proto::SessionOperation::List) {
                session.error("TIMEOUT", "request timed out",
                              {{"operation", operation_name}, {"state", nullptr}}, kTimeout);
            } else {
                session.error("TIMEOUT", "request timed out",
                              {{"operation", operation_name},
                               {"phase", "preflight"},
                               {"state", nullptr},
                               {"outcome", "not_started"},
                               {"idempotency", "not_requested"}},
                              kTimeout);
            }
        }
        return false;
    }
    if (gate.terminal) {
        const auto& value = *gate.terminal;
        session.error(value.at("code").get<std::string>(), value.at("message").get<std::string>(),
                      value.at("details"), value.at("exit_code").get<int>());
        return false;
    }
    if (gate.status == IdempotencyCoreGateStatus::SpoolUnavailable && gate.spool_failure) {
        const auto reason = gate.spool_failure->durability_reason
                                ? spool_reason_name(*gate.spool_failure->durability_reason)
                                : std::string_view{"contradiction"};
        session.error("SPOOL_UNAVAILABLE", "attachment spool is unavailable",
                      {{"operation", operation_name}, {"path", "spool/"}, {"reason", reason}},
                      kGeneric);
        return false;
    }
    session.error("AUDIT_UNAVAILABLE", "account audit log is unavailable",
                  {{"account", session.request().account},
                   {"path", foundation->audit().path()},
                   {"reason", account_audit_durability_reason_name(gate.audit_failure.reason)}},
                  kDenied);
    return false;
}

std::optional<core::TdM6ChatFoldersUpdate>
m6_wait_for_folders(core::TdClient& client,
                    const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
                    const ResolverCaller& caller, RequestSession& session) {
    const auto ready = [&]() {
        if (deadline_expired(session.deadline())) {
            emit_resolver_error(ResolverError{ResolverTimeoutError{
                                    .operation = caller, .state = core::AuthState::Ready}},
                                session, caller);
            return false;
        }
        if (session.shutdown_requested()) {
            session.error("DAEMON_SHUTDOWN", "daemon is shutting down",
                          {{"reason", "daemon_shutdown"}}, kGeneric);
            return false;
        }
        if (session.cancellation_requested()) {
            return false;
        }
        const auto current = client.auth_state();
        if (!authorization || !current || current != authorization ||
            current->data.state != core::AuthState::Ready) {
            emit_resolver_error(
                ResolverError{ResolverNotAuthenticatedError{
                    .account = session.request().account,
                    .state = current ? current->data.state : core::AuthState::Unknown,
                    .reason = ResolverNotAuthedReason::AuthorizationLost}},
                session, caller);
            return false;
        }
        return true;
    };
    if (!ready()) {
        return std::nullopt;
    }
    if (auto cached = client.m6_chat_folders(authorization)) {
        return cached;
    }
    std::atomic<bool> notified{false};
    std::mutex mutex;
    std::condition_variable condition;
    const UpdateSubscription subscription(client, [&](const core::TdValue& update) {
        if (update.get_if<core::TdM6ChatFoldersUpdate>() != nullptr) {
            notified.store(true, std::memory_order_release);
            condition.notify_one();
        }
    });
    for (;;) {
        if (!ready()) {
            return std::nullopt;
        }
        if (auto cached = client.m6_chat_folders(authorization)) {
            return cached;
        }
        std::unique_lock lock(mutex);
        condition.wait_for(lock, 2ms,
                           [&] { return notified.exchange(false, std::memory_order_acq_rel); });
    }
}

namespace {

std::optional<ResolvedChatTarget> resolve_exact(ResolverConsumer& resolver, std::string selector,
                                                const ResolverCaller& caller,
                                                RequestSession& session) {
    const auto outcome = resolver.resolve_exact_chat(std::move(selector));
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, caller);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    return std::get<ResolvedChatTarget>(outcome);
}

bool valid_contact_request(proto::M6Operation operation, const json& arguments) {
    if (operation == proto::M6Operation::ContactList) {
        return exact_fields(arguments, {});
    }
    return operation == proto::M6Operation::ContactSearch && exact_fields(arguments, {"query"}) &&
           arguments["query"].is_string() &&
           valid_m6_contact_query(arguments["query"].get_ref<const std::string&>());
}

std::optional<std::vector<core::TdUserSummary>>
hydrate_contacts(core::TdClient& client, std::string_view account, const ResolverCaller& caller,
                 ResolverConsumer& resolver, const core::TdM6Users& users,
                 RequestSession& session) {
    std::vector<core::TdUserSummary> hydrated;
    hydrated.reserve(users.user_ids.size());
    std::size_t charged_bytes = 0;
    for (const auto user_id : users.user_ids) {
        auto user = resolver.read_target(
            [&](const auto& current) { return client.get_user(current, user_id); });
        if (stopped(user, client, account, caller, session)) {
            return std::nullopt;
        }
        if (const auto* error = user.value.get_if<core::TdError>()) {
            td_error(session, caller, *error);
            return std::nullopt;
        }
        const auto* value = user.value.get_if<core::TdUserSummary>();
        const auto identity = value != nullptr ? m6_user_identity(*value) : std::nullopt;
        if (!identity || identity->id != user_id) {
            internal(session, resolver_caller_name(caller));
            return std::nullopt;
        }
        const auto bytes = m6_user_identity_json(*identity).dump().size();
        if (bytes > 262'144) {
            capacity(session, resolver_caller_name(caller), "item_bytes", 262'144);
            return std::nullopt;
        }
        if (charged_bytes > 16'777'216 - bytes) {
            capacity(session, resolver_caller_name(caller), "bytes", 16'777'216);
            return std::nullopt;
        }
        charged_bytes += bytes;
        hydrated.push_back(*value);
    }
    return hydrated;
}

std::optional<core::TdM6ForumTopics>
read_topic_page(core::TdClient& client, std::string_view account, const ResolverCaller& caller,
                ResolverConsumer& resolver, std::int64_t chat_id, const M6TopicCursor& cursor,
                RequestSession& session) {
    auto response = resolver.read_target([&](const auto& current) {
        return client.m6_read(
            current, core::TdM6GetForumTopicsRequest{.chat_id = chat_id,
                                                     .query = {},
                                                     .offset_date = cursor.date,
                                                     .offset_message_id = cursor.message_id,
                                                     .offset_forum_topic_id = cursor.topic_id,
                                                     .limit = 100});
    });
    if (stopped(response, client, account, caller, session)) {
        return std::nullopt;
    }
    if (const auto* error = response.value.get_if<core::TdError>()) {
        td_error(session, caller, *error);
        return std::nullopt;
    }
    const auto* envelope = response.value.get_if<core::TdM6Response>();
    const auto* page = envelope != nullptr ? std::get_if<core::TdM6ForumTopics>(envelope) : nullptr;
    if (page == nullptr) {
        internal(session, resolver_caller_name(caller));
        return std::nullopt;
    }
    return *page;
}

std::string_view topic_capacity_resource(M6TopicCapacityResource resource) {
    switch (resource) {
    case M6TopicCapacityResource::Topics:
        return "topics";
    case M6TopicCapacityResource::Bytes:
        return "bytes";
    case M6TopicCapacityResource::ItemBytes:
        return "item_bytes";
    }
    return "topics";
}

bool finish_topic_append(const M6TopicScanResult& appended, const M6TopicAccumulator& accumulator,
                         const ResolverCaller& caller, RequestSession& session) {
    switch (appended.status) {
    case M6TopicScanStatus::Accepted:
        return false;
    case M6TopicScanStatus::Complete:
        session.result({{"items", accumulator.items()}, {"next", nullptr}});
        return true;
    case M6TopicScanStatus::StructuralError:
        internal(session, resolver_caller_name(caller));
        return true;
    case M6TopicScanStatus::NonAdvancing:
        session.error(
            "PAGINATION_INVALID", "topic pagination did not advance",
            {{"operation", resolver_caller_name(caller)}, {"reason", "non_advancing_upstream"}},
            kGeneric);
        return true;
    case M6TopicScanStatus::Capacity:
        capacity(session, resolver_caller_name(caller),
                 topic_capacity_resource(
                     appended.capacity_resource.value_or(M6TopicCapacityResource::Topics)),
                 appended.capacity_limit);
        return true;
    }
    return true;
}

} // namespace

void M6Coordinator::contact(proto::M6Operation operation, const proto::Request& request,
                            RequestSession& session) {
    const bool search = operation == proto::M6Operation::ContactSearch;
    if (!valid_contact_request(operation, request.args)) {
        usage(session, "contact command received malformed arguments", search ? "query" : nullptr);
        return;
    }
    const ResolverCaller caller{operation};
    ResolverConsumer resolver(client_.get(), account_, session);
    if (!bind(resolver, caller, session, true)) {
        return;
    }
    auto response = resolver.read_target([&](const auto& current) {
        return client_.get().m6_read(
            current, search ? core::TdM6Request{core::TdM6SearchContactsRequest{
                                  .query = request.args["query"].get<std::string>(), .limit = 100}}
                            : core::TdM6Request{core::TdM6GetContactsRequest{}});
    });
    if (stopped(response, client_.get(), account_, caller, session)) {
        return;
    }
    if (const auto* error = response.value.get_if<core::TdError>()) {
        td_error(session, caller, *error);
        return;
    }
    const auto* envelope = response.value.get_if<core::TdM6Response>();
    const auto* users = envelope != nullptr ? std::get_if<core::TdM6Users>(envelope) : nullptr;
    const auto maximum_users = search ? std::size_t{100} : std::size_t{131'072};
    if (users == nullptr || users->total_count < 0 ||
        static_cast<std::size_t>(users->total_count) < users->user_ids.size()) {
        internal(session, operation_name(operation));
        return;
    }
    if (users->user_ids.size() > maximum_users) {
        capacity(session, operation_name(operation), "users", maximum_users);
        return;
    }
    std::unordered_set<std::int64_t> user_ids;
    if (!std::ranges::all_of(users->user_ids, [&](std::int64_t id) {
            return id > 0 && user_ids.insert(id).second;
        })) {
        internal(session, operation_name(operation));
        return;
    }
    auto hydrated = hydrate_contacts(client_.get(), account_, caller, resolver, *users, session);
    if (!hydrated) {
        return;
    }
    auto result = m6_contact_list_json(*users, *hydrated, search);
    if (!result) {
        internal(session, operation_name(operation));
        return;
    }
    session.result(std::move(*result));
}

void M6Coordinator::folder_list(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::M6Operation::FolderList;
    if (!exact_fields(request.args, {})) {
        usage(session, "folder list received malformed arguments", nullptr);
        return;
    }
    const ResolverCaller caller{operation};
    ResolverConsumer resolver(client_.get(), account_, session);
    if (!bind(resolver, caller, session, true)) {
        return;
    }
    const auto folders =
        m6_wait_for_folders(client_.get(), resolver.bound_authorization(), caller, session);
    if (!folders) {
        return;
    }
    auto result = m6_folder_list_json(*folders);
    if (!result) {
        internal(session, operation_name(operation));
        return;
    }
    session.result(std::move(*result));
}

void M6Coordinator::folder_show(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::M6Operation::FolderShow;
    if (!exact_fields(request.args, {"folder_id"}) ||
        !request.args["folder_id"].is_number_integer() ||
        request.args["folder_id"].get<std::int64_t>() < 1 ||
        request.args["folder_id"].get<std::int64_t>() > std::numeric_limits<std::int32_t>::max()) {
        usage(session, "folder show requires a positive int32 folder id", "folder_id");
        return;
    }
    const auto folder_id = request.args["folder_id"].get<std::int32_t>();
    const ResolverCaller caller{operation};
    ResolverConsumer resolver(client_.get(), account_, session);
    if (!bind(resolver, caller, session, true)) {
        return;
    }
    const auto folders =
        m6_wait_for_folders(client_.get(), resolver.bound_authorization(), caller, session);
    if (!folders) {
        return;
    }
    const auto info = std::ranges::find(folders->folders, folder_id, &core::TdM6FolderInfo::id);
    if (info == folders->folders.end()) {
        session.error("NOT_FOUND", "folder was not found",
                      {{"operation", operation_name(operation)}, {"folder_id", folder_id}},
                      kNotFound);
        return;
    }
    auto response = resolver.read_target([&](const auto& current) {
        return client_.get().m6_read(current, core::TdM6GetChatFolderRequest{folder_id});
    });
    if (stopped(response, client_.get(), account_, caller, session)) {
        return;
    }
    if (const auto* error = response.value.get_if<core::TdError>()) {
        td_error(session, caller, *error);
        return;
    }
    const auto* envelope = response.value.get_if<core::TdM6Response>();
    const auto* maybe =
        envelope != nullptr ? std::get_if<core::TdM6MaybeChatFolder>(envelope) : nullptr;
    if (maybe == nullptr) {
        internal(session, operation_name(operation));
        return;
    }
    if (!maybe->folder) {
        session.error("NOT_FOUND", "folder was not found",
                      {{"operation", operation_name(operation)}, {"folder_id", folder_id}},
                      kNotFound);
        return;
    }
    auto projected = m6_folder_snapshot_json(folder_id, *maybe->folder, *info);
    if (!projected) {
        internal(session, operation_name(operation));
        return;
    }
    session.result({{"folder", std::move(*projected)}});
}

void M6Coordinator::topic_list(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::M6Operation::TopicList;
    if (!exact_fields(request.args, {"chat"}) || !request.args["chat"].is_string() ||
        !valid_m6_exact_selector(request.args["chat"].get_ref<const std::string&>())) {
        usage(session, "topic list requires an exact chat selector", "chat");
        return;
    }
    const ResolverCaller caller{operation};
    ResolverConsumer resolver(client_.get(), account_, session);
    if (!bind(resolver, caller, session, false)) {
        return;
    }
    const auto target =
        resolve_exact(resolver, request.args["chat"].get<std::string>(), caller, session);
    if (!target) {
        return;
    }
    const bool private_bot =
        target->observed_chat && target->observed_chat->kind == core::TdChatKind::Private &&
        target->observed_user && target->observed_user->id == target->observed_chat->related_id &&
        target->observed_user->is_bot && target->observed_user->bot_has_topics;
    const bool forum_supergroup =
        target->observed_chat && target->observed_chat->kind == core::TdChatKind::Supergroup &&
        target->observed_supergroup &&
        target->observed_supergroup->id == target->observed_chat->related_id &&
        !target->observed_supergroup->is_channel && target->observed_supergroup->is_forum;
    if (!private_bot && !forum_supergroup) {
        session.error("USAGE", "chat does not support topic listing",
                      {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
        return;
    }
    M6TopicAccumulator accumulator(target->chat.id);
    M6TopicCursor cursor;
    while (true) {
        auto page = read_topic_page(client_.get(), account_, caller, resolver, target->chat.id,
                                    cursor, session);
        if (!page) {
            return;
        }
        const auto appended = accumulator.append(cursor, *page);
        if (finish_topic_append(appended, accumulator, caller, session)) {
            return;
        }
        if (!appended.next) {
            internal(session, operation_name(operation));
            return;
        }
        cursor = *appended.next;
    }
}

void M6Coordinator::storage_stats(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::M6Operation::StorageStats;
    if (!exact_fields(request.args, {})) {
        usage(session, "storage stats received malformed arguments", nullptr);
        return;
    }
    const ResolverCaller caller{operation};
    ResolverConsumer resolver(client_.get(), account_, session);
    if (!bind(resolver, caller, session, false)) {
        return;
    }
    auto response = resolver.read_target([&](const auto& current) {
        return client_.get().m6_read(current,
                                     core::TdM6GetStorageStatisticsRequest{.chat_limit = 100});
    });
    if (stopped(response, client_.get(), account_, caller, session)) {
        return;
    }
    if (const auto* error = response.value.get_if<core::TdError>()) {
        td_error(session, caller, *error);
        return;
    }
    const auto* envelope = response.value.get_if<core::TdM6Response>();
    const auto* statistics =
        envelope != nullptr ? std::get_if<core::TdM6StorageStatistics>(envelope) : nullptr;
    auto projected = statistics != nullptr ? m6_storage_statistics_json(*statistics) : std::nullopt;
    if (!projected) {
        internal(session, operation_name(operation));
        return;
    }
    session.result(std::move(*projected));
}

void M6Coordinator::session_list(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::SessionOperation::List;
    if (!exact_fields(request.args, {})) {
        usage(session, "session list received malformed arguments", nullptr);
        return;
    }
    if (!run_session_recovery_preflight(foundation_, operation, session)) {
        return;
    }
    const ResolverCaller caller{operation};
    ResolverConsumer resolver(client_.get(), account_, session);
    if (!bind(resolver, caller, session, true)) {
        return;
    }
    auto response = resolver.read_target(
        [&](const auto& current) { return client_.get().get_active_sessions(current); });
    if (stopped(response, client_.get(), account_, caller, session)) {
        return;
    }
    if (const auto* error = response.value.get_if<core::TdError>()) {
        td_error(session, caller, *error);
        return;
    }
    const auto* sessions = response.value.get_if<core::TdSessions>();
    auto projected = sessions != nullptr ? m6_session_list_json(*sessions) : std::nullopt;
    if (!projected) {
        internal(session, resolver_caller_name(caller));
        return;
    }
    session.result(std::move(*projected));
}

void register_m6_commands(Dispatcher& dispatcher, M6Coordinator& reads, WriteCoordinator& writes) {
    for (const auto& identity : proto::m6_operation_identities()) {
        CommandDescriptor descriptor;
        switch (identity.tier) {
        case proto::M6Tier::Read:
            descriptor.tier = Tier::Read;
            break;
        case proto::M6Tier::Write:
            descriptor.tier = Tier::Write;
            break;
        case proto::M6Tier::Destructive:
            descriptor.tier = Tier::Destructive;
            break;
        }
        descriptor.m6_operation = identity.operation;
        descriptor.handler = [&reads, &writes, operation = identity.operation](
                                 const proto::Request& request, RequestSession& session) {
            switch (operation) {
            case proto::M6Operation::ContactList:
            case proto::M6Operation::ContactSearch:
                reads.contact(operation, request, session);
                return;
            case proto::M6Operation::FolderList:
                reads.folder_list(request, session);
                return;
            case proto::M6Operation::FolderShow:
                reads.folder_show(request, session);
                return;
            case proto::M6Operation::TopicList:
                reads.topic_list(request, session);
                return;
            case proto::M6Operation::StorageStats:
                reads.storage_stats(request, session);
                return;
            default:
                writes.m6_mutation(operation, request, session);
                return;
            }
        };
        dispatcher.register_command(std::string(identity.command_path), std::move(descriptor));
    }

    CommandDescriptor list;
    list.tier = Tier::Read;
    list.session_operation = proto::SessionOperation::List;
    list.handler = [&reads](const proto::Request& request, RequestSession& session) {
        reads.session_list(request, session);
    };
    dispatcher.register_command("session list", std::move(list));

    CommandDescriptor terminate;
    terminate.tier = Tier::Destructive;
    terminate.session_operation = proto::SessionOperation::Terminate;
    terminate.handler = [&writes](const proto::Request& request, RequestSession& session) {
        writes.terminate_session(request, session);
    };
    dispatcher.register_command("session terminate", std::move(terminate));
}

} // namespace tgcli::daemon
