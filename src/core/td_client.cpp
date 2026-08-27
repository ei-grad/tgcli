#include "core/td_client.hpp"

#include "core/query_registry.hpp"
#include "core/request_lifecycle.hpp"
#include "core/td_authorization.hpp"
#include "core/update_bus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tgcli::core {

namespace {

constexpr auto kCloseTimeout = std::chrono::seconds(30);
constexpr auto kReceiveTimeout = std::chrono::milliseconds(100);

std::future<TdValue> failed_future(const std::string& message) {
    std::promise<TdValue> promise;
    auto future = promise.get_future();
    promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
    return future;
}

std::future<TdValue> failed_future(TdAuthorizationFailure failure) {
    std::promise<TdValue> promise;
    auto future = promise.get_future();
    promise.set_exception(std::make_exception_ptr(TdAuthorizationError(failure)));
    return future;
}

TdClosedDecisionStatus terminal_decision(TdLifecycleClaimStatus claim) {
    switch (claim) {
    case TdLifecycleClaimStatus::Active:
        return TdClosedDecisionStatus::Pending;
    case TdLifecycleClaimStatus::Disconnected:
        return TdClosedDecisionStatus::Disconnected;
    case TdLifecycleClaimStatus::Shutdown:
        return TdClosedDecisionStatus::Shutdown;
    case TdLifecycleClaimStatus::TimedOut:
        return TdClosedDecisionStatus::TimedOut;
    case TdLifecycleClaimStatus::Rejected:
        return TdClosedDecisionStatus::Rejected;
    }
    return TdClosedDecisionStatus::Rejected;
}

std::optional<DescriptorKind> direct_mutation_tier(TdFunctionKind function) {
    switch (function) {
    case TdFunctionKind::SendMessage:
    case TdFunctionKind::ForwardMessages:
    case TdFunctionKind::EditMessageText:
    case TdFunctionKind::AddMessageReaction:
    case TdFunctionKind::RemoveMessageReaction:
    case TdFunctionKind::PinChatMessage:
    case TdFunctionKind::UnpinChatMessage:
    case TdFunctionKind::ViewMessages:
    case TdFunctionKind::SetChatNotificationSettings:
    case TdFunctionKind::ToggleChatIsPinned:
    case TdFunctionKind::AddChatToList:
    case TdFunctionKind::JoinChat:
    case TdFunctionKind::JoinChatByInviteLink:
    case TdFunctionKind::AddContact:
    case TdFunctionKind::RemoveContacts:
    case TdFunctionKind::SetMessageSenderBlockList:
    case TdFunctionKind::CreateChatFolder:
    case TdFunctionKind::EditChatFolder:
    case TdFunctionKind::CreateForumTopic:
    case TdFunctionKind::EditForumTopic:
    case TdFunctionKind::ToggleForumTopicIsClosed:
    case TdFunctionKind::SetChatTitle:
    case TdFunctionKind::SetChatPhoto:
    case TdFunctionKind::SetChatDescription:
    case TdFunctionKind::SetChatMemberStatus:
    case TdFunctionKind::SetChatPermissions:
        return DescriptorKind::Write;
    case TdFunctionKind::DeleteMessages:
    case TdFunctionKind::LeaveChat:
    case TdFunctionKind::DeleteChatFolder:
    case TdFunctionKind::CreateChatInviteLink:
    case TdFunctionKind::RevokeChatInviteLink:
    case TdFunctionKind::OptimizeStorage:
    case TdFunctionKind::TerminateSession:
        return DescriptorKind::Destructive;
    default:
        return std::nullopt;
    }
}

TdFunctionKind direct_request_kind(const TdDirectRequest& request) {
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, TdEditMessageTextRequest>) {
                return TdFunctionKind::EditMessageText;
            } else if constexpr (std::is_same_v<Request, TdDeleteMessagesRequest>) {
                return TdFunctionKind::DeleteMessages;
            } else if constexpr (std::is_same_v<Request, TdMessageReactionRequest>) {
                return value.remove ? TdFunctionKind::RemoveMessageReaction
                                    : TdFunctionKind::AddMessageReaction;
            } else if constexpr (std::is_same_v<Request, TdPinMessageRequest>) {
                return value.pinned ? TdFunctionKind::PinChatMessage
                                    : TdFunctionKind::UnpinChatMessage;
            } else if constexpr (std::is_same_v<Request, TdViewMessagesRequest>) {
                return TdFunctionKind::ViewMessages;
            } else if constexpr (std::is_same_v<Request, TdSetChatNotificationSettingsRequest>) {
                return TdFunctionKind::SetChatNotificationSettings;
            } else if constexpr (std::is_same_v<Request, TdToggleChatIsPinnedRequest>) {
                return TdFunctionKind::ToggleChatIsPinned;
            } else if constexpr (std::is_same_v<Request, TdAddChatToListRequest>) {
                return TdFunctionKind::AddChatToList;
            } else if constexpr (std::is_same_v<Request, TdJoinChatRequest>) {
                return value.is_invite_request() ? TdFunctionKind::JoinChatByInviteLink
                                                 : TdFunctionKind::JoinChat;
            } else if constexpr (std::is_same_v<Request, TdTerminateSessionRequest>) {
                return TdFunctionKind::TerminateSession;
            } else if constexpr (std::is_same_v<Request, TdM6Request>) {
                return td_m6_request_kind(value);
            } else {
                static_assert(std::is_same_v<Request, TdLeaveChatRequest>);
                return TdFunctionKind::LeaveChat;
            }
        },
        request);
}
} // namespace

struct TdSendLease::State {
    std::function<std::future<TdValue>(TdlibParameters)> submit;
};

struct TdPreparedWrite::State {
    std::function<std::future<TdValue>()> submit;
    std::optional<TdAuthorizationFailure> authorization_failure;
};

struct TdOwnerLease::State {
    TdRequestOwner owner;
    std::function<void()> revoke;
};

struct TdClosedDecision::State {
    std::mutex mutex;
    TdClosedDecisionStatus status = TdClosedDecisionStatus::Pending;
    std::uint64_t query_id = 0;
    bool expected_transition = false;
    bool released = false;
    std::size_t callbacks_in_flight = 0;
    std::condition_variable callbacks_done;
    std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim;
    std::function<void()> release;
};

class TdClient::Impl {
  public:
    Impl(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging,
         TdClientEventHooks event_hooks, TdGenerationObserverFactory generation_observer_factory)
        : runtime_(std::move(runtime)),
          event_now_(event_hooks.now ? std::move(event_hooks.now)
                                     : [] { return TdEventClock::now(); }),
          after_event_observed_(std::move(event_hooks.after_observed)),
          before_lifecycle_callback_drain_wait_(
              std::move(event_hooks.before_lifecycle_callback_drain_wait)),
          before_closed_decisions_drain_wait_(
              std::move(event_hooks.before_closed_decisions_drain_wait)),
          generation_observer_factory_(std::move(generation_observer_factory)) {
        if (runtime_ == nullptr) {
            throw std::invalid_argument("TdClient runtime must not be null");
        }
        runtime_->initialize_process(logging);
        activate_initial_generation();
        receive_thread_ = std::thread([this] { receive_loop(); });
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        close();
    }

