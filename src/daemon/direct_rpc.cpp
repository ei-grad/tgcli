#include "daemon/direct_rpc.hpp"

#include "common/utf8.hpp"
#include "daemon/request_session.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;

bool valid_sender(const core::TdMessageSender& sender) {
    switch (sender.kind) {
    case core::TdMessageSenderKind::User:
        return core::valid_td_message_id(sender.id);
    case core::TdMessageSenderKind::Chat:
        return core::valid_td_chat_id(sender.id);
    case core::TdMessageSenderKind::Unknown:
        return false;
    }
    return false;
}

bool valid_topic(const std::optional<core::TdTopic>& topic) {
    if (!topic) {
        return true;
    }
    if (!core::valid_td_message_id(topic->id)) {
        return false;
    }
    switch (topic->kind) {
    case core::TdTopicKind::Forum:
        return topic->id <= std::numeric_limits<std::int32_t>::max();
    case core::TdTopicKind::Thread:
    case core::TdTopicKind::Direct:
    case core::TdTopicKind::Saved:
        return true;
    case core::TdTopicKind::Unknown:
        return false;
    }
    return false;
}

bool valid_write_message(const core::TdMessageWriteResult& message) {
    const auto valid_text = [&] {
        if (message.text.size() > 16'384 || !common::valid_utf8(message.text)) {
            return false;
        }
        std::size_t scalar_count = 0;
        for (const auto byte : message.text) {
            if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U && ++scalar_count > 4'096) {
                return false;
            }
        }
        return true;
    };
    return core::valid_td_nonzero_int53(message.id) && core::valid_td_chat_id(message.chat_id) &&
           message.scheduled == !message.date.has_value() &&
           (!message.date || *message.date >= 0) && valid_sender(message.sender) &&
           valid_topic(message.topic) && valid_text();
}

std::future<core::TdValue>
start_request(core::TdClient& client, const core::TdDirectRequest& request,
              const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
    return std::visit(
        [&](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, core::TdEditMessageTextRequest>) {
                return client.edit_message_text(authorization, value);
            } else if constexpr (std::is_same_v<Request, core::TdDeleteMessagesRequest>) {
                return client.delete_messages(authorization, value);
            } else if constexpr (std::is_same_v<Request, core::TdMessageReactionRequest>) {
                return client.set_message_reaction(authorization, value);
            } else if constexpr (std::is_same_v<Request, core::TdPinMessageRequest>) {
                return client.set_message_pinned(authorization, value);
            } else if constexpr (std::is_same_v<Request, core::TdViewMessagesRequest>) {
                return client.view_messages(authorization, value);
            } else if constexpr (std::is_same_v<Request,
                                                core::TdSetChatNotificationSettingsRequest>) {
                return client.set_chat_notification_settings(authorization, value);
            } else if constexpr (std::is_same_v<Request, core::TdToggleChatIsPinnedRequest>) {
                return client.toggle_chat_is_pinned(authorization, value);
            } else if constexpr (std::is_same_v<Request, core::TdAddChatToListRequest>) {
                return client.add_chat_to_list(authorization, value);
            } else if constexpr (std::is_same_v<Request, core::TdJoinChatRequest>) {
                return client.join_chat(authorization, value);
            } else {
                static_assert(std::is_same_v<Request, core::TdLeaveChatRequest>);
                return client.leave_chat(authorization, value);
            }
        },
        request);
}

bool is_ok_response(const core::TdValue& value) {
    return value.get_if<core::TdOk>() != nullptr;
}

DirectOutcome direct_result_from([[maybe_unused]] const core::TdEditMessageTextRequest& input,
                                 const core::TdValue& value) {
    const auto* message = value.get_if<core::TdMessageWriteResult>();
    if (message == nullptr || !valid_write_message(*message)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = *message};
}

DirectOutcome direct_result_from(const core::TdDeleteMessagesRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = DirectDeleteResult{.chat_id = input.chat_id,
                                                      .message_ids = input.message_ids,
                                                      .for_all = input.revoke}};
}

