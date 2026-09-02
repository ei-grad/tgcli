#include "daemon/direct_rpc.hpp"

#include "common/secure_wipe.hpp"
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

DirectTdError::DirectTdError(core::TdError& source, secure::WipeObserver wipe_observer)
    : error{.code = source.code, .message = {}}, wipe_observer_(std::move(wipe_observer)) {
    const secure::StringWiper source_wiper(source.message, wipe_observer_,
                                           "direct_td_error_source");
    error.message.assign(source.message);
}

DirectTdError::~DirectTdError() {
    secure::wipe(error.message, wipe_observer_, "direct_td_error");
}

// NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
DirectTdError::DirectTdError(DirectTdError&& other)
    : error{.code = other.error.code, .message = {}}, mutation_state(other.mutation_state),
      wipe_observer_(std::move(other.wipe_observer_)) {
    const secure::StringWiper source_wiper(other.error.message, wipe_observer_,
                                           "direct_td_error_move_source");
    error.message.assign(other.error.message);
}

// NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
DirectTdError& DirectTdError::operator=(DirectTdError&& other) {
    if (this != &other) {
        secure::wipe(error.message, wipe_observer_, "direct_td_error");
        error.code = other.error.code;
        mutation_state = other.mutation_state;
        wipe_observer_ = std::move(other.wipe_observer_);
        const secure::StringWiper source_wiper(other.error.message, wipe_observer_,
                                               "direct_td_error_move_source");
        error.message.assign(other.error.message);
    }
    return *this;
}

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

enum class WriteMessageValidity { Valid, Oversized, Malformed };

WriteMessageValidity write_message_validity(const core::TdMessageWriteResult& message) {
    if (!core::valid_td_nonzero_int53(message.id) || !core::valid_td_chat_id(message.chat_id) ||
        message.scheduled != !message.date.has_value() || (message.date && *message.date < 0) ||
        !valid_sender(message.sender) || !valid_topic(message.topic) ||
        !common::valid_utf8(message.text)) {
        return WriteMessageValidity::Malformed;
    }
    std::size_t scalar_count = 0;
    for (const auto byte : message.text) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
            ++scalar_count;
        }
    }
    return message.text.size() > 16'384 || scalar_count > 4'096 ? WriteMessageValidity::Oversized
                                                                : WriteMessageValidity::Valid;
}

bool is_ok_response(const core::TdValue& value) {
    return value.get_if<core::TdOk>() != nullptr;
}