    std::future<TdValue> send(TdSendDescriptor descriptor, TdValue request,
                              TdQueryLifetime lifetime = {}) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr) {
            return failed_future("tdlib client closed");
        }

        return generation->lifecycle.send([this, generation, descriptor = std::move(descriptor),
                                           request = std::move(request),
                                           lifetime = std::move(lifetime)]() mutable {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            if (!generation->initial_state_installed) {
                return failed_future(TdAuthorizationFailure::AuthStateMismatch);
            }
            return submit_locked(generation, descriptor, request, lifetime).future;
        });
    }

    std::future<TdValue> send(TdSendDescriptor descriptor, TdlibParameters parameters) {
        return send(std::move(descriptor),
                    runtime_->make_set_tdlib_parameters(std::move(parameters)));
    }

    std::future<TdValue> send_read(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdFunctionKind function, TdValue request,
                                   TdQueryLifetime lifetime = {}) {
        if (!authorization ||
            (function != TdFunctionKind::GetOption && function != TdFunctionKind::GetMe &&
             function != TdFunctionKind::GetContacts &&
             function != TdFunctionKind::SearchContacts &&
             function != TdFunctionKind::GetSavedMessagesTags &&
             function != TdFunctionKind::SearchSavedMessages &&
             function != TdFunctionKind::GetActiveSessions && function != TdFunctionKind::GetChat &&
             function != TdFunctionKind::GetChatFolder &&
             function != TdFunctionKind::GetForumTopics &&
             function != TdFunctionKind::GetForumTopic &&
             function != TdFunctionKind::GetChatMember &&
             function != TdFunctionKind::GetStorageStatistics &&
             function != TdFunctionKind::GetChatHistory &&
             function != TdFunctionKind::GetChatMessageByDate &&
             function != TdFunctionKind::GetMessageThread &&
             function != TdFunctionKind::GetForumTopicHistory &&
             function != TdFunctionKind::GetMessageThreadHistory &&
             function != TdFunctionKind::GetDirectMessagesChatTopicHistory &&
             function != TdFunctionKind::GetSavedMessagesTopicHistory &&
             function != TdFunctionKind::GetMessages &&
             function != TdFunctionKind::GetMessageLink && function != TdFunctionKind::GetChats &&
             function != TdFunctionKind::LoadChats &&
             function != TdFunctionKind::SearchPublicChat &&
             function != TdFunctionKind::GetInternalLinkType &&
             function != TdFunctionKind::GetMessageLinkInfo &&
             function != TdFunctionKind::CheckChatInviteLink &&
             function != TdFunctionKind::GetUser &&
             function != TdFunctionKind::GetBasicGroupFullInfo &&
             function != TdFunctionKind::GetSupergroup &&
             function != TdFunctionKind::GetSupergroupFullInfo &&
             function != TdFunctionKind::GetSupergroupMembers &&
             function != TdFunctionKind::CreatePrivateChat &&
             function != TdFunctionKind::GetMessage &&
             function != TdFunctionKind::GetMessageProperties &&
             function != TdFunctionKind::GetMessageAvailableReactions &&
             function != TdFunctionKind::ParseTextEntities)) {
            return failed_future(TdAuthorizationFailure::FunctionDenied);
        }
        auto owner = issue_owner(TdOwnerKind::Request);
        if (!owner) {
            return failed_future(TdAuthorizationFailure::GenerationClosed);
        }
        return send(TdSendDescriptor{.function = function,
                                     .tier = DescriptorKind::Read,
                                     .owner = owner.owner(),
                                     .client_generation = authorization->client_generation,
                                     .auth_sequence = authorization->auth_sequence,
                                     .auth_state = authorization->data.state},
                    std::move(request), std::move(lifetime));
    }

    std::future<TdValue> get_me(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        if (!authorization) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        return send_read(authorization, TdFunctionKind::GetMe,
                         runtime_->make_auth_function(TdAuthRequest{TdFunctionKind::GetMe}));
    }

    std::future<TdValue>
    get_contacts(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        return send_read(authorization, TdFunctionKind::GetContacts, runtime_->make_get_contacts());
    }

    std::future<TdValue> m6_read(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                 TdM6Request request) {
        if (!valid_td_m6_request(request) || !td_m6_request_is_read(request)) {
            return failed_future(TdAuthorizationFailure::FunctionDenied);
        }
        const auto function = td_m6_request_kind(request);
        return send_read(authorization, function, runtime_->make_m6_function(std::move(request)));
    }

    [[nodiscard]] std::optional<TdM6ChatFoldersUpdate>
    m6_chat_folders(const std::shared_ptr<const AuthStateSnapshot>& authorization) const {
        if (!authorization || authorization->data.state != AuthState::Ready) {
            return std::nullopt;
        }
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard lock(state_mutex_);
            generation = current_;
        }
        if (!generation || generation->client_id != authorization->client_id ||
            generation->number != authorization->client_generation) {
            return std::nullopt;
        }
        const std::lock_guard lock(generation->m6_cache_mutex);
        return generation->m6_chat_folders;
    }

    std::future<TdValue>
    get_saved_messages_tags(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                            std::int64_t saved_messages_topic_id) {
        return send_read(authorization, TdFunctionKind::GetSavedMessagesTags,
                         runtime_->make_get_saved_messages_tags(saved_messages_topic_id));
    }

    std::future<TdValue>
    search_saved_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          TdSearchSavedMessagesRequest request) {
        return send_read(authorization, TdFunctionKind::SearchSavedMessages,
                         runtime_->make_search_saved_messages(std::move(request)));
    }

    std::future<TdValue>
    get_active_sessions(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        return send_read(authorization, TdFunctionKind::GetActiveSessions,
                         runtime_->make_get_active_sessions());
    }

    std::future<TdValue> get_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t chat_id) {
        return send_read(authorization, TdFunctionKind::GetChat, runtime_->make_get_chat(chat_id));
    }

    std::future<TdValue>
    get_chat_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     std::int64_t chat_id, std::int64_t from_message_id, std::int32_t offset,
                     std::int32_t limit, bool only_local) {
        return send_read(
            authorization, TdFunctionKind::GetChatHistory,
            runtime_->make_get_chat_history(chat_id, from_message_id, offset, limit, only_local));
    }

    std::future<TdValue>
    get_chat_message_by_date(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::int64_t chat_id, std::int32_t date) {
        return send_read(authorization, TdFunctionKind::GetChatMessageByDate,
                         runtime_->make_get_chat_message_by_date(chat_id, date));
    }

    std::future<TdValue>
    get_message_thread(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       std::int64_t chat_id, std::int64_t message_id) {
        return send_read(authorization, TdFunctionKind::GetMessageThread,
                         runtime_->make_get_message_thread(chat_id, message_id));
    }

    std::future<TdValue>
    get_forum_topic_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                            std::int64_t chat_id, std::int32_t forum_topic_id,
                            std::int64_t from_message_id, std::int32_t offset, std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::GetForumTopicHistory,
                         runtime_->make_get_forum_topic_history(chat_id, forum_topic_id,
                                                                from_message_id, offset, limit));
    }

    std::future<TdValue>
    get_message_thread_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                               std::int64_t chat_id, std::int64_t message_id,
                               std::int64_t from_message_id, std::int32_t offset,
                               std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::GetMessageThreadHistory,
                         runtime_->make_get_message_thread_history(chat_id, message_id,
                                                                   from_message_id, offset, limit));
    }

    std::future<TdValue> get_direct_messages_chat_topic_history(
        const std::shared_ptr<const AuthStateSnapshot>& authorization, std::int64_t chat_id,
        std::int64_t topic_id, std::int64_t from_message_id, std::int32_t offset,
        std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::GetDirectMessagesChatTopicHistory,
                         runtime_->make_get_direct_messages_chat_topic_history(
                             chat_id, topic_id, from_message_id, offset, limit));
    }

    std::future<TdValue>
    get_saved_messages_topic_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                     std::int64_t topic_id, std::int64_t from_message_id,
                                     std::int32_t offset, std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::GetSavedMessagesTopicHistory,
                         runtime_->make_get_saved_messages_topic_history(topic_id, from_message_id,
                                                                         offset, limit));
    }

    std::future<TdValue> get_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                      std::int64_t chat_id, std::vector<std::int64_t> message_ids) {
        return send_read(authorization, TdFunctionKind::GetMessages,
                         runtime_->make_get_messages(chat_id, std::move(message_ids)));
    }

    std::future<TdValue>
    get_message_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     std::int64_t chat_id, std::int64_t message_id, std::int32_t media_timestamp,
                     std::int32_t checklist_task_id, std::string poll_option_id, bool for_album,
                     bool in_message_thread) {
        return send_read(authorization, TdFunctionKind::GetMessageLink,
                         runtime_->make_get_message_link(
                             chat_id, message_id, media_timestamp, checklist_task_id,
                             std::move(poll_option_id), for_album, in_message_thread));
    }

    std::future<TdValue> get_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdChatList list, std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::GetChats,
                         runtime_->make_get_chats(list, limit));
    }

    std::future<TdValue> load_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    TdChatList list, std::int32_t limit) {
        return send_read(authorization, TdFunctionKind::LoadChats,
                         runtime_->make_load_chats(list, limit));
    }

    std::future<TdValue>
    search_public_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       std::string username) {
        return send_read(authorization, TdFunctionKind::SearchPublicChat,
                         runtime_->make_search_public_chat(std::move(username)));
    }

    std::future<TdValue>
    get_internal_link_type(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::string_view link, TdQueryLifetime lifetime,
                           const secure::WipeObserver& wipe_observer) {
        const bool sensitive = lifetime != nullptr;
        auto request = runtime_->make_get_internal_link_type(link, sensitive, wipe_observer);
        return send_read(authorization, TdFunctionKind::GetInternalLinkType, std::move(request),
                         std::move(lifetime));
    }

    std::future<TdValue>
    get_message_link_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          std::string url) {
        return send_read(authorization, TdFunctionKind::GetMessageLinkInfo,
                         runtime_->make_get_message_link_info(std::move(url)));
    }

    std::future<TdValue>
    check_chat_invite_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::string_view link, TdQueryLifetime lifetime,
                           const secure::WipeObserver& wipe_observer) {
        return send_read(authorization, TdFunctionKind::CheckChatInviteLink,
                         runtime_->make_check_chat_invite_link(link, wipe_observer),
                         std::move(lifetime));
    }

    std::future<TdValue> get_user(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t user_id) {
        return send_read(authorization, TdFunctionKind::GetUser, runtime_->make_get_user(user_id));
    }

    std::future<TdValue>
    get_basic_group_full_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                              std::int64_t basic_group_id) {
        return send_read(authorization, TdFunctionKind::GetBasicGroupFullInfo,
                         runtime_->make_get_basic_group_full_info(basic_group_id));
    }

    std::future<TdValue>
    get_supergroup(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                   std::int64_t supergroup_id) {
        return send_read(authorization, TdFunctionKind::GetSupergroup,
                         runtime_->make_get_supergroup(supergroup_id));
    }

    std::future<TdValue>
    get_supergroup_full_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::int64_t supergroup_id) {
        return send_read(authorization, TdFunctionKind::GetSupergroupFullInfo,
                         runtime_->make_get_supergroup_full_info(supergroup_id));
    }

    std::future<TdValue>
    get_supergroup_members(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::int64_t supergroup_id, std::string query, std::int32_t offset,
                           std::int32_t limit) {
        return send_read(
            authorization, TdFunctionKind::GetSupergroupMembers,
            runtime_->make_get_supergroup_members(supergroup_id, std::move(query), offset, limit));
    }

    std::future<TdValue>
    create_private_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                        std::int64_t user_id, bool force) {
        return send_read(authorization, TdFunctionKind::CreatePrivateChat,
                         runtime_->make_create_private_chat(user_id, force));
    }

    std::future<TdValue> get_message(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                     std::int64_t chat_id, std::int64_t message_id) {
        return send_read(authorization, TdFunctionKind::GetMessage,
                         runtime_->make_get_message(chat_id, message_id));
    }

    std::future<TdValue>
    get_message_properties(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::int64_t chat_id, std::int64_t message_id) {
        return send_read(authorization, TdFunctionKind::GetMessageProperties,
                         runtime_->make_get_message_properties(chat_id, message_id));
    }

    std::future<TdValue>
    get_message_available_reactions(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    std::int64_t chat_id, std::int64_t message_id) {
        return send_read(authorization, TdFunctionKind::GetMessageAvailableReactions,
                         runtime_->make_get_message_available_reactions(chat_id, message_id));
    }

    std::future<TdValue>
    get_unix_time(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        return send_read(authorization, TdFunctionKind::GetOption, runtime_->make_get_unix_time());
    }

    std::future<TdValue>
    parse_text_entities(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                        std::string text, TdTextParseMode mode) {
        return send_read(authorization, TdFunctionKind::ParseTextEntities,
                         runtime_->make_parse_text_entities(std::move(text), mode));
    }

    std::future<TdValue> send_message(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                      TdSendMessageRequest request) {
        if (!authorization) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        if (!valid_td_send_message_request(request)) {
            return failed_future("sendMessage request is invalid");
        }
        auto owner = issue_owner(TdOwnerKind::Request);
        if (!owner) {
            return failed_future(TdAuthorizationFailure::GenerationClosed);
        }
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (!generation) {
            return failed_future(TdAuthorizationFailure::GenerationClosed);
        }
        const TdSendDescriptor descriptor{.function = TdFunctionKind::SendMessage,
                                          .tier = DescriptorKind::Write,
                                          .owner = owner.owner(),
                                          .client_generation = authorization->client_generation,
                                          .auth_sequence = authorization->auth_sequence,
                                          .auth_state = authorization->data.state};
        return generation->lifecycle.send([this, generation, descriptor,
                                           request = std::move(request)]() mutable {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            if (!generation->initial_state_installed) {
                return failed_future(TdAuthorizationFailure::AuthStateMismatch);
            }
            if (const auto failure = authorization_failure_locked(
                    generation, descriptor, TdFunctionData{TdFunctionKind::SendMessage})) {
                return failed_future(*failure);
            }
            if (request.content.parsed &&
                !request.content.formatted_text.capability.valid_for(generation->number)) {
                return failed_future("parsed formattedText capability expired");
            }
            try {
                auto function = runtime_->make_send_message(std::move(request), generation->number);
                return submit_admitted_locked(generation, descriptor, function).future;
            } catch (const std::exception& error) {
                return failed_future(error.what());
            }
        });
    }

    TdPreparedWrite prepare_write(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  TdFunctionKind function, DescriptorKind tier,
                                  TdValue request_value, TdQueryLifetime lifetime = {}) {
        const auto& function_data = request_value.function_data();
        if (!authorization || !function_data || function_data->kind() != function) {
            return {};
        }
        auto owner = std::make_shared<TdOwnerLease>(issue_owner(TdOwnerKind::Request));
        if (!*owner) {
            return {};
        }
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (!generation) {
            return {};
        }
        const TdSendDescriptor descriptor{.function = function,
                                          .tier = tier,
                                          .owner = owner->owner(),
                                          .client_generation = authorization->client_generation,
                                          .auth_sequence = authorization->auth_sequence,
                                          .auth_state = authorization->data.state};
        TdPreparedWrite prepared;
        std::optional<TdAuthorizationFailure> rejection;
        const bool admitted = generation->lifecycle.admit([&] {
            auto held = std::make_shared<LeaseLocks>();
            held->auth_commit = std::unique_lock<std::mutex>(generation->auth_commit_mutex);
            held->outbound = std::unique_lock<std::mutex>(generation->outbound_mutex);
            if (!generation->initial_state_installed) {
                rejection = TdAuthorizationFailure::AuthStateMismatch;
                return;
            }
            if (const auto failure = authorization_failure_locked(
                    generation, descriptor, TdFunctionData{descriptor.function})) {
                rejection = failure;
                return;
            }
            auto resources = std::make_shared<PreparedWriteResources>();
            resources->owner = std::move(owner);
            resources->held = std::move(held);
            resources->request.emplace(std::move(request_value));
            resources->lifetime = std::move(lifetime);
            auto state = std::make_shared<TdPreparedWrite::State>();
            state->submit = [this, generation, descriptor,
                             resources = std::move(resources)]() mutable {
                if (!resources->request.has_value()) {
                    return failed_future(TdAuthorizationFailure::AuthStateMismatch);
                }
                auto value = std::move(resources->request.value());
                resources->request.reset();
                auto submission =
                    submit_admitted_locked(generation, descriptor, value, resources->lifetime);
                resources->release_locks_and_owner();
                value = {};
                resources->lifetime.reset();
                return std::move(submission.future);
            };
            prepared = TdPreparedWrite(std::move(state));
        });
        if (prepared) {
            return prepared;
        }
        auto state = std::make_shared<TdPreparedWrite::State>();
        state->authorization_failure =
            admitted && rejection ? rejection
                                  : std::optional{TdAuthorizationFailure::GenerationClosed};
        return TdPreparedWrite(std::move(state));
    }

    TdPreparedWrite
    prepare_send_message(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                         TdSendMessageRequest request) {
        if (!authorization || !valid_td_send_message_request(request) ||
            (request.content.parsed && !request.content.formatted_text.capability.valid_for(
                                           authorization->client_generation))) {
            return {};
        }
        try {
            return prepare_write(
                authorization, TdFunctionKind::SendMessage, DescriptorKind::Write,
                runtime_->make_send_message(std::move(request), authorization->client_generation));
        } catch (const std::exception&) {
            return {};
        }
    }

    TdPreparedWrite
    prepare_forward_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             TdForwardMessagesRequest request) {
        if (!authorization || !valid_td_forward_messages_request(request)) {
            return {};
        }
        try {
            return prepare_write(authorization, TdFunctionKind::ForwardMessages,
                                 DescriptorKind::Write,
                                 runtime_->make_forward_messages(std::move(request)));
        } catch (const std::exception&) {
            return {};
        }
    }

    TdPreparedWrite
    prepare_direct_mutation(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                            TdDirectRequest request, TdQueryLifetime lifetime) {
        if (!authorization || !valid_td_direct_request(request)) {
            return {};
        }
        const auto function = direct_request_kind(request);
        const auto tier = direct_mutation_tier(function);
        if (!tier) {
            return {};
        }
        try {
            auto value = std::visit(
                [this](auto&& input) -> TdValue {
                    using Request = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<Request, TdEditMessageTextRequest>) {
                        return runtime_->make_edit_message_text(
                            std::forward<decltype(input)>(input));
                    } else if constexpr (std::is_same_v<Request, TdDeleteMessagesRequest>) {
                        return runtime_->make_delete_messages(std::forward<decltype(input)>(input));
                    } else if constexpr (std::is_same_v<Request, TdMessageReactionRequest>) {
                        return runtime_->make_message_reaction(
                            std::forward<decltype(input)>(input));
                    } else if constexpr (std::is_same_v<Request, TdPinMessageRequest>) {
                        return runtime_->make_pin_message(input);
                    } else if constexpr (std::is_same_v<Request, TdViewMessagesRequest>) {
                        return runtime_->make_view_messages(std::forward<decltype(input)>(input));
                    } else if constexpr (std::is_same_v<Request,
                                                        TdSetChatNotificationSettingsRequest>) {
                        return runtime_->make_set_chat_notification_settings(
                            std::forward<decltype(input)>(input));
                    } else if constexpr (std::is_same_v<Request, TdToggleChatIsPinnedRequest>) {
                        return runtime_->make_toggle_chat_is_pinned(input);
                    } else if constexpr (std::is_same_v<Request, TdAddChatToListRequest>) {
                        return runtime_->make_add_chat_to_list(input);
                    } else if constexpr (std::is_same_v<Request, TdJoinChatRequest>) {
                        return runtime_->make_join_chat(std::forward<decltype(input)>(input));
                    } else if constexpr (std::is_same_v<Request, TdTerminateSessionRequest>) {
                        return runtime_->make_terminate_session(input.session_id);
                    } else if constexpr (std::is_same_v<Request, TdM6Request>) {
                        return runtime_->make_m6_function(std::forward<decltype(input)>(input));
                    } else {
                        static_assert(std::is_same_v<Request, TdLeaveChatRequest>);
                        return runtime_->make_leave_chat(input);
                    }
                },
                std::move(request));
            return prepare_write(authorization, function, *tier, std::move(value),
                                 std::move(lifetime));
        } catch (const std::exception&) {
            return {};
        }
    }

    static std::future<TdValue> send(TdPreparedWrite prepared) {
        if (!prepared.state_) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        if (!prepared.state_->submit) {
            return failed_future(prepared.state_->authorization_failure.value_or(
                TdAuthorizationFailure::AuthStateMismatch));
        }
        auto state = std::move(prepared.state_);
        auto submit = std::move(state->submit);
        return submit();
    }

    std::future<TdValue>
    send_direct_mutation(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                         TdValue request) {
        if (!authorization) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        const auto& function_data = request.function_data();
        if (!function_data) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        const auto function = function_data->kind();
        if (!function) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        const auto tier = direct_mutation_tier(function.value_or(TdFunctionKind::Close));
        if (!tier) {
            return failed_future(TdAuthorizationFailure::FunctionDenied);
        }
        auto owner = issue_owner(TdOwnerKind::Request);
        if (!owner) {
            return failed_future(TdAuthorizationFailure::GenerationClosed);
        }
        return send(TdSendDescriptor{.function = function.value_or(TdFunctionKind::Close),
                                     .tier = tier.value_or(DescriptorKind::Read),
                                     .owner = owner.owner(),
                                     .client_generation = authorization->client_generation,
                                     .auth_sequence = authorization->auth_sequence,
                                     .auth_state = authorization->data.state},
                    std::move(request));
    }

    std::future<TdValue>
    edit_message_text(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                      TdEditMessageTextRequest request) {
        return send_direct_mutation(authorization,
                                    runtime_->make_edit_message_text(std::move(request)));
    }

    std::future<TdValue>
    delete_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                    TdDeleteMessagesRequest request) {
        return send_direct_mutation(authorization,
                                    runtime_->make_delete_messages(std::move(request)));
    }

    std::future<TdValue>
    set_message_reaction(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                         TdMessageReactionRequest request) {
        return send_direct_mutation(authorization,
                                    runtime_->make_message_reaction(std::move(request)));
    }

    std::future<TdValue>
    set_message_pinned(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       TdPinMessageRequest request) {
        return send_direct_mutation(authorization, runtime_->make_pin_message(request));
    }

    std::future<TdValue>
    view_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                  TdViewMessagesRequest request) {
        return send_direct_mutation(authorization,
                                    runtime_->make_view_messages(std::move(request)));
    }

    std::future<TdValue>
    set_chat_notification_settings(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdSetChatNotificationSettingsRequest request) {
        return send_direct_mutation(authorization,
                                    runtime_->make_set_chat_notification_settings(request));
    }

    std::future<TdValue>
    toggle_chat_is_pinned(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          TdToggleChatIsPinnedRequest request) {
        return send_direct_mutation(authorization, runtime_->make_toggle_chat_is_pinned(request));
    }

    std::future<TdValue>
    add_chat_to_list(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     TdAddChatToListRequest request) {
        return send_direct_mutation(authorization, runtime_->make_add_chat_to_list(request));
    }

    std::future<TdValue> join_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdJoinChatRequest request) {
        return send_direct_mutation(authorization, runtime_->make_join_chat(std::move(request)));
    }

    std::future<TdValue> leave_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    TdLeaveChatRequest request) {
        return send_direct_mutation(authorization, runtime_->make_leave_chat(request));
    }

    std::future<TdValue> send_login(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    const TdRequestOwner& owner, TdAuthRequest request) {
        if (!authorization) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        const auto function = request.function;
        return send(TdSendDescriptor{.function = function,
                                     .tier = DescriptorKind::AuthBootstrap,
                                     .owner = owner,
                                     .client_generation = authorization->client_generation,
                                     .auth_sequence = authorization->auth_sequence,
                                     .auth_state = authorization->data.state},
                    runtime_->make_auth_function(std::move(request)));
    }

    std::future<TdValue> send_logout(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                     TdClosedDecision& decision) {
        if (!authorization || !decision.state_) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        auto owner = issue_owner(TdOwnerKind::Request);
        if (!owner) {
            return failed_future(TdAuthorizationFailure::GenerationClosed);
        }
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard lock(state_mutex_);
            generation = current_;
        }
        if (!generation || generation->number != authorization->client_generation) {
            return failed_future(TdAuthorizationFailure::GenerationMismatch);
        }
        const TdSendDescriptor descriptor{.function = TdFunctionKind::LogOut,
                                          .tier = DescriptorKind::Destructive,
                                          .owner = owner.owner(),
                                          .client_generation = authorization->client_generation,
                                          .auth_sequence = authorization->auth_sequence,
                                          .auth_state = authorization->data.state};
        auto function = runtime_->make_function(TdBuiltinFunction::LogOut);
        auto decision_state = decision.state_;
        return generation->lifecycle.send([this, generation, descriptor,
                                           function = std::move(function),
                                           decision_state = std::move(decision_state)]() mutable {
            const std::lock_guard lock(generation->outbound_mutex);
            if (!generation->initial_state_installed) {
                return failed_future(TdAuthorizationFailure::AuthStateMismatch);
            }
            const auto& function_data = function.function_data();
            if (const auto failure = authorization_failure_locked(
                    generation, descriptor, function_data ? &*function_data : nullptr)) {
                return failed_future(*failure);
            }
            auto [query_id, future] = generation->queries.reserve();
            {
                const std::lock_guard decision_lock(decision_state->mutex);
                if (decision_state->status != TdClosedDecisionStatus::Pending ||
                    decision_state->query_id != 0) {
                    static_cast<void>(generation->queries.fail(
                        query_id, std::make_exception_ptr(TdAuthorizationError(
                                      TdAuthorizationFailure::GenerationClosed))));
                    return std::move(future);
                }
                decision_state->query_id = query_id;
            }
            try {
                runtime_->send(generation->client_id, generation->number, query_id, function);
            } catch (const std::exception&) {
                {
                    const std::lock_guard decision_lock(decision_state->mutex);
                    if (decision_state->status == TdClosedDecisionStatus::Pending) {
                        decision_state->status = TdClosedDecisionStatus::Rejected;
                    }
                }
                static_cast<void>(generation->queries.fail(query_id, std::current_exception()));
            }
            return std::move(future);
        });
    }

    TdClosedDecision begin_logout_decision(
        const std::shared_ptr<const AuthStateSnapshot>& authorization,
        std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim) {
        if (!authorization || authorization->data.state != AuthState::Ready || !claim) {
            return {};
        }
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard lock(state_mutex_);
            generation = current_;
        }
        if (!generation || generation->number != authorization->client_generation ||
            generation->client_id != authorization->client_id) {
            return {};
        }
        auto state = std::make_shared<TdClosedDecision::State>();
        state->claim = std::move(claim);
        state->release = [generation] {
            {
                const std::lock_guard lock(generation->closed_decision_mutex);
                if (generation->pending_closed_decisions > 0) {
                    --generation->pending_closed_decisions;
                }
            }
            generation->closed_decision_cv.notify_all();
        };
        {
            const std::lock_guard commit_lock(generation->auth_commit_mutex);
            const auto current = auth_state_.load(std::memory_order_acquire);
            if (!current || current->client_generation != authorization->client_generation ||
                current->auth_sequence != authorization->auth_sequence ||
                current->data.state != AuthState::Ready) {
                return {};
            }
            const std::lock_guard decision_lock(generation->closed_decision_mutex);
            if (generation->closed_committed) {
                return {};
            }
            std::erase_if(generation->closed_decisions,
                          [](const auto& entry) { return entry.expired(); });
            ++generation->pending_closed_decisions;
            generation->closed_decisions.emplace_back(state);
        }
        return TdClosedDecision(std::move(state));
    }

    bool restart_generation(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (!authorization || !generation ||
            generation->number != authorization->client_generation) {
            return false;
        }
        return generation->lifecycle.begin_close([this, generation] {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->close_requested = true;
            if (generation->initial_state_installed && !generation->final) {
                send_close_locked(generation);
            }
        });
    }

    TdSendLease acquire_send_lease(TdSendDescriptor descriptor) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr) {
            return {};
        }

        TdSendLease lease;
        const bool admitted = generation->lifecycle.admit([&] {
            auto held = std::make_shared<LeaseLocks>();
            held->auth_commit = std::unique_lock<std::mutex>(generation->auth_commit_mutex);
            held->outbound = std::unique_lock<std::mutex>(generation->outbound_mutex);
            if (!generation->initial_state_installed ||
                authorization_failure_locked(generation, descriptor,
                                             TdFunctionData{descriptor.function})) {
                return;
            }
            auto state = std::make_shared<TdSendLease::State>();
            state->submit = [this, generation, descriptor,
                             held = std::move(held)](TdlibParameters parameters) mutable {
                auto function = runtime_->make_set_tdlib_parameters(std::move(parameters));
                auto submission = submit_admitted_locked(generation, descriptor, function);
                held.reset();
                return std::move(submission.future);
            };
            lease = TdSendLease(std::move(state));
        });
        return admitted ? std::move(lease) : TdSendLease{};
    }

    static std::future<TdValue> send(TdSendLease lease, TdlibParameters parameters) {
        if (!lease.state_ || !lease.state_->submit) {
            return failed_future(TdAuthorizationFailure::AuthStateMismatch);
        }
        auto state = std::move(lease.state_);
        return state->submit(std::move(parameters));
    }

    TdRequestOwner internal_auth_owner() const {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_ == nullptr) {
            return {};
        }
        const std::lock_guard<std::mutex> outbound_lock(current_->outbound_mutex);
        return {TdOwnerKind::InternalAuth, current_->internal_auth_owner_id,
                current_->internal_auth_owner_capability};
    }

    TdOwnerLease issue_owner(TdOwnerKind kind) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr || (kind != TdOwnerKind::Login && kind != TdOwnerKind::Request)) {
            return {};
        }
        const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
        if (generation->final) {
            return {};
        }
        const auto id = next_owner_id_.fetch_add(1, std::memory_order_relaxed);
        auto capability = std::make_shared<const std::uint64_t>(id);
        (kind == TdOwnerKind::Login ? generation->login_owner_capabilities
                                    : generation->request_owner_capabilities)
            .insert(capability);
        auto state = std::make_unique<TdOwnerLease::State>();
        state->owner = {kind, id, capability};
        state->revoke = [generation, kind, capability = std::move(capability)] {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            (kind == TdOwnerKind::Login ? generation->login_owner_capabilities
                                        : generation->request_owner_capabilities)
                .erase(capability);
        };
        return TdOwnerLease(std::move(state));
    }

    bool owns(const TdRequestOwner& owner, std::uint64_t client_generation) const {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr || generation->number != client_generation) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
        switch (owner.kind) {
        case TdOwnerKind::InternalAuth:
            return owner.capability == generation->internal_auth_owner_capability;
        case TdOwnerKind::Lifecycle:
            return owner.capability == generation->lifecycle_owner_capability;
        case TdOwnerKind::Login:
            return generation->login_owner_capabilities.contains(owner.capability);
        case TdOwnerKind::Request:
            return generation->request_owner_capabilities.contains(owner.capability);
        }
        return false;
    }

    std::uint64_t subscribe_updates(UpdateHandler handler) {
        return updates_.subscribe(std::move(handler));
    }

    void unsubscribe_updates(std::uint64_t id) {
        updates_.unsubscribe(id);
    }

    std::uint64_t subscribe_send_updates(UpdateHandler handler) {
        return send_updates_.subscribe(std::move(handler));
    }

    void unsubscribe_send_updates(std::uint64_t id) {
        send_updates_.unsubscribe(id);
    }

    std::uint64_t subscribe_response_completions(ResponseCompletionHandler handler) {
        return response_completions_.subscribe(std::move(handler));
    }

    void unsubscribe_response_completions(std::uint64_t id) {
        response_completions_.unsubscribe(id);
    }

    std::shared_ptr<const AuthStateSnapshot> auth_state() const {
        return auth_state_.load(std::memory_order_acquire);
    }

    std::uint64_t subscribe_auth_states(AuthStateHandler handler) {
        return auth_states_.subscribe(std::move(handler));
    }

    void unsubscribe_auth_states(std::uint64_t id) {
        auth_states_.unsubscribe(id);
    }

    std::mutex& event_publication_mutex() const {
        return event_publication_mutex_;
    }

    bool close_until(std::chrono::steady_clock::time_point deadline) {
        std::call_once(close_begin_once_, [this] {
            std::shared_ptr<Generation> generation;
            {
                const std::lock_guard<std::mutex> lock(state_mutex_);
                shutting_down_ = true;
                generation = current_;
                shutdown_generation_ = generation == nullptr ? 0 : generation->number;
            }

            if (generation != nullptr) {
                static_cast<void>(generation->lifecycle.begin_close([this, generation] {
                    const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
                    generation->close_requested = true;
                    if (generation->initial_state_installed && !generation->final) {
                        send_close_locked(generation);
                    }
                }));
            }
        });

        {
            std::unique_lock<std::mutex> lock(closed_mutex_);
            if (shutdown_generation_ != 0 &&
                !closed_cv_.wait_until(lock, deadline, [this] { return shutdown_closed_; })) {
                return false;
            }
        }
        finalize_shutdown();
        return true;
    }

    void close() {
        if (!close_until(std::chrono::steady_clock::now() + kCloseTimeout)) {
            std::fputs("warning: tdlib did not reach authorizationStateClosed within 30s; "
                       "shutting down without the clean-close guarantee\n",
                       stderr);
            finalize_shutdown();
        }
    }

  private:
    void finalize_shutdown() {
        std::call_once(finalize_once_, [this] {
            stop_.store(true, std::memory_order_release);
            if (receive_thread_.joinable()) {
                receive_thread_.join();
            }
            std::shared_ptr<Generation> generation;
            {
                const std::lock_guard<std::mutex> lock(state_mutex_);
                generation = current_;
            }
            if (generation != nullptr) {
                generation->queries.fail_all("tdlib client closed");
            }
        });
    }

    struct Submission {
        std::uint64_t query_id = 0;
        std::future<TdValue> future;
        std::exception_ptr synchronous_failure;
    };

    struct LeaseLocks {
        std::unique_lock<std::mutex> auth_commit;
        std::unique_lock<std::mutex> outbound;
    };

    struct PreparedWriteResources {
        PreparedWriteResources() = default;
        ~PreparedWriteResources() {
            release_locks_and_owner();
            request.reset();
            lifetime.reset();
        }
        PreparedWriteResources(const PreparedWriteResources&) = delete;
        PreparedWriteResources& operator=(const PreparedWriteResources&) = delete;
        PreparedWriteResources(PreparedWriteResources&&) = delete;
        PreparedWriteResources& operator=(PreparedWriteResources&&) = delete;

        void release_locks_and_owner() {
            held.reset();
            owner.reset();
        }

        std::shared_ptr<TdOwnerLease> owner;
        std::shared_ptr<LeaseLocks> held;
        std::optional<TdValue> request;
        TdQueryLifetime lifetime;
    };

    struct Generation {
        Generation(std::int32_t client_id_value, std::uint64_t number_value,
                   std::unique_ptr<TdGenerationObserver> observer_value)
            : client_id(client_id_value), number(number_value),
              observer(std::move(observer_value)) {}

        std::int32_t client_id;
        std::uint64_t number;
        std::unique_ptr<TdGenerationObserver> observer;
        std::uint64_t current_state_query_id = 0;
        std::uint64_t last_receive_event_sequence = 0;
        QueryRegistry<TdValue> queries;
        detail::RequestLifecycle<TdValue> lifecycle{"tdlib client generation closed"};
        std::mutex auth_commit_mutex;
        std::mutex outbound_mutex;
        std::mutex closed_decision_mutex;
        mutable std::mutex m6_cache_mutex;
        std::condition_variable closed_decision_cv;
        std::size_t pending_closed_decisions = 0;
        bool closed_committed = false;
        std::vector<std::weak_ptr<TdClosedDecision::State>> closed_decisions;
        std::uint64_t internal_auth_owner_id = 0;
        std::uint64_t lifecycle_owner_id = 0;
        std::shared_ptr<const void> internal_auth_owner_capability;
        std::shared_ptr<const void> lifecycle_owner_capability;
        std::unordered_set<std::shared_ptr<const void>> login_owner_capabilities;
        std::unordered_set<std::shared_ptr<const void>> request_owner_capabilities;
        bool initial_state_installed = false;
        bool accepted_auth_update = false;
        bool close_requested = false;
        bool close_sent = false;
        bool final = false;
        std::deque<TdValue> pending_updates;
        std::optional<TdM6ChatFoldersUpdate> m6_chat_folders;
    };

    std::shared_ptr<Generation> make_generation() {
        const auto generation_number = next_generation_++;
        const auto client_id = runtime_->create_client(generation_number);
        auto observer = generation_observer_factory_
                            ? generation_observer_factory_(client_id, generation_number)
                            : nullptr;
        if (generation_observer_factory_ && observer == nullptr) {
            throw std::logic_error("TdClient generation observer factory returned null");
        }
        auto generation =
            std::make_shared<Generation>(client_id, generation_number, std::move(observer));
        generation->internal_auth_owner_id = next_owner_id_.fetch_add(1, std::memory_order_relaxed);
        generation->lifecycle_owner_id = next_owner_id_.fetch_add(1, std::memory_order_relaxed);
        generation->internal_auth_owner_capability =
            std::make_shared<const std::uint64_t>(generation->internal_auth_owner_id);
        generation->lifecycle_owner_capability =
            std::make_shared<const std::uint64_t>(generation->lifecycle_owner_id);
        auto unknown = std::make_shared<const AuthStateSnapshot>(
            AuthStateSnapshot{.client_id = client_id,
                              .client_generation = generation_number,
                              .auth_sequence = 0,
                              .data = AuthStateData{AuthState::Unknown},
                              .receive_observed_at = std::nullopt});
        auth_state_.store(std::move(unknown), std::memory_order_release);

        const TdSendDescriptor descriptor{
            .function = TdFunctionKind::GetAuthorizationState,
            .tier = DescriptorKind::AuthBootstrap,
            .owner = {TdOwnerKind::InternalAuth, generation->internal_auth_owner_id,
                      generation->internal_auth_owner_capability},
            .client_generation = generation_number,
            .auth_sequence = 0,
            .auth_state = AuthState::Unknown,
        };
        auto function = runtime_->make_function(TdBuiltinFunction::GetAuthorizationState);
        auto submission = submit_locked(generation, descriptor, function);
        if (submission.query_id != 1) {
            throw std::logic_error("authorization bootstrap must reserve query id 1");
        }
        const TdSendDescriptor current_state_descriptor{
            .function = TdFunctionKind::GetCurrentState,
            .tier = DescriptorKind::AuthBootstrap,
            .owner = {TdOwnerKind::InternalAuth, generation->internal_auth_owner_id,
                      generation->internal_auth_owner_capability},
            .client_generation = generation_number,
            .auth_sequence = 0,
            .auth_state = AuthState::Unknown,
        };
        auto current_state = runtime_->make_get_current_state();
        auto current_state_submission =
            submit_locked(generation, current_state_descriptor, current_state);
        if (current_state_submission.query_id != 2) {
            throw std::logic_error("current-state bootstrap must reserve query id 2");
        }
        if (current_state_submission.synchronous_failure) {
            if (generation->observer != nullptr) {
                generation->observer->on_current_state_failure(
                    current_state_submission.synchronous_failure);
            }
        } else {
            generation->current_state_query_id = current_state_submission.query_id;
        }
        return generation;
    }

    void activate_initial_generation() {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        current_ = make_generation();
    }

    Submission submit_locked(const std::shared_ptr<Generation>& generation,
                             const TdSendDescriptor& descriptor, TdValue& function,
                             const TdQueryLifetime& lifetime = {}) {
        const auto& function_data = function.function_data();
        if (const auto failure = authorization_failure_locked(
                generation, descriptor, function_data ? &*function_data : nullptr)) {
            return {0, failed_future(*failure), {}};
        }
        return submit_admitted_locked(generation, descriptor, function, lifetime);
    }

    std::optional<TdAuthorizationFailure>
    authorization_failure_locked(const std::shared_ptr<Generation>& generation,
                                 const TdSendDescriptor& descriptor,
                                 const TdFunctionData& function) const {
        return authorization_failure_locked(generation, descriptor, &function);
    }

    std::optional<TdAuthorizationFailure>
    authorization_failure_locked(const std::shared_ptr<Generation>& generation,
                                 const TdSendDescriptor& descriptor,
                                 const TdFunctionData* function) const {
        const auto snapshot = auth_state_.load(std::memory_order_acquire);
        if (snapshot == nullptr) {
            return TdAuthorizationFailure::AuthStateMismatch;
        }
        if (const auto failure =
                authorize_td_send(descriptor, function, *snapshot, generation->final)) {
            return failure;
        }
        const bool owner_registered = [&] {
            switch (descriptor.owner.kind) {
            case TdOwnerKind::InternalAuth:
                return descriptor.owner.capability == generation->internal_auth_owner_capability;
            case TdOwnerKind::Lifecycle:
                return descriptor.owner.capability == generation->lifecycle_owner_capability;
            case TdOwnerKind::Login:
                return generation->login_owner_capabilities.contains(descriptor.owner.capability);
            case TdOwnerKind::Request:
                return generation->request_owner_capabilities.contains(descriptor.owner.capability);
            }
            return false;
        }();
        if (!owner_registered) {
            return TdAuthorizationFailure::OwnerMismatch;
        }
        return std::nullopt;
    }

    Submission submit_admitted_locked(const std::shared_ptr<Generation>& generation,
                                      const TdSendDescriptor& admitted_descriptor,
                                      TdValue& function, const TdQueryLifetime& lifetime = {}) {
        static_cast<void>(admitted_descriptor);
        auto [query_id, future] = generation->queries.reserve(lifetime);
        std::exception_ptr synchronous_failure;
        try {
            runtime_->send(generation->client_id, generation->number, query_id, function);
        } catch (const std::exception&) {
            synchronous_failure = std::current_exception();
            static_cast<void>(generation->queries.fail(query_id, synchronous_failure));
        }
        return {query_id, std::move(future), std::move(synchronous_failure)};
    }

    void send_close_locked(const std::shared_ptr<Generation>& generation) {
        if (generation->close_sent) {
            return;
        }
        generation->close_sent = true;
        const auto snapshot = auth_state_.load(std::memory_order_acquire);
        if (snapshot == nullptr) {
            return;
        }
        const TdSendDescriptor descriptor{
            .function = TdFunctionKind::Close,
            .tier = DescriptorKind::Lifecycle,
            .owner = {TdOwnerKind::Lifecycle, generation->lifecycle_owner_id,
                      generation->lifecycle_owner_capability},
            .client_generation = generation->number,
            .auth_sequence = snapshot->auth_sequence,
            .auth_state = snapshot->data.state,
        };
        auto function = runtime_->make_function(TdBuiltinFunction::Close);
        static_cast<void>(submit_locked(generation, descriptor, function));
    }

    static std::vector<std::shared_ptr<TdClosedDecision::State>>
    active_closed_decisions(const std::shared_ptr<Generation>& generation) {
        std::vector<std::shared_ptr<TdClosedDecision::State>> result;
        const std::lock_guard lock(generation->closed_decision_mutex);
        std::erase_if(generation->closed_decisions,
                      [](const auto& entry) { return entry.expired(); });
        result.reserve(generation->closed_decisions.size());
        for (const auto& entry : generation->closed_decisions) {
            if (auto state = entry.lock()) {
                result.push_back(std::move(state));
            }
        }
        return result;
    }

    void claim_lifecycle_event(const std::shared_ptr<TdClosedDecision::State>& state,
                               TdClosedDecisionStatus accepted_status, std::uint64_t query_id = 0) {
        std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim;
        std::chrono::steady_clock::time_point committed_at;
        {
            std::unique_lock lock(state->mutex);
            if (before_lifecycle_callback_drain_wait_) {
                before_lifecycle_callback_drain_wait_();
            }
            state->callbacks_done.wait(lock, [&] { return state->callbacks_in_flight == 0; });
            if (state->released || state->status != TdClosedDecisionStatus::Pending ||
                (accepted_status == TdClosedDecisionStatus::Error &&
                 (state->query_id != query_id || state->expected_transition))) {
                return;
            }
            committed_at = std::chrono::steady_clock::now();
            ++state->callbacks_in_flight;
            claim = state->claim;
        }
        const auto claimed = claim ? claim(committed_at) : TdLifecycleClaimStatus::Rejected;
        {
            const std::lock_guard lock(state->mutex);
            if (!state->released && state->status == TdClosedDecisionStatus::Pending) {
                if (claimed == TdLifecycleClaimStatus::Active) {
                    if (accepted_status == TdClosedDecisionStatus::Pending) {
                        state->expected_transition = true;
                    } else if (accepted_status == TdClosedDecisionStatus::Closed &&
                               state->query_id == 0) {
                        state->status = TdClosedDecisionStatus::Rejected;
                    } else {
                        state->status = accepted_status;
                    }
                } else {
                    state->status = terminal_decision(claimed);
                }
            }
            --state->callbacks_in_flight;
        }
        state->callbacks_done.notify_all();
    }

    void note_expected_transition(const std::shared_ptr<Generation>& generation) {
        for (const auto& state : active_closed_decisions(generation)) {
            claim_lifecycle_event(state, TdClosedDecisionStatus::Pending);
        }
    }

    void resolve_lifecycle_event(const std::shared_ptr<Generation>& generation,
                                 TdClosedDecisionStatus accepted_status,
                                 std::uint64_t query_id = 0) {
        if (accepted_status == TdClosedDecisionStatus::Closed) {
            const std::lock_guard lock(generation->closed_decision_mutex);
            generation->closed_committed = true;
        }
        for (const auto& state : active_closed_decisions(generation)) {
            claim_lifecycle_event(state, accepted_status, query_id);
        }
    }

    void receive_loop() {
        while (!stop_.load(std::memory_order_acquire)) {
            auto event = runtime_->receive(kReceiveTimeout);
            if (!event.has_value()) {
                std::shared_ptr<Generation> generation;
                {
                    const std::lock_guard<std::mutex> lock(state_mutex_);
                    generation = current_;
                }
                receive_boundary(generation);
                continue;
            }
            handle_event(std::move(*event));
        }
    }

    static void receive_boundary(const std::shared_ptr<Generation>& generation) noexcept {
        if (generation != nullptr && generation->observer != nullptr) {
            generation->observer->on_receive_boundary(generation->last_receive_event_sequence);
        }
    }

    static void observe_authorization(const std::shared_ptr<Generation>& generation,
                                      const AuthStateData& state,
                                      std::uint64_t receive_event_sequence) noexcept {
        if (generation->observer != nullptr) {
            generation->observer->on_authorization_state(state, receive_event_sequence);
        }
    }

    static void retain_m6_folder_update(const std::shared_ptr<Generation>& generation,
                                        const TdValue& update) {
        const auto* folders = update.get_if<TdM6ChatFoldersUpdate>();
        if (folders == nullptr || !valid_td_m6_chat_folders_update(*folders)) {
            return;
        }
        const std::lock_guard lock(generation->m6_cache_mutex);
        generation->m6_chat_folders = *folders;
    }

    static void retain_m6_current_state(const std::shared_ptr<Generation>& generation,
                                        TdValue& value) {
        auto* state = value.get_if<TdCurrentState>();
        if (state == nullptr) {
            return;
        }
        for (auto& update : state->updates) {
            auto* folders = update.get_if<TdM6ChatFoldersUpdate>();
            if (folders == nullptr || !valid_td_m6_chat_folders_update(*folders)) {
                continue;
            }
            const std::lock_guard lock(generation->m6_cache_mutex);
            generation->m6_chat_folders = std::move(*folders);
        }
    }

    struct EventPublication {
        std::unique_lock<std::mutex> lock;
        TdEventClock::time_point observed_at;
    };

    struct AuthPublication {
        std::shared_ptr<const AuthStateSnapshot> snapshot;
        std::deque<TdValue> pending_updates;
        bool close_requested = false;
    };

    // Auth publication lock order is auth_commit -> outbound -> gate. Response
    // promises are detached from the query registry before any of those locks.
    // Clock/test callbacks, lifecycle coordination, TD sends, and generic
    // subscriber callbacks stay outside the gate. Dedicated send-update
    // subscribers run inside it and may only queue the stamped update, which
    // makes deadline arbitration atomic with their visibility.
    TdEventClock::time_point observe_event() {
        const auto observed_at = event_now_();
        if (after_event_observed_) {
            after_event_observed_(observed_at);
        }
        return observed_at;
    }

    EventPublication begin_event_publication(TdEventClock::time_point observed_at) {
        std::unique_lock lock(event_publication_mutex_);
        return {std::move(lock), observed_at};
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): atomic auth/query event arbiter.
    void handle_event(TdRuntimeEvent event) {
        std::shared_ptr<Generation> generation;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            generation = current_;
        }
        if (generation == nullptr || event.client_id != generation->client_id ||
            event.client_generation != generation->number) {
            receive_boundary(generation);
            return;
        }
        const bool closes_generation =
            event.authorization_state && event.authorization_state->state == AuthState::Closed;
        if (closes_generation) {
            close_generation_admission(generation);
        }
        const auto receive_event_sequence = next_receive_event_sequence_++;
        generation->last_receive_event_sequence = receive_event_sequence;

        if (event.authorization_state) {
            if (event.authorization_state->state == AuthState::LoggingOut ||
                event.authorization_state->state == AuthState::Closing) {
                note_expected_transition(generation);
            } else if (closes_generation) {
                resolve_lifecycle_event(generation, TdClosedDecisionStatus::Closed);
            }
        }
        if (event.query_id != 0 && event.object.get_if<TdError>() != nullptr) {
            resolve_lifecycle_event(generation, TdClosedDecisionStatus::Error, event.query_id);
        }

        if (event.query_id == 0) {
            handle_update(generation, std::move(event), receive_event_sequence);
            return;
        }

        auto response = generation->queries.take(event.query_id);
        const bool current_state_response =
            response && event.query_id == generation->current_state_query_id &&
            std::exchange(generation->current_state_query_id, 0) == event.query_id;
        std::optional<LeaseLocks> auth_locks;
        bool install_response = false;
        if (event.query_id == 1 && event.authorization_state.has_value()) {
            auth_locks.emplace();
            auth_locks->auth_commit = std::unique_lock<std::mutex>(generation->auth_commit_mutex);
            auth_locks->outbound = std::unique_lock<std::mutex>(generation->outbound_mutex);
            install_response =
                !generation->accepted_auth_update && !generation->initial_state_installed;
        }
        const bool installed_closed = install_response && closes_generation;

        std::optional<AuthPublication> auth_publication;
        {
            const auto observed_at = observe_event();
            if (install_response) {
                auth_publication.emplace(prepare_auth_publication(
                    generation, *event.authorization_state, receive_event_sequence, observed_at));
            }
            auto publication = begin_event_publication(observed_at);
            event.object.set_receive_event_metadata(receive_event_sequence,
                                                    publication.observed_at);
            if (current_state_response && generation->observer != nullptr) {
                generation->observer->on_current_state(event.object);
            }
            if (current_state_response) {
                retain_m6_current_state(generation, event.object);
            }
            if (install_response) {
                commit_auth_state_locked(generation, *auth_publication, false);
                observe_authorization(generation, *event.authorization_state,
                                      receive_event_sequence);
            }
            if (response) {
                response->promise.set_value(std::move(event.object));
            }
        }
        auth_locks.reset();
        if (auth_publication) {
            finish_auth_publication(generation, *auth_publication);
        }
        if (response) {
            response_completions_.publish(receive_event_sequence);
        }
        receive_boundary(generation);
        if (installed_closed) {
            handle_closed(generation);
        }
    }

    void handle_update(const std::shared_ptr<Generation>& generation, TdRuntimeEvent event,
                       std::uint64_t receive_event_sequence) {
        if (event.authorization_state.has_value()) {
            const auto closed = event.authorization_state->state == AuthState::Closed;
            LeaseLocks locks;
            locks.auth_commit = std::unique_lock<std::mutex>(generation->auth_commit_mutex);
            locks.outbound = std::unique_lock<std::mutex>(generation->outbound_mutex);
            const auto observed_at = observe_event();
            auto auth_publication = prepare_auth_publication(generation, *event.authorization_state,
                                                             receive_event_sequence, observed_at);
            {
                auto publication = begin_event_publication(observed_at);
                event.object.set_receive_event_metadata(receive_event_sequence,
                                                        publication.observed_at);
                commit_auth_state_locked(generation, auth_publication, true);
                observe_authorization(generation, *event.authorization_state,
                                      receive_event_sequence);
            }
            locks = {};
            finish_auth_publication(generation, auth_publication);
            updates_.publish(event.object);
            receive_boundary(generation);
            if (closed) {
                handle_closed(generation);
            }
            return;
        }

        bool pending = false;
        {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            const auto observed_at = observe_event();
            auto publication = begin_event_publication(observed_at);
            event.object.set_receive_event_metadata(receive_event_sequence,
                                                    publication.observed_at);
            if (generation->observer != nullptr) {
                generation->observer->on_update(event.object);
            }
            if (!generation->initial_state_installed) {
                generation->pending_updates.push_back(std::move(event.object));
                pending = true;
            } else if (event.object.get_if<TdUpdateMessageSendSucceeded>() != nullptr ||
                       event.object.get_if<TdUpdateMessageSendFailed>() != nullptr ||
                       event.object.get_if<TdUpdateDeleteMessages>() != nullptr) {
                send_updates_.publish(event.object);
            }
        }
        if (!pending) {
            retain_m6_folder_update(generation, event.object);
            updates_.publish(event.object);
        }
        receive_boundary(generation);
    }

    AuthPublication prepare_auth_publication(const std::shared_ptr<Generation>& generation,
                                             const AuthStateData& state,
                                             std::uint64_t receive_event_sequence,
                                             TdEventClock::time_point observed_at) {
        AuthPublication publication;
        const auto previous = auth_state_.load(std::memory_order_acquire);
        const auto sequence =
            previous != nullptr && previous->client_generation == generation->number
                ? previous->auth_sequence + 1
                : 1;
        publication.snapshot = std::make_shared<const AuthStateSnapshot>(
            AuthStateSnapshot{.client_id = generation->client_id,
                              .client_generation = generation->number,
                              .auth_sequence = sequence,
                              .receive_event_sequence = receive_event_sequence,
                              .data = state,
                              .receive_observed_at = observed_at});
        return publication;
    }

    void commit_auth_state_locked(const std::shared_ptr<Generation>& generation,
                                  AuthPublication& publication, bool from_update) {
        if (publication.snapshot->data.state != AuthState::Ready) {
            const std::lock_guard cache_lock(generation->m6_cache_mutex);
            generation->m6_chat_folders.reset();
        }
        auth_state_.store(publication.snapshot, std::memory_order_release);
        generation->initial_state_installed = true;
        generation->accepted_auth_update |= from_update;
        if (publication.snapshot->data.state != AuthState::Closed) {
            publication.close_requested = generation->close_requested;
            publication.pending_updates.swap(generation->pending_updates);
        }
    }

    void finish_auth_publication(const std::shared_ptr<Generation>& generation,
                                 const AuthPublication& publication) {
        if (publication.close_requested) {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            send_close_locked(generation);
        }
        for (const auto& update : publication.pending_updates) {
            retain_m6_folder_update(generation, update);
            updates_.publish(update);
        }
        auth_states_.publish(publication.snapshot);
    }

    static void close_generation_admission(const std::shared_ptr<Generation>& generation) {
        const auto closed_now = generation->lifecycle.begin_close([generation] {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->final = true;
            generation->pending_updates.clear();
        });
        if (!closed_now) {
            const std::lock_guard<std::mutex> lock(generation->outbound_mutex);
            generation->final = true;
            generation->pending_updates.clear();
        }
    }

    void handle_closed(const std::shared_ptr<Generation>& generation) {
        {
            std::unique_lock lock(generation->closed_decision_mutex);
            if (before_closed_decisions_drain_wait_) {
                before_closed_decisions_drain_wait_();
            }
            generation->closed_decision_cv.wait(
                lock, [&] { return generation->pending_closed_decisions == 0; });
        }
        generation->queries.fail_all("tdlib client generation closed");

        bool notify_shutdown = false;
        std::shared_ptr<const AuthStateSnapshot> unknown;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            if (current_ != generation) {
                return;
            }
            if (shutting_down_ && shutdown_generation_ == generation->number) {
                notify_shutdown = true;
            } else if (!shutting_down_) {
                current_ = make_generation();
                unknown = auth_state_.load(std::memory_order_acquire);
            }
        }
        if (notify_shutdown) {
            {
                const std::lock_guard<std::mutex> lock(closed_mutex_);
                shutdown_closed_ = true;
            }
            closed_cv_.notify_all();
            return;
        }
        if (unknown != nullptr) {
            auth_states_.publish(unknown);
        }
    }

    std::unique_ptr<TdRuntime> runtime_;
    std::function<TdEventClock::time_point()> event_now_;
    std::function<void(TdEventClock::time_point)> after_event_observed_;
    std::function<void()> before_lifecycle_callback_drain_wait_;
    std::function<void()> before_closed_decisions_drain_wait_;
    TdGenerationObserverFactory generation_observer_factory_;
    mutable std::mutex event_publication_mutex_;
    mutable std::mutex state_mutex_;
    std::shared_ptr<Generation> current_;
    std::uint64_t next_generation_ = 1;
    std::uint64_t next_receive_event_sequence_ = 1;
    std::atomic<std::uint64_t> next_owner_id_{1};
    bool shutting_down_ = false;
    std::uint64_t shutdown_generation_ = 0;
    std::atomic<std::shared_ptr<const AuthStateSnapshot>> auth_state_;
    UpdateBus<TdValue> updates_;
    UpdateBus<TdValue> send_updates_;
    UpdateBus<std::uint64_t> response_completions_;
    UpdateBus<std::shared_ptr<const AuthStateSnapshot>> auth_states_;
    std::atomic<bool> stop_{false};
    std::mutex closed_mutex_;
    std::condition_variable closed_cv_;
    bool shutdown_closed_ = false;
    std::once_flag close_begin_once_;
    std::once_flag finalize_once_;
    std::thread receive_thread_;
};