DirectOutcome direct_result_from(const core::TdMessageReactionRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = DirectReactionResult{.chat_id = input.chat_id,
                                                        .message_id = input.message_id,
                                                        .reaction = input.reaction,
                                                        .removed = input.remove,
                                                        .big = input.big}};
}

DirectOutcome direct_result_from(const core::TdPinMessageRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = DirectMessagePinResult{.chat_id = input.chat_id,
                                                          .message_id = input.message_id,
                                                          .pinned = input.pinned}};
}

DirectOutcome direct_result_from(const core::TdViewMessagesRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{
        .result = DirectMarkReadResult{.chat_id = input.chat_id,
                                       .last_read_message_id = input.message_ids.back()}};
}

DirectOutcome direct_result_from(const core::TdSetChatNotificationSettingsRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = DirectMuteResult{.chat_id = input.chat_id,
                                                    .muted = input.settings.mute_for != 0,
                                                    .duration_seconds = input.settings.mute_for}};
}

DirectOutcome direct_result_from(const core::TdToggleChatIsPinnedRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = DirectChatPinResult{.chat_id = input.chat_id,
                                                       .chat_list = input.list,
                                                       .pinned = input.pinned}};
}

DirectOutcome direct_result_from(const core::TdAddChatToListRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{
        .result = DirectArchiveResult{.chat_id = input.chat_id,
                                      .archived = input.list == core::TdDirectChatList::Archive}};
}

DirectOutcome direct_result_from(const core::TdJoinChatRequest& input, const core::TdValue& value) {
    const auto* joined = value.get_if<core::TdChatJoinResult>();
    if (joined == nullptr) {
        return DirectMalformed{};
    }
    switch (joined->kind) {
    case core::TdChatJoinResultKind::Success:
        if (!joined->chat_id || !core::valid_td_chat_id(joined->chat_id.value_or(0))) {
            return DirectMalformed{};
        }
        return DirectSuccess{.result = DirectJoinResult{.status = DirectJoinStatus::Joined,
                                                        .chat_id = joined->chat_id}};
    case core::TdChatJoinResultKind::RequestSent:
        return DirectSuccess{.result = DirectJoinResult{.status = DirectJoinStatus::RequestSent,
                                                        .chat_id = input.chat_id}};
    case core::TdChatJoinResultKind::GuardBotApprovalRequired:
        if (!joined->guard_bot_user_id || !joined->guard_query_id ||
            !core::valid_td_message_id(joined->guard_bot_user_id.value_or(0)) ||
            !core::valid_td_message_id(joined->guard_query_id.value_or(0))) {
            return DirectMalformed{};
        }
        return DirectJoinGuardRequired{.bot_user_id = joined->guard_bot_user_id.value_or(0),
                                       .query_id = joined->guard_query_id.value_or(0)};
    case core::TdChatJoinResultKind::Declined:
        return DirectJoinDeclined{};
    case core::TdChatJoinResultKind::Unknown:
        return DirectMalformed{.tdlib_type_id = joined->unsupported_tdlib_type_id};
    }
    return DirectMalformed{};
}

DirectOutcome direct_result_from(const core::TdLeaveChatRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = DirectLeaveResult{.chat_id = input.chat_id}};
}

DirectOutcome success_from(const core::TdDirectRequest& request, const core::TdValue& value) {
    if (const auto* error = value.get_if<core::TdError>()) {
        return DirectTdError{.error = *error};
    }
    if (const auto* malformed = value.get_if<core::TdDirectConversionError>()) {
        return DirectMalformed{.tdlib_type_id = malformed->tdlib_type_id};
    }
    return std::visit([&](const auto& input) { return direct_result_from(input, value); }, request);
}

} // namespace

