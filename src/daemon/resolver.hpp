#pragma once

#include "core/td_client.hpp"
#include "daemon/chat_identity.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/message_summary.hpp"
#include "daemon/ready_read.hpp"
#include "proto/operation.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tgcli::daemon {

enum class ResolverScope { ActiveDialogs, LocalMaterialized };

enum class M2Operation {
    Chats,
    Read,
    MsgGet,
    MsgLink,
    Search,
    Unread,
    Fetch,
    Resolve,
    ChatInfo,
    ChatMembers,
    Listen,
    WaitFor,
};

std::string_view m2_operation_name(M2Operation operation);

using ResolverCaller = std::variant<M2Operation, proto::M3Operation>;

std::string_view resolver_caller_name(const ResolverCaller& caller);

enum class ResolverStop { Cancelled };
enum class ResolverUsageReason { InvalidArgument, UnsupportedChatType, UnsupportedLinkType };
enum class ResolverNotAuthedReason { NotReady, AuthorizationLost };
enum class ResolvedLinkType {
    PublicChat,
    BotStart,
    Message,
    ChatInvite,
    DirectMessagesChat,
    SavedMessages,
};

struct ResolverUsageError {
    std::string argument;
    ResolverUsageReason reason = ResolverUsageReason::InvalidArgument;
};

struct ResolverNotAuthenticatedError {
    std::string account;
    core::AuthState state = core::AuthState::Unknown;
    ResolverNotAuthedReason reason = ResolverNotAuthedReason::NotReady;
};

struct ResolverBotUnsupportedError {
    ResolverCaller operation;
};

struct ResolverNotFoundError {
    std::string selector;
    std::optional<ResolverScope> scope;
};

struct ResolverAmbiguousError {
    std::string selector;
    ResolverScope scope = ResolverScope::ActiveDialogs;
    std::vector<ChatIdentity> candidates;
    bool truncated = false;
    std::string argument = "selector";
};

struct ResolverRateLimitedError {
    ResolverCaller operation;
    std::int32_t retry_after = 0;
};

struct ResolverTdlibError {
    ResolverCaller operation;
    std::int32_t tdlib_code = 0;
};

struct ResolverTimeoutError {
    ResolverCaller operation;
    std::optional<core::AuthState> state;
};

struct ResolverInternalError {
    ResolverCaller operation;
};

struct UserIdentity {
    std::int64_t id = 0;
    std::string display_name;
    std::vector<std::string> usernames;
    bool is_bot = false;

    bool operator==(const UserIdentity&) const = default;
};

struct ResolverUserAmbiguousError {
    std::string selector;
    std::vector<UserIdentity> candidates;
    bool truncated = false;
};

struct ResolverPaginationInvalidError {
    ResolverCaller operation;
};

using ResolverError =
    std::variant<ResolverUsageError, ResolverNotAuthenticatedError, ResolverBotUnsupportedError,
                 ResolverNotFoundError, ResolverAmbiguousError, ResolverRateLimitedError,
                 ResolverTdlibError, ResolverTimeoutError, ResolverInternalError,
                 ResolverUserAmbiguousError, ResolverPaginationInvalidError>;

struct ResolverPrincipal {
    std::int64_t id = 0;
    bool is_bot = false;

    bool operator==(const ResolverPrincipal&) const = default;
};

struct ResolvedChatTarget {
    ResolverPrincipal principal;
    ChatIdentity chat;
    std::optional<core::TdChat> observed_chat;
    std::optional<std::int64_t> contextual_message_id;
    std::optional<TopicRef> contextual_topic;
    std::optional<ResolvedLinkType> link_type;
    std::optional<bool> is_public;
    std::optional<std::int64_t> private_user_id;
    std::optional<core::TdUserPresence> private_user_presence;
};

using ResolverPrincipalOutcome = std::variant<ResolverPrincipal, ResolverError, ResolverStop>;
using ResolverOutcome = std::variant<ResolvedChatTarget, ResolverError, ResolverStop>;
using UserResolverOutcome = std::variant<UserIdentity, ResolverError, ResolverStop>;

enum class ExactWriteSelectorStatus { Exact, Invalid, Title, UnsupportedLink };

ExactWriteSelectorStatus classify_exact_write_selector(std::string_view selector);

class ResolverConsumer {
  public:
    ResolverConsumer(core::TdClient& client, std::string_view account, RequestSession& session);
    ~ResolverConsumer();
    ResolverConsumer(const ResolverConsumer&) = delete;
    ResolverConsumer& operator=(const ResolverConsumer&) = delete;
    ResolverConsumer(ResolverConsumer&&) = delete;
    ResolverConsumer& operator=(ResolverConsumer&&) = delete;

    ResolverPrincipalOutcome bind_principal(ResolverCaller caller);
    ResolverOutcome resolve_chat(std::string selector, ResolverScope scope);
    ResolverOutcome resolve_exact_chat(std::string selector, std::string argument = "chat");
    ResolverOutcome resolve_saved_messages();
    ResolverOutcome resolve_saved_messages_for_write();
    UserResolverOutcome resolve_user(std::string selector,
                                     const std::optional<core::TdChat>& domain = std::nullopt);
    [[nodiscard]] std::optional<core::TdChat> cached_saved_messages_chat() const;
    ReadyReadResult read_target(const ReadyReadStart& start);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

void emit_resolver_error(const ResolverError& error, RequestSession& session,
                         M2Operation owning_operation = M2Operation::Resolve);
[[nodiscard]] nlohmann::json
resolver_error_terminal(const ResolverError& error,
                        M2Operation owning_operation = M2Operation::Resolve);

bool valid_resolve_selector(std::string_view selector);

class ResolveCoordinator {
  public:
    ResolveCoordinator(core::TdClient& client, std::string account)
        : client_(client), account_(std::move(account)) {}

    void resolve(const proto::Request& request, RequestSession& session);
    void resolve_for_scope(std::string selector, ResolverScope scope, RequestSession& session);

  private:
    std::reference_wrapper<core::TdClient> client_;
    std::string account_;
};

void register_resolve_command(Dispatcher& dispatcher, ResolveCoordinator& coordinator);

} // namespace tgcli::daemon