TdAuthorizationError::TdAuthorizationError(TdAuthorizationFailure failure)
    : std::runtime_error("TDLib send denied: " + std::string(authorization_failure_name(failure))),
      failure_(failure) {}

TdSendLease::TdSendLease() = default;
TdSendLease::~TdSendLease() = default;
TdSendLease::TdSendLease(std::shared_ptr<State> state) : state_(std::move(state)) {}
TdSendLease::TdSendLease(TdSendLease&&) noexcept = default;
TdSendLease& TdSendLease::operator=(TdSendLease&& other) noexcept = default;

TdSendLease::operator bool() const noexcept {
    return state_ != nullptr;
}

TdPreparedWrite::TdPreparedWrite() = default;
TdPreparedWrite::~TdPreparedWrite() = default;
TdPreparedWrite::TdPreparedWrite(std::shared_ptr<State> state) : state_(std::move(state)) {}
TdPreparedWrite::TdPreparedWrite(TdPreparedWrite&&) noexcept = default;
TdPreparedWrite& TdPreparedWrite::operator=(TdPreparedWrite&& other) noexcept = default;

TdPreparedWrite::operator bool() const noexcept {
    return state_ != nullptr && static_cast<bool>(state_->submit);
}

std::optional<TdAuthorizationFailure> TdPreparedWrite::authorization_failure() const noexcept {
    return state_ ? state_->authorization_failure : std::nullopt;
}