class DirectRpcCoordinator::Impl {
  public:
    Impl(core::TdClient& client, RequestSession& session, DirectRpcHooks hooks)
        : client_(client), session_(session),
          now_(hooks.now ? std::move(hooks.now) : [] { return core::TdEventClock::now(); }),
          wait_(std::move(hooks.wait)), before_request_(std::move(hooks.before_request)),
          before_event_arbitration_(std::move(hooks.before_event_arbitration)),
          before_wait_(std::move(hooks.before_wait)) {
        response_subscription_ =
            client_.subscribe_response_completions([this](std::uint64_t) { wake(); });
        auth_subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
                record(snapshot);
            });
        record(client_.auth_state());
    }

    ~Impl() {
        client_.unsubscribe_response_completions(response_subscription_);
        client_.unsubscribe_auth_states(auth_subscription_);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    DirectOutcome execute(const core::TdDirectRequest& request,
                          const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
        if (executed_ || !authorization || authorization->data.state != core::AuthState::Ready ||
            !core::valid_td_direct_request(request)) {
            return DirectRejected{};
        }
        executed_ = true;
        if (deadline_expired(session_.deadline(), now_())) {
            return DirectTimedOut{.mutation_state = DirectMutationState::None};
        }
        try {
            if (before_request_) {
                before_request_();
            }
        } catch (const std::exception&) {
            return DirectRejected{};
        }
        if (!session_.reserve_direct_in_flight()) {
            return DirectCancelled{.mutation_state = DirectMutationState::None};
        }
        std::future<core::TdValue> response;
        try {
            response = start_request(client_, request, authorization);
        } catch (const std::exception&) {
            session_.settle_in_flight();
            return DirectRejected{};
        }
        auto outcome = wait_for_response(request, response, *authorization);
        session_.settle_in_flight();
        return outcome;
    }

  private:
    enum class AuthCompetitionKind { None, Lost };

    struct AuthCompetition {
        AuthCompetitionKind kind = AuthCompetitionKind::None;
        std::shared_ptr<const core::AuthStateSnapshot> snapshot;
    };

    AuthCompetition first_auth_competitor(const core::AuthStateSnapshot& sent) {
        record(client_.auth_state());
        const std::lock_guard lock(auth_mutex_);
        for (const auto& candidate : auth_events_) {
            if (!candidate || std::tie(candidate->client_generation, candidate->auth_sequence) <=
                                  std::tie(sent.client_generation, sent.auth_sequence)) {
                continue;
            }
            if (candidate->receive_event_sequence == 0 || !candidate->receive_observed_at) {
                continue;
            }
            if (!event_precedes_deadline(candidate->receive_observed_at, session_.deadline())) {
                continue;
            }
            if (candidate->data.state != core::AuthState::Ready) {
                return {AuthCompetitionKind::Lost, candidate};
            }
        }
        return {};
    }

    DirectOutcome consume_response(const core::TdDirectRequest& request,
                                   std::future<core::TdValue>& response,
                                   const core::AuthStateSnapshot& sent) {
        try {
            auto value = response.get();
            const auto auth = first_auth_competitor(sent);
            const auto response_sequence = value.receive_event_sequence();
            const auto observed_at = value.receive_observed_at();
            if (response_sequence == 0 || !observed_at) {
                return DirectMalformed{};
            }
            const bool response_eligible =
                event_precedes_deadline(observed_at, session_.deadline());
            if (auth.kind == AuthCompetitionKind::Lost && auth.snapshot &&
                (!response_eligible || auth.snapshot->receive_event_sequence < response_sequence)) {
                return DirectAuthorizationLost{.snapshot = auth.snapshot};
            }
            if (!response_eligible) {
                return DirectTimedOut{};
            }
            return success_from(request, value);
        } catch (const core::TdAuthorizationError& error) {
            const auto auth = first_auth_competitor(sent);
            if (auth.kind == AuthCompetitionKind::Lost) {
                return DirectAuthorizationLost{.snapshot = auth.snapshot,
                                               .mutation_state = DirectMutationState::None};
            }
            return DirectRejected{.authorization_failure = error.failure()};
        } catch (const std::exception&) {
            const auto auth = first_auth_competitor(sent);
            if (auth.kind == AuthCompetitionKind::Lost) {
                return DirectAuthorizationLost{.snapshot = auth.snapshot};
            }
            return DirectRejected{};
        }
    }

    DirectOutcome wait_for_response(const core::TdDirectRequest& request,
                                    std::future<core::TdValue>& response,
                                    const core::AuthStateSnapshot& sent) {
        for (;;) {
            std::uint64_t wake_sequence = 0;
            {
                const std::lock_guard lock(wait_mutex_);
                wake_sequence = wake_sequence_;
            }
            {
                if (before_event_arbitration_) {
                    before_event_arbitration_();
                }
                const auto publication_lock = client_.lock_event_publication();
                if (response.wait_for(0ms) == std::future_status::ready) {
                    return consume_response(request, response, sent);
                }
                const auto auth = first_auth_competitor(sent);
                if (auth.kind == AuthCompetitionKind::Lost) {
                    return DirectAuthorizationLost{.snapshot = auth.snapshot};
                }
                if (deadline_expired(session_.deadline(), now_())) {
                    return DirectTimedOut{};
                }
            }
            if (session_.cancellation_requested()) {
                return DirectCancelled{};
            }
            if (before_wait_) {
                before_wait_();
            }
            if (wait_) {
                wait_(session_.deadline(), session_.cancellation_token());
                continue;
            }
            std::unique_lock lock(wait_mutex_);
            const auto changed = [this, wake_sequence] { return wake_sequence_ != wake_sequence; };
            if (const auto expires_at = session_.deadline().expires_at) {
                static_cast<void>(wait_condition_.wait_until(lock, session_.cancellation_token(),
                                                             *expires_at, changed));
            } else {
                static_cast<void>(
                    wait_condition_.wait(lock, session_.cancellation_token(), changed));
            }
        }
    }

    void record(const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
        bool recorded = false;
        {
            const std::lock_guard lock(auth_mutex_);
            if (snapshot &&
                (!latest_auth_ ||
                 std::tie(snapshot->client_generation, snapshot->auth_sequence) >
                     std::tie(latest_auth_->client_generation, latest_auth_->auth_sequence))) {
                latest_auth_ = snapshot;
                auth_events_.push_back(snapshot);
                recorded = true;
            }
        }
        if (recorded) {
            wake();
        }
    }

    void wake() {
        const std::lock_guard lock(wait_mutex_);
        ++wake_sequence_;
        wait_condition_.notify_all();
    }

    core::TdClient& client_;
    RequestSession& session_;
    std::function<core::TdEventClock::time_point()> now_;
    std::function<void(const RequestDeadline&, const std::stop_token&)> wait_;
    std::function<void()> before_request_;
    std::function<void()> before_event_arbitration_;
    std::function<void()> before_wait_;
    bool executed_ = false;
    std::mutex wait_mutex_;
    std::condition_variable_any wait_condition_;
    std::uint64_t wake_sequence_ = 0;
    std::mutex auth_mutex_;
    std::shared_ptr<const core::AuthStateSnapshot> latest_auth_;
    std::vector<std::shared_ptr<const core::AuthStateSnapshot>> auth_events_;
    std::uint64_t response_subscription_ = 0;
    std::uint64_t auth_subscription_ = 0;
};

DirectRpcCoordinator::DirectRpcCoordinator(core::TdClient& client, RequestSession& session,
                                           DirectRpcHooks hooks)
    : impl_(std::make_unique<Impl>(client, session, std::move(hooks))) {}

DirectRpcCoordinator::~DirectRpcCoordinator() = default;

DirectOutcome
DirectRpcCoordinator::execute(const core::TdDirectRequest& request,
                              const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
    return impl_->execute(request, authorization);
}

} // namespace tgcli::daemon