DirectOutcome direct_result_from(const core::TdEditMessageTextRequest& input,
                                 const core::TdValue& value) {
    const auto* message = value.get_if<core::TdMessageWriteResult>();
    if (message == nullptr) {
        return DirectMalformed{};
    }
    const auto validity = write_message_validity(*message);
    if (validity == WriteMessageValidity::Oversized) {
        return message->chat_id == input.chat_id && message->id == input.message_id
                   ? DirectOutcome{DirectOversizedMessage{}}
                   : DirectOutcome{DirectMalformed{}};
    }
    if (validity == WriteMessageValidity::Malformed) {
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
        return DirectSuccess{
            .result = DirectJoinResult{.status = DirectJoinStatus::RequestSent,
                                       .chat_id = input.chat_id ? input.chat_id
                                                                : input.expected_invite_chat_id}};
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

DirectOutcome direct_result_from(const core::TdTerminateSessionRequest& input,
                                 const core::TdValue& value) {
    if (!is_ok_response(value)) {
        return DirectMalformed{};
    }
    return DirectSuccess{.result = DirectTerminateSessionResult{input.session_id}};
}

DirectOutcome direct_result_from(const core::TdM6Request& /*input*/, const core::TdValue& value) {
    const auto* response = value.get_if<core::TdM6Response>();
    if (response == nullptr) {
        return DirectMalformed{};
    }
    if (const auto* malformed = std::get_if<core::TdM6ConversionError>(response)) {
        return DirectMalformed{.tdlib_type_id = malformed->tdlib_type_id};
    }
    return DirectSuccess{.result = *response};
}

DirectOutcome success_from(const core::TdDirectRequest& request, core::TdValue& value) {
    if (auto* error = value.get_if<core::TdError>()) {
        secure::WipeObserver wipe_observer;
        if (const auto* join = std::get_if<core::TdJoinChatRequest>(&request);
            join != nullptr && join->is_invite_request()) {
            wipe_observer = join->wipe_observer();
        }
        return DirectTdError(*error, std::move(wipe_observer));
    }
    if (const auto* malformed = value.get_if<core::TdDirectConversionError>()) {
        return DirectMalformed{.tdlib_type_id = malformed->tdlib_type_id};
    }
    return std::visit([&](const auto& input) { return direct_result_from(input, value); }, request);
}

void wipe_request_secrets(core::TdDirectRequest& request) {
    if (auto* join = std::get_if<core::TdJoinChatRequest>(&request);
        join != nullptr && join->has_invite_link()) {
        join->clear_invite_link();
    }
    if (auto* m6 = std::get_if<core::TdM6Request>(&request)) {
        if (auto* revoke = std::get_if<core::TdM6RevokeChatInviteLinkRequest>(m6)) {
            secure::wipe(revoke->invite_link);
        }
    }
}

} // namespace

class DirectRpcCoordinator::Impl {
  public:
    Impl(core::TdClient& client, RequestSession& session, DirectRpcHooks hooks)
        : client_(client), session_(session),
          now_(hooks.now ? std::move(hooks.now) : [] { return core::TdEventClock::now(); }),
          wait_(std::move(hooks.wait)), before_request_(std::move(hooks.before_request)),
          before_submit_(std::move(hooks.before_submit)),
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
        if (request_) {
            wipe_request_secrets(*request_);
        }
        settle_in_flight();
        client_.unsubscribe_response_completions(response_subscription_);
        client_.unsubscribe_auth_states(auth_subscription_);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    DirectOutcome execute(const core::TdDirectRequest& request,
                          const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
                          core::TdQueryLifetime lifetime) {
        auto preparation =
            prepare(core::TdDirectRequest{request}, authorization, std::move(lifetime));
        return std::visit(
            [this](auto&& outcome) -> DirectOutcome {
                using Outcome = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<Outcome, DirectPrepared>) {
                    return execute_prepared();
                } else {
                    return std::forward<decltype(outcome)>(outcome);
                }
            },
            std::move(preparation));
    }

    DirectPreparationOutcome
    prepare(core::TdDirectRequest request,
            const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
            core::TdQueryLifetime lifetime) {
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
        if (deadline_expired(session_.deadline(), now_())) {
            return DirectTimedOut{.mutation_state = DirectMutationState::None};
        }
        if (session_.cancellation_requested()) {
            return DirectCancelled{.mutation_state = DirectMutationState::None};
        }
        if (!session_.reserve_direct_in_flight()) {
            return DirectCancelled{.mutation_state = DirectMutationState::None};
        }
        in_flight_ = true;
        request_ = request;
        if (request_) {
            wipe_request_secrets(*request_);
        }
        authorization_ = authorization;
        prepared_write_ =
            client_.prepare_direct_mutation(authorization, std::move(request), std::move(lifetime));
        wipe_request_secrets(request);
        if (!prepared_write_) {
            const auto failure = prepared_write_.authorization_failure();
            const auto auth = first_auth_competitor(*authorization);
            settle_in_flight();
            if (auth.kind == AuthCompetitionKind::Lost) {
                return DirectAuthorizationLost{.snapshot = auth.snapshot,
                                               .mutation_state = DirectMutationState::None};
            }
            return DirectRejected{.authorization_failure = failure,
                                  .mutation_state = DirectMutationState::None};
        }
        if (deadline_expired(session_.deadline(), now_())) {
            prepared_write_ = {};
            settle_in_flight();
            return DirectTimedOut{.mutation_state = DirectMutationState::None};
        }
        if (session_.cancellation_requested()) {
            prepared_write_ = {};
            settle_in_flight();
            return DirectCancelled{.mutation_state = DirectMutationState::None};
        }
        prepared_ = true;
        return DirectPrepared{};
    }

    DirectOutcome execute_prepared() {
        if (!prepared_ || !prepared_write_ || !request_ || !authorization_) {
            return DirectRejected{.authorization_failure = std::nullopt,
                                  .mutation_state = DirectMutationState::Possible};
        }
        prepared_ = false;
        try {
            if (before_submit_) {
                before_submit_();
            }
        } catch (const std::exception&) {
            prepared_write_ = {};
            return finish(DirectRejected{.authorization_failure = std::nullopt,
                                         .mutation_state = DirectMutationState::Possible});
        }
        if (deadline_expired(session_.deadline(), now_())) {
            prepared_write_ = {};
            return finish(DirectTimedOut{.mutation_state = DirectMutationState::Possible});
        }
        if (session_.cancellation_requested()) {
            prepared_write_ = {};
            return finish(DirectCancelled{.mutation_state = DirectMutationState::Possible});
        }
        std::future<core::TdValue> response;
        try {
            response = client_.send(std::move(prepared_write_));
        } catch (const std::exception&) {
            return finish(DirectRejected{.authorization_failure = std::nullopt,
                                         .mutation_state = DirectMutationState::Possible});
        }
        return finish(wait_for_response(*request_, response, *authorization_));
    }

  private:
    void settle_in_flight() {
        if (in_flight_) {
            session_.settle_in_flight();
            in_flight_ = false;
        }
    }

    DirectOutcome finish(DirectOutcome outcome) {
        settle_in_flight();
        return outcome;
    }

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
                static_cast<void>(cancellation::wait_until(
                    wait_condition_, lock, session_.cancellation_token(), *expires_at, changed));
            } else {
                static_cast<void>(cancellation::wait(wait_condition_, lock,
                                                     session_.cancellation_token(), changed));
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
    std::function<void(const RequestDeadline&, const cancellation::Token&)> wait_;
    std::function<void()> before_request_;
    std::function<void()> before_submit_;
    std::function<void()> before_event_arbitration_;
    std::function<void()> before_wait_;
    bool executed_ = false;
    bool prepared_ = false;
    bool in_flight_ = false;
    core::TdPreparedWrite prepared_write_;
    std::optional<core::TdDirectRequest> request_;
    std::shared_ptr<const core::AuthStateSnapshot> authorization_;
    std::mutex wait_mutex_;
    cancellation::Condition wait_condition_;
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
                              const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
                              core::TdQueryLifetime lifetime) {
    return impl_->execute(request, authorization, std::move(lifetime));
}

DirectPreparationOutcome
DirectRpcCoordinator::prepare(core::TdDirectRequest request,
                              const std::shared_ptr<const core::AuthStateSnapshot>& authorization,
                              core::TdQueryLifetime lifetime) {
    return impl_->prepare(std::move(request), authorization, std::move(lifetime));
}

DirectOutcome DirectRpcCoordinator::execute_prepared() {
    return impl_->execute_prepared();
}

} // namespace tgcli::daemon