TdClosedDecision::TdClosedDecision() = default;
TdClosedDecision::~TdClosedDecision() {
    if (state_) {
        std::function<void()> release;
        {
            std::unique_lock lock(state_->mutex);
            state_->released = true;
            state_->claim = {};
            state_->callbacks_done.wait(lock, [this] { return state_->callbacks_in_flight == 0; });
            release = std::move(state_->release);
        }
        if (release) {
            release();
        }
    }
}
TdClosedDecision::TdClosedDecision(std::shared_ptr<State> state) : state_(std::move(state)) {}
TdClosedDecision::TdClosedDecision(TdClosedDecision&&) noexcept = default;
TdClosedDecision& TdClosedDecision::operator=(TdClosedDecision&& other) noexcept {
    if (this != &other) {
        if (state_) {
            std::function<void()> release;
            {
                std::unique_lock lock(state_->mutex);
                state_->released = true;
                state_->claim = {};
                state_->callbacks_done.wait(lock,
                                            [this] { return state_->callbacks_in_flight == 0; });
                release = std::move(state_->release);
            }
            if (release) {
                release();
            }
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

TdClosedDecision::operator bool() const noexcept {
    return state_ != nullptr;
}

TdClosedDecisionStatus TdClosedDecision::status() const {
    if (!state_) {
        return TdClosedDecisionStatus::Rejected;
    }
    const std::lock_guard lock(state_->mutex);
    return state_->status;
}

TdClosedDecisionStatus TdClosedDecision::settle_terminal() {
    if (!state_) {
        return TdClosedDecisionStatus::Rejected;
    }
    std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim;
    {
        std::unique_lock lock(state_->mutex);
        state_->callbacks_done.wait(lock, [this] { return state_->callbacks_in_flight == 0; });
        if (state_->released || state_->status != TdClosedDecisionStatus::Pending) {
            return state_->status;
        }
        ++state_->callbacks_in_flight;
        claim = state_->claim;
    }
    const auto claimed =
        claim ? claim(std::chrono::steady_clock::now()) : TdLifecycleClaimStatus::Rejected;
    {
        const std::lock_guard lock(state_->mutex);
        if (!state_->released && state_->status == TdClosedDecisionStatus::Pending) {
            state_->status = terminal_decision(claimed);
        }
        --state_->callbacks_in_flight;
    }
    state_->callbacks_done.notify_all();
    return status();
}

TdOwnerLease::TdOwnerLease() = default;
TdOwnerLease::~TdOwnerLease() {
    if (state_ && state_->revoke) {
        state_->revoke();
    }
}
TdOwnerLease::TdOwnerLease(std::unique_ptr<State> state) : state_(std::move(state)) {}
TdOwnerLease::TdOwnerLease(TdOwnerLease&&) noexcept = default;
TdOwnerLease& TdOwnerLease::operator=(TdOwnerLease&& other) noexcept {
    if (this != &other) {
        if (state_ && state_->revoke) {
            state_->revoke();
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

TdRequestOwner TdOwnerLease::owner() const noexcept {
    return state_ ? state_->owner : TdRequestOwner{};
}

TdOwnerLease::operator bool() const noexcept {
    return state_ != nullptr;
}

TdClient::TdClient(const TdLogConfiguration& logging)
    : TdClient(make_production_td_runtime(), logging) {}

TdClient::TdClient(const TdLogConfiguration& logging,
                   TdGenerationObserverFactory generation_observer_factory)
    : TdClient(make_production_td_runtime(), logging, {}, std::move(generation_observer_factory)) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime)
    : TdClient(std::move(runtime), TdLogConfiguration{}) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging)
    : TdClient(std::move(runtime), logging, {}) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging,
                   TdClientEventHooks event_hooks)
    : TdClient(std::move(runtime), logging, std::move(event_hooks), {}) {}

TdClient::TdClient(std::unique_ptr<TdRuntime> runtime, const TdLogConfiguration& logging,
                   TdClientEventHooks event_hooks,
                   TdGenerationObserverFactory generation_observer_factory)
    : impl_(std::make_unique<Impl>(std::move(runtime), logging, std::move(event_hooks),
                                   std::move(generation_observer_factory))) {}

TdClient::~TdClient() = default;

std::future<TdValue> TdClient::send(TdSendDescriptor descriptor, TdValue request) {
    return impl_->send(std::move(descriptor), std::move(request));
}

std::future<TdValue> TdClient::send(TdSendDescriptor descriptor, TdlibParameters parameters) {
    return impl_->send(std::move(descriptor), std::move(parameters));
}

TdSendLease TdClient::acquire_send_lease(TdSendDescriptor descriptor) {
    return impl_->acquire_send_lease(std::move(descriptor));
}

std::future<TdValue> TdClient::send(TdSendLease&& lease, TdlibParameters parameters) {
    return impl_->send(std::move(lease), std::move(parameters));
}

std::future<TdValue>
TdClient::send_read(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                    TdFunctionKind function, TdValue request) {
    return impl_->send_read(authorization, function, std::move(request));
}

std::future<TdValue>
TdClient::get_me(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->get_me(authorization);
}

std::future<TdValue>
TdClient::get_contacts(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->get_contacts(authorization);
}

std::future<TdValue>
TdClient::m6_read(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                  TdM6Request request) {
    return impl_->m6_read(authorization, std::move(request));
}

std::optional<TdM6ChatFoldersUpdate>
TdClient::m6_chat_folders(const std::shared_ptr<const AuthStateSnapshot>& authorization) const {
    return impl_->m6_chat_folders(authorization);
}

std::future<TdValue>
TdClient::get_saved_messages_tags(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t saved_messages_topic_id) {
    return impl_->get_saved_messages_tags(authorization, saved_messages_topic_id);
}

std::future<TdValue>
TdClient::search_saved_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                TdSearchSavedMessagesRequest request) {
    return impl_->search_saved_messages(authorization, std::move(request));
}

std::future<TdValue>
TdClient::get_active_sessions(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->get_active_sessions(authorization);
}

std::future<TdValue>
TdClient::get_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                   std::int64_t chat_id) {
    return impl_->get_chat(authorization, chat_id);
}

std::future<TdValue>
TdClient::get_chat_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::int64_t chat_id, std::int64_t from_message_id, std::int32_t offset,
                           std::int32_t limit, bool only_local) {
    return impl_->get_chat_history(authorization, chat_id, from_message_id, offset, limit,
                                   only_local);
}

