#include "daemon/m6_commands.hpp"

#include "common/exit_codes.hpp"
#include "daemon/m6_domain.hpp"
#include "daemon/m6_model.hpp"
#include "daemon/m6_topic_scan.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <string_view>
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

std::optional<core::TdM6ChatFoldersUpdate>
wait_for_folders(core::TdClient& client,
                 const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
                 RequestSession& session) {
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
    while (!session.cancellation_requested() && !session.shutdown_requested()) {
        if (auto cached = client.m6_chat_folders(authorization)) {
            return cached;
        }
        const auto current = client.auth_state();
        if (!current || current->client_id != authorization->client_id ||
            current->client_generation != authorization->client_generation ||
            current->auth_sequence != authorization->auth_sequence ||
            current->data.state != core::AuthState::Ready || deadline_expired(session.deadline())) {
            return std::nullopt;
        }
        std::unique_lock lock(mutex);
        condition.wait_for(lock, 2ms,
                           [&] { return notified.exchange(false, std::memory_order_acq_rel); });
    }
    return std::nullopt;
}

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

} // namespace

void M6Coordinator::contact(proto::M6Operation operation, const proto::Request& request,
                            RequestSession& session) {
    const bool search = operation == proto::M6Operation::ContactSearch;
    if ((operation != proto::M6Operation::ContactList && !search) ||
        (search ? (!exact_fields(request.args, {"query"}) || !request.args["query"].is_string() ||
                   !valid_m6_contact_query(request.args["query"].get_ref<const std::string&>()))
                : !exact_fields(request.args, {}))) {
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
    if (users == nullptr || users->total_count < 0 ||
        static_cast<std::size_t>(users->total_count) < users->user_ids.size() ||
        users->user_ids.size() > (search ? 100 : 131'072)) {
        internal(session, operation_name(operation));
        return;
    }
    std::vector<core::TdUserSummary> hydrated;
    hydrated.reserve(users->user_ids.size());
    for (const auto user_id : users->user_ids) {
        auto user = resolver.read_target(
            [&](const auto& current) { return client_.get().get_user(current, user_id); });
        if (stopped(user, client_.get(), account_, caller, session)) {
            return;
        }
        if (const auto* error = user.value.get_if<core::TdError>()) {
            td_error(session, caller, *error);
            return;
        }
        const auto* value = user.value.get_if<core::TdUserSummary>();
        if (value == nullptr) {
            internal(session, operation_name(operation));
            return;
        }
        hydrated.push_back(*value);
    }
    auto result = m6_contact_list_json(*users, hydrated, search);
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
    const auto folders = wait_for_folders(client_.get(), resolver.bound_authorization(), session);
    if (!folders) {
        const auto current = client_.get().auth_state();
        if (!session.cancellation_requested() && !session.shutdown_requested()) {
            if (deadline_expired(session.deadline())) {
                emit_resolver_error(
                    ResolverError{ResolverTimeoutError{
                        .operation = caller,
                        .state = current ? std::optional{current->data.state} : std::nullopt}},
                    session, caller);
            } else {
                emit_resolver_error(
                    ResolverError{ResolverNotAuthenticatedError{
                        .account = account_,
                        .state = current ? current->data.state : core::AuthState::Unknown,
                        .reason = ResolverNotAuthedReason::AuthorizationLost}},
                    session, caller);
            }
        }
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
    const auto folders = wait_for_folders(client_.get(), resolver.bound_authorization(), session);
    if (!folders) {
        internal(session, operation_name(operation));
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
    M6TopicAccumulator accumulator(target->chat.id);
    M6TopicCursor cursor;
    while (true) {
        auto response = resolver.read_target([&](const auto& current) {
            return client_.get().m6_read(
                current, core::TdM6GetForumTopicsRequest{.chat_id = target->chat.id,
                                                         .query = {},
                                                         .offset_date = cursor.date,
                                                         .offset_message_id = cursor.message_id,
                                                         .offset_forum_topic_id = cursor.topic_id,
                                                         .limit = 100});
        });
        if (stopped(response, client_.get(), account_, caller, session)) {
            return;
        }
        if (const auto* error = response.value.get_if<core::TdError>()) {
            td_error(session, caller, *error);
            return;
        }
        const auto* envelope = response.value.get_if<core::TdM6Response>();
        const auto* page =
            envelope != nullptr ? std::get_if<core::TdM6ForumTopics>(envelope) : nullptr;
        if (page == nullptr) {
            internal(session, operation_name(operation));
            return;
        }
        const auto appended = accumulator.append(cursor, *page);
        if (appended.status == M6TopicScanStatus::StructuralError) {
            internal(session, operation_name(operation));
            return;
        }
        if (appended.status == M6TopicScanStatus::NonAdvancing) {
            session.error(
                "PAGINATION_INVALID", "topic pagination did not advance",
                {{"operation", operation_name(operation)}, {"reason", "non_advancing_upstream"}},
                kGeneric);
            return;
        }
        if (appended.status == M6TopicScanStatus::Capacity) {
            capacity(session, operation_name(operation), "topics", 4'096);
            return;
        }
        if (appended.status == M6TopicScanStatus::Complete) {
            session.result({{"items", accumulator.items()}, {"next", nullptr}});
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

} // namespace tgcli::daemon