std::future<TdValue>
TdClient::get_chat_message_by_date(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   std::int64_t chat_id, std::int32_t date) {
    return impl_->get_chat_message_by_date(authorization, chat_id, date);
}

std::future<TdValue>
TdClient::get_message_thread(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::int64_t chat_id, std::int64_t message_id) {
    return impl_->get_message_thread(authorization, chat_id, message_id);
}

std::future<TdValue>
TdClient::get_forum_topic_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  std::int64_t chat_id, std::int32_t forum_topic_id,
                                  std::int64_t from_message_id, std::int32_t offset,
                                  std::int32_t limit) {
    return impl_->get_forum_topic_history(authorization, chat_id, forum_topic_id, from_message_id,
                                          offset, limit);
}

std::future<TdValue>
TdClient::get_message_thread_history(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                     std::int64_t chat_id, std::int64_t message_id,
                                     std::int64_t from_message_id, std::int32_t offset,
                                     std::int32_t limit) {
    return impl_->get_message_thread_history(authorization, chat_id, message_id, from_message_id,
                                             offset, limit);
}

std::future<TdValue> TdClient::get_direct_messages_chat_topic_history(
    const std::shared_ptr<const AuthStateSnapshot>& authorization, std::int64_t chat_id,
    std::int64_t topic_id, std::int64_t from_message_id, std::int32_t offset, std::int32_t limit) {
    return impl_->get_direct_messages_chat_topic_history(authorization, chat_id, topic_id,
                                                         from_message_id, offset, limit);
}

std::future<TdValue> TdClient::get_saved_messages_topic_history(
    const std::shared_ptr<const AuthStateSnapshot>& authorization, std::int64_t topic_id,
    std::int64_t from_message_id, std::int32_t offset, std::int32_t limit) {
    return impl_->get_saved_messages_topic_history(authorization, topic_id, from_message_id, offset,
                                                   limit);
}

std::future<TdValue>
TdClient::get_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       std::int64_t chat_id, std::vector<std::int64_t> message_ids) {
    return impl_->get_messages(authorization, chat_id, std::move(message_ids));
}

std::future<TdValue>
TdClient::get_message_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           std::int64_t chat_id, std::int64_t message_id,
                           std::int32_t media_timestamp, std::int32_t checklist_task_id,
                           std::string poll_option_id, bool for_album, bool in_message_thread) {
    return impl_->get_message_link(authorization, chat_id, message_id, media_timestamp,
                                   checklist_task_id, std::move(poll_option_id), for_album,
                                   in_message_thread);
}

std::future<TdValue>
TdClient::get_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                    TdChatListKind list, std::int32_t limit) {
    return get_chats(authorization, TdChatList{.kind = list}, limit);
}

std::future<TdValue>
TdClient::get_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization, TdChatList list,
                    std::int32_t limit) {
    return impl_->get_chats(authorization, list, limit);
}

std::future<TdValue>
TdClient::load_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     TdChatListKind list, std::int32_t limit) {
    return load_chats(authorization, TdChatList{.kind = list}, limit);
}

std::future<TdValue>
TdClient::load_chats(const std::shared_ptr<const AuthStateSnapshot>& authorization, TdChatList list,
                     std::int32_t limit) {
    return impl_->load_chats(authorization, list, limit);
}

std::future<TdValue>
TdClient::search_public_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             std::string username) {
    return impl_->search_public_chat(authorization, std::move(username));
}

std::future<TdValue>
TdClient::get_internal_link_type(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                 std::string_view link, TdQueryLifetime lifetime,
                                 const secure::WipeObserver& wipe_observer) {
    return impl_->get_internal_link_type(authorization, link, std::move(lifetime), wipe_observer);
}

std::future<TdValue>
TdClient::get_message_link_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                std::string url) {
    return impl_->get_message_link_info(authorization, std::move(url));
}

std::future<TdValue>
TdClient::check_chat_invite_link(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                 std::string_view link, TdQueryLifetime lifetime,
                                 const secure::WipeObserver& wipe_observer) {
    return impl_->check_chat_invite_link(authorization, link, std::move(lifetime), wipe_observer);
}

std::future<TdValue>
TdClient::get_user(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                   std::int64_t user_id) {
    return impl_->get_user(authorization, user_id);
}

std::future<TdValue>
TdClient::get_basic_group_full_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                    std::int64_t basic_group_id) {
    return impl_->get_basic_group_full_info(authorization, basic_group_id);
}

std::future<TdValue>
TdClient::get_supergroup(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                         std::int64_t supergroup_id) {
    return impl_->get_supergroup(authorization, supergroup_id);
}

std::future<TdValue>
TdClient::get_supergroup_full_info(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   std::int64_t supergroup_id) {
    return impl_->get_supergroup_full_info(authorization, supergroup_id);
}

std::future<TdValue>
TdClient::get_supergroup_members(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                 std::int64_t supergroup_id, std::string query, std::int32_t offset,
                                 std::int32_t limit) {
    return impl_->get_supergroup_members(authorization, supergroup_id, std::move(query), offset,
                                         limit);
}

std::future<TdValue>
TdClient::create_private_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                              std::int64_t user_id, bool force) {
    return impl_->create_private_chat(authorization, user_id, force);
}

std::future<TdValue>
TdClient::get_message(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                      std::int64_t chat_id, std::int64_t message_id) {
    return impl_->get_message(authorization, chat_id, message_id);
}

std::future<TdValue>
TdClient::get_message_properties(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                 std::int64_t chat_id, std::int64_t message_id) {
    return impl_->get_message_properties(authorization, chat_id, message_id);
}

std::future<TdValue> TdClient::get_message_available_reactions(
    const std::shared_ptr<const AuthStateSnapshot>& authorization, std::int64_t chat_id,
    std::int64_t message_id) {
    return impl_->get_message_available_reactions(authorization, chat_id, message_id);
}

std::future<TdValue>
TdClient::get_unix_time(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->get_unix_time(authorization);
}

std::future<TdValue>
TdClient::parse_text_entities(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                              std::string text, TdTextParseMode mode) {
    return impl_->parse_text_entities(authorization, std::move(text), mode);
}

std::future<TdValue>
TdClient::send_message(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                       TdSendMessageRequest request) {
    return impl_->send_message(authorization, std::move(request));
}

TdPreparedWrite
TdClient::prepare_send_message(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                               TdSendMessageRequest request) {
    return impl_->prepare_send_message(authorization, std::move(request));
}

TdPreparedWrite
TdClient::prepare_forward_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                   TdForwardMessagesRequest request) {
    return impl_->prepare_forward_messages(authorization, std::move(request));
}

TdPreparedWrite
TdClient::prepare_direct_mutation(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                  TdDirectRequest request, TdQueryLifetime lifetime) {
    return impl_->prepare_direct_mutation(authorization, std::move(request), std::move(lifetime));
}

std::future<TdValue> TdClient::send(TdPreparedWrite&& prepared) {
    return impl_->send(std::move(prepared));
}

std::future<TdValue>
TdClient::edit_message_text(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                            TdEditMessageTextRequest request) {
    return impl_->edit_message_text(authorization, std::move(request));
}

std::future<TdValue>
TdClient::delete_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                          TdDeleteMessagesRequest request) {
    return impl_->delete_messages(authorization, std::move(request));
}

std::future<TdValue>
TdClient::set_message_reaction(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                               TdMessageReactionRequest request) {
    return impl_->set_message_reaction(authorization, std::move(request));
}

std::future<TdValue>
TdClient::set_message_pinned(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                             TdPinMessageRequest request) {
    return impl_->set_message_pinned(authorization, request);
}

std::future<TdValue>
TdClient::view_messages(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                        TdViewMessagesRequest request) {
    return impl_->view_messages(authorization, std::move(request));
}

std::future<TdValue> TdClient::set_chat_notification_settings(
    const std::shared_ptr<const AuthStateSnapshot>& authorization,
    TdSetChatNotificationSettingsRequest request) {
    return impl_->set_chat_notification_settings(authorization, request);
}

std::future<TdValue>
TdClient::toggle_chat_is_pinned(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                                TdToggleChatIsPinnedRequest request) {
    return impl_->toggle_chat_is_pinned(authorization, request);
}

std::future<TdValue>
TdClient::add_chat_to_list(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                           TdAddChatToListRequest request) {
    return impl_->add_chat_to_list(authorization, request);
}

std::future<TdValue>
TdClient::join_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                    TdJoinChatRequest request) {
    return impl_->join_chat(authorization, std::move(request));
}

std::future<TdValue>
TdClient::leave_chat(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     TdLeaveChatRequest request) {
    return impl_->leave_chat(authorization, request);
}

std::future<TdValue>
TdClient::send_login(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                     const TdRequestOwner& owner, TdAuthRequest request) {
    return impl_->send_login(authorization, owner, std::move(request));
}

std::future<TdValue>
TdClient::send_logout(const std::shared_ptr<const AuthStateSnapshot>& authorization,
                      TdClosedDecision& decision) {
    return impl_->send_logout(authorization, decision);
}

TdClosedDecision TdClient::begin_logout_decision(
    const std::shared_ptr<const AuthStateSnapshot>& authorization,
    std::function<TdLifecycleClaimStatus(std::chrono::steady_clock::time_point)> claim) {
    return impl_->begin_logout_decision(authorization, std::move(claim));
}

bool TdClient::restart_generation(const std::shared_ptr<const AuthStateSnapshot>& authorization) {
    return impl_->restart_generation(authorization);
}

TdRequestOwner TdClient::internal_auth_owner() const {
    return impl_->internal_auth_owner();
}

TdOwnerLease TdClient::issue_login_owner() {
    return impl_->issue_owner(TdOwnerKind::Login);
}

bool TdClient::owns(const TdRequestOwner& owner, std::uint64_t client_generation) const {
    return impl_->owns(owner, client_generation);
}

std::uint64_t TdClient::subscribe_updates(UpdateHandler handler) {
    return impl_->subscribe_updates(std::move(handler));
}

void TdClient::unsubscribe_updates(std::uint64_t id) {
    impl_->unsubscribe_updates(id);
}

std::uint64_t TdClient::subscribe_send_updates(UpdateHandler handler) {
    return impl_->subscribe_send_updates(std::move(handler));
}

void TdClient::unsubscribe_send_updates(std::uint64_t id) {
    impl_->unsubscribe_send_updates(id);
}

std::uint64_t TdClient::subscribe_response_completions(ResponseCompletionHandler handler) {
    return impl_->subscribe_response_completions(std::move(handler));
}

void TdClient::unsubscribe_response_completions(std::uint64_t id) {
    impl_->unsubscribe_response_completions(id);
}

std::shared_ptr<const AuthStateSnapshot> TdClient::auth_state() const {
    return impl_->auth_state();
}

std::uint64_t TdClient::subscribe_auth_states(AuthStateHandler handler) {
    return impl_->subscribe_auth_states(std::move(handler));
}

void TdClient::unsubscribe_auth_states(std::uint64_t id) {
    impl_->unsubscribe_auth_states(id);
}

std::unique_lock<std::mutex> TdClient::lock_event_publication() const {
    return std::unique_lock(impl_->event_publication_mutex());
}

void TdClient::close() {
    impl_->close();
}

bool TdClient::close_until(std::chrono::steady_clock::time_point deadline) {
    return impl_->close_until(deadline);
}

std::string TdClient::tdlib_version() {
    return production_tdlib_version();
}

} // namespace tgcli::core
