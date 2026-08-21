#include "daemon/single_send.hpp"

#include "common/utf8.hpp"
#include "daemon/rate_limit.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;

using SendUpdate = std::variant<core::TdUpdateMessageSendSucceeded, core::TdUpdateMessageSendFailed,
                                core::TdUpdateDeleteMessages>;

struct StampedUpdate {
    std::uint64_t sequence = 0;
    std::optional<core::TdEventClock::time_point> observed_at;
    SendUpdate update;
};

struct StampedOutcome {
    std::uint64_t sequence = 0;
    SingleSendOutcome outcome;
};

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

bool valid_message_text(std::string_view text) {
    if (text.size() > 16'384 || !common::valid_utf8(text)) {
        return false;
    }
    std::size_t scalar_count = 0;
    for (const auto byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U && ++scalar_count > 4'096) {
            return false;
        }
    }
    return true;
}

bool valid_message_summary(const core::TdMessageSummary& message, std::int64_t chat_id,
                           [[maybe_unused]] bool stable) {
    return core::valid_td_nonzero_int53(message.id) && message.chat_id == chat_id &&
           message.date >= 0 && valid_sender(message.sender) && valid_topic(message.topic) &&
           valid_message_text(message.text);
}

bool valid_sending_state_defaults(const core::TdMessageSendingState& state) {
    return !state.error && !state.can_retry && !state.need_another_sender &&
           !state.need_another_reply_quote && !state.need_drop_reply &&
           state.required_paid_message_star_count == 0 && state.retry_after == 0 &&
           !state.unsupported_tdlib_type_id;
}

bool valid_schedule(const core::TdMessageSchedulingState& schedule) {
    switch (schedule.kind) {
    case core::TdMessageSchedulingStateKind::None:
        return schedule.send_date == 0 && schedule.repeat_period == 0 &&
               !schedule.unsupported_tdlib_type_id;
    case core::TdMessageSchedulingStateKind::SendAtDate:
        return schedule.send_date > 0 && schedule.repeat_period == 0 &&
               !schedule.unsupported_tdlib_type_id;
    case core::TdMessageSchedulingStateKind::SendWhenOnline:
        return schedule.send_date == 0 && schedule.repeat_period == 0 &&
               !schedule.unsupported_tdlib_type_id;
    case core::TdMessageSchedulingStateKind::SendWhenVideoProcessed:
    case core::TdMessageSchedulingStateKind::Unknown:
        return false;
    }
    return false;
}

bool valid_pending_message(const core::TdWriteMessage& message, std::int64_t chat_id,
                           std::int32_t sending_id) {
    return valid_message_summary(message.message, chat_id, false) &&
           message.sending_state.kind == core::TdMessageSendingStateKind::Pending &&
           message.sending_state.sending_id == sending_id && sending_id != 0 &&
           valid_sending_state_defaults(message.sending_state) &&
           valid_schedule(message.scheduling_state);
}

bool valid_failed_message(const core::TdWriteMessage& message, std::int64_t chat_id) {
    const auto& state = message.sending_state;
    return valid_message_summary(message.message, chat_id, false) &&
           state.kind == core::TdMessageSendingStateKind::Failed && state.sending_id == 0 &&
           state.error.has_value() && !state.unsupported_tdlib_type_id &&
           state.required_paid_message_star_count >= 0 && std::isfinite(state.retry_after) &&
           state.retry_after >= 0 && valid_schedule(message.scheduling_state);
}

std::optional<SingleSendSucceeded>
success_from(const core::TdWriteMessage& message,
             const std::optional<SingleSendTemporaryId>& temporary, std::int64_t chat_id) {
    if (!valid_message_summary(message.message, chat_id, true) ||
        message.sending_state.kind != core::TdMessageSendingStateKind::Stable ||
        message.sending_state.sending_id != 0 ||
        !valid_sending_state_defaults(message.sending_state) ||
        !valid_schedule(message.scheduling_state)) {
        return std::nullopt;
    }
    const bool scheduled =
        message.scheduling_state.kind != core::TdMessageSchedulingStateKind::None;
    const auto& summary = message.message;
    return SingleSendSucceeded{
        .temporary = temporary,
        .authoritative_message = message,
        .result = {.id = summary.id,
                   .chat_id = summary.chat_id,
                   .date = scheduled ? std::nullopt : std::optional<std::int32_t>{summary.date},
                   .sender = summary.sender,
                   .is_outgoing = summary.is_outgoing,
                   .topic = summary.topic,
                   .content_kind = summary.content_kind,
                   .text = summary.text,
                   .scheduled = scheduled}};
}

std::int32_t retry_after(const core::TdError& error, std::optional<double> precise) {
    if (precise && std::isfinite(*precise) && *precise > 0 &&
        *precise <= std::numeric_limits<std::int32_t>::max()) {
        return static_cast<std::int32_t>(std::ceil(*precise));
    }
    return parse_retry_after_seconds(error.message);
}

SingleSendOutcome failure_from(core::TdError error, std::optional<SingleSendTemporaryId> temporary,
                               SingleSendMutationState mutation_state,
                               std::optional<double> precise_retry_after = std::nullopt) {
    if (error.code == 429) {
        const auto delay = retry_after(error, precise_retry_after);
        return SingleSendRateLimited{.temporary = temporary,
                                     .error = std::move(error),
                                     .retry_after = delay,
                                     .mutation_state = mutation_state};
    }
    return SingleSendFailed{
        .temporary = temporary, .error = std::move(error), .mutation_state = mutation_state};
}

std::optional<std::int32_t> unsupported_type_id(const core::TdWriteMessage& message) {
    if (message.sending_state.unsupported_tdlib_type_id) {
        return message.sending_state.unsupported_tdlib_type_id;
    }
    return message.scheduling_state.unsupported_tdlib_type_id;
}

std::optional<SingleSendOutcome>
outcome_from_update(const core::TdUpdateMessageSendSucceeded& update,
                    const SingleSendTemporaryId& temporary) {
    if (update.client_generation != temporary.client_generation ||
        update.old_message_id != temporary.temporary_message_id) {
        return std::nullopt;
    }
    if (!update.message) {
        return SingleSendMalformed{.temporary = temporary, .tdlib_type_id = std::nullopt};
    }
    auto success = success_from(*update.message, temporary, temporary.chat_id);
    if (!success) {
        return SingleSendMalformed{.temporary = temporary,
                                   .tdlib_type_id = unsupported_type_id(*update.message)};
    }
    return SingleSendOutcome{std::move(*success)};
}

std::optional<SingleSendOutcome> outcome_from_update(const core::TdUpdateMessageSendFailed& update,
                                                     const SingleSendTemporaryId& temporary) {
    if (update.client_generation != temporary.client_generation ||
        update.old_message_id != temporary.temporary_message_id) {
        return std::nullopt;
    }
    if (!update.message || !update.error ||
        !valid_failed_message(*update.message, temporary.chat_id) ||
        update.message->sending_state.error != update.error) {
        return SingleSendMalformed{
            .temporary = temporary,
            .tdlib_type_id = update.message ? unsupported_type_id(*update.message) : std::nullopt};
    }
    return failure_from(*update.error, temporary, SingleSendMutationState::None,
                        update.message->sending_state.retry_after);
}

std::optional<SingleSendOutcome> outcome_from_update(const core::TdUpdateDeleteMessages& update,
                                                     const SingleSendTemporaryId& temporary) {
    if (update.client_generation != temporary.client_generation ||
        update.chat_id != temporary.chat_id ||
        std::ranges::find(update.message_ids, temporary.temporary_message_id) ==
            update.message_ids.end()) {
        return std::nullopt;
    }
    const bool valid_ids = std::ranges::all_of(update.message_ids, [](auto id) {
        return id != 0 && id >= -core::kTdInt53Max && id <= core::kTdInt53Max;
    });
    if (!core::valid_td_chat_id(update.chat_id) || !valid_ids) {
        return SingleSendMalformed{.temporary = temporary, .tdlib_type_id = std::nullopt};
    }
    return SingleSendDeletedBeforeConfirmation{.temporary = temporary};
}

} // namespace

class SingleSendCoordinator::Impl {
  public:
    Impl(core::TdClient& client, RequestSession& session, SingleSendHooks hooks)
        : client_(client), session_(session),
          now_(hooks.now ? std::move(hooks.now) : [] { return core::TdEventClock::now(); }),
          wait_(std::move(hooks.wait)), before_request_(std::move(hooks.before_request)),
          before_event_arbitration_(std::move(hooks.before_event_arbitration)),
          before_wait_(std::move(hooks.before_wait)),
          on_temporary_id_(std::move(hooks.on_temporary_id)) {
        update_subscription_ = client_.subscribe_send_updates(
            [this](const core::TdValue& value) { record_update(value); });
        response_subscription_ =
            client_.subscribe_response_completions([this](std::uint64_t) { wake(); });
        auth_subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
                record_auth(snapshot);
            });
        record_auth(client_.auth_state());
    }

    ~Impl() {
        client_.unsubscribe_send_updates(update_subscription_);
        client_.unsubscribe_response_completions(response_subscription_);
        client_.unsubscribe_auth_states(auth_subscription_);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    SingleSendOutcome execute(core::TdSendMessageRequest request,
                              const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
        if (executed_ || !authorization || authorization->data.state != core::AuthState::Ready ||
            !core::valid_td_send_message_request(request) ||
            (request.content.parsed && !request.content.formatted_text.capability.valid_for(
                                           authorization->client_generation))) {
            return SingleSendRejected{};
        }
        executed_ = true;
        if (deadline_expired(session_.deadline(), now_())) {
            return SingleSendTimedOut{.temporary = std::nullopt,
                                      .mutation_state = SingleSendMutationState::None};
        }
        try {
            if (before_request_) {
                before_request_();
            }
        } catch (const std::exception&) {
            return SingleSendRejected{};
        }
        if (!session_.reserve_direct_in_flight()) {
            return SingleSendCancelled{.temporary = std::nullopt,
                                       .mutation_state = SingleSendMutationState::None};
        }
        const auto chat_id = request.chat_id;
        const auto sending_id = request.options.sending_id;
        std::future<core::TdValue> response;
        try {
            response = client_.send_message(authorization, std::move(request));
        } catch (const std::exception&) {
            session_.settle_in_flight();
            return SingleSendRejected{};
        }
        auto outcome = wait_for_terminal(response, *authorization, chat_id, sending_id);
        session_.settle_in_flight();
        return outcome;
    }

  private:
    void record_update(const core::TdValue& value) {
        std::optional<SendUpdate> update;
        if (const auto* succeeded = value.get_if<core::TdUpdateMessageSendSucceeded>()) {
            update = *succeeded;
        } else if (const auto* failed = value.get_if<core::TdUpdateMessageSendFailed>()) {
            update = *failed;
        } else if (const auto* deleted = value.get_if<core::TdUpdateDeleteMessages>()) {
            update = *deleted;
        }
        if (!update) {
            return;
        }
        {
            const std::lock_guard lock(event_mutex_);
            updates_.push_back({.sequence = value.receive_event_sequence(),
                                .observed_at = value.receive_observed_at(),
                                .update = std::move(*update)});
            ++wake_sequence_;
        }
        event_condition_.notify_all();
    }

    void record_auth(const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
        bool recorded = false;
        {
            const std::lock_guard lock(event_mutex_);
            if (snapshot &&
                (!latest_auth_ ||
                 std::tie(snapshot->client_generation, snapshot->auth_sequence) >
                     std::tie(latest_auth_->client_generation, latest_auth_->auth_sequence))) {
                latest_auth_ = snapshot;
                auth_events_.push_back(snapshot);
                ++wake_sequence_;
                recorded = true;
            }
        }
        if (recorded) {
            event_condition_.notify_all();
        }
    }

    void wake() {
        {
            const std::lock_guard lock(event_mutex_);
            ++wake_sequence_;
        }
        event_condition_.notify_all();
    }

    std::optional<StampedOutcome> auth_terminal(const core::AuthStateSnapshot& sent) {
        const std::lock_guard lock(event_mutex_);
        std::optional<StampedOutcome> result;
        for (const auto& snapshot : auth_events_) {
            if (!snapshot ||
                std::tie(snapshot->client_generation, snapshot->auth_sequence) <=
                    std::tie(sent.client_generation, sent.auth_sequence) ||
                snapshot->receive_event_sequence == 0 || !snapshot->receive_observed_at ||
                !event_precedes_deadline(snapshot->receive_observed_at, session_.deadline()) ||
                snapshot->data.state == core::AuthState::Ready) {
                continue;
            }
            const auto temporary = temporary_ && temporary_sequence_ &&
                                           *temporary_sequence_ < snapshot->receive_event_sequence
                                       ? temporary_
                                       : std::nullopt;
            SingleSendOutcome outcome;
            if (snapshot->client_generation != sent.client_generation ||
                snapshot->data.state == core::AuthState::Closed) {
                outcome = SingleSendGenerationClosed{.temporary = temporary,
                                                     .client_generation = sent.client_generation};
            } else {
                outcome =
                    SingleSendAuthorizationLost{.temporary = temporary,
                                                .client_generation = snapshot->client_generation,
                                                .auth_sequence = snapshot->auth_sequence,
                                                .state = snapshot->data.state};
            }
            if (!result || snapshot->receive_event_sequence < result->sequence) {
                result = StampedOutcome{.sequence = snapshot->receive_event_sequence,
                                        .outcome = std::move(outcome)};
            }
        }
        return result;
    }

    std::optional<StampedOutcome> update_terminal(const SingleSendTemporaryId& temporary) {
        const std::lock_guard lock(event_mutex_);
        std::optional<StampedOutcome> result;
        for (const auto& stamped : updates_) {
            if (stamped.sequence == 0 || !stamped.observed_at ||
                !event_precedes_deadline(stamped.observed_at, session_.deadline())) {
                continue;
            }
            auto candidate = std::visit(
                [&](const auto& update) { return outcome_from_update(update, temporary); },
                stamped.update);
            if (candidate && (!result || stamped.sequence < result->sequence)) {
                result =
                    StampedOutcome{.sequence = stamped.sequence, .outcome = std::move(*candidate)};
            }
        }
        return result;
    }

    static std::optional<StampedOutcome> earliest(std::optional<StampedOutcome> first,
                                                  std::optional<StampedOutcome> second) {
        if (!first) {
            return second;
        }
        if (!second || first->sequence < second->sequence) {
            return first;
        }
        return second;
    }

    std::optional<StampedOutcome> consume_response(std::future<core::TdValue>& response,
                                                   const core::AuthStateSnapshot& sent,
                                                   std::int64_t chat_id, std::int32_t sending_id) {
        try {
            auto value = response.get();
            response_consumed_ = true;
            const auto sequence = value.receive_event_sequence();
            const auto observed_at = value.receive_observed_at();
            if (sequence == 0 || !observed_at) {
                return StampedOutcome{.sequence = std::numeric_limits<std::uint64_t>::max(),
                                      .outcome = SingleSendMalformed{}};
            }
            if (!event_precedes_deadline(observed_at, session_.deadline())) {
                return StampedOutcome{.sequence = std::numeric_limits<std::uint64_t>::max(),
                                      .outcome = SingleSendTimedOut{}};
            }
            if (const auto* error = value.get_if<core::TdError>()) {
                return StampedOutcome{.sequence = sequence,
                                      .outcome = failure_from(*error, std::nullopt,
                                                              SingleSendMutationState::Possible)};
            }
            if (const auto* malformed = value.get_if<core::TdDirectConversionError>()) {
                return StampedOutcome{
                    .sequence = sequence,
                    .outcome = SingleSendMalformed{.temporary = std::nullopt,
                                                   .tdlib_type_id = malformed->tdlib_type_id}};
            }
            const auto* message = value.get_if<core::TdWriteMessage>();
            if (message == nullptr) {
                return StampedOutcome{.sequence = sequence, .outcome = SingleSendMalformed{}};
            }
            switch (message->sending_state.kind) {
            case core::TdMessageSendingStateKind::Pending:
                if (!valid_pending_message(*message, chat_id, sending_id)) {
                    return StampedOutcome{.sequence = sequence,
                                          .outcome = SingleSendMalformed{
                                              .temporary = std::nullopt,
                                              .tdlib_type_id = unsupported_type_id(*message)}};
                }
                temporary_ = SingleSendTemporaryId{.client_generation = sent.client_generation,
                                                   .chat_id = chat_id,
                                                   .temporary_message_id = message->message.id,
                                                   .sending_id = sending_id};
                temporary_sequence_ = sequence;
                return std::nullopt;
            case core::TdMessageSendingStateKind::Failed: {
                const auto* failure_error =
                    message->sending_state.error ? &*message->sending_state.error : nullptr;
                if (!valid_failed_message(*message, chat_id) || failure_error == nullptr) {
                    return StampedOutcome{.sequence = sequence,
                                          .outcome = SingleSendMalformed{
                                              .temporary = std::nullopt,
                                              .tdlib_type_id = unsupported_type_id(*message)}};
                }
                return StampedOutcome{.sequence = sequence,
                                      .outcome = failure_from(*failure_error, std::nullopt,
                                                              SingleSendMutationState::None,
                                                              message->sending_state.retry_after)};
            }
            case core::TdMessageSendingStateKind::Stable: {
                auto success = success_from(*message, std::nullopt, chat_id);
                return StampedOutcome{.sequence = sequence,
                                      .outcome = success
                                                     ? SingleSendOutcome{std::move(*success)}
                                                     : SingleSendOutcome{SingleSendMalformed{}}};
            }
            case core::TdMessageSendingStateKind::Unknown:
                return StampedOutcome{
                    .sequence = sequence,
                    .outcome = SingleSendMalformed{
                        .temporary = std::nullopt,
                        .tdlib_type_id = message->sending_state.unsupported_tdlib_type_id}};
            }
        } catch (const core::TdAuthorizationError& error) {
            response_consumed_ = true;
            return StampedOutcome{.sequence = std::numeric_limits<std::uint64_t>::max(),
                                  .outcome =
                                      SingleSendRejected{.authorization_failure = error.failure()}};
        } catch (const std::exception&) {
            response_consumed_ = true;
            return StampedOutcome{.sequence = std::numeric_limits<std::uint64_t>::max(),
                                  .outcome = SingleSendMalformed{}};
        }
        return std::nullopt;
    }

    struct ArbitrationDecision {
        std::optional<StampedOutcome> selected;
        bool notify_temporary = false;
    };

    static bool authorization_precedes_temporary(const StampedOutcome& selected) {
        if (const auto* lost = std::get_if<SingleSendAuthorizationLost>(&selected.outcome)) {
            return !lost->temporary;
        }
        if (const auto* closed = std::get_if<SingleSendGenerationClosed>(&selected.outcome)) {
            return !closed->temporary;
        }
        return false;
    }

    ArbitrationDecision arbitrate_visible(std::future<core::TdValue>& response,
                                          const core::AuthStateSnapshot& sent, std::int64_t chat_id,
                                          std::int32_t sending_id,
                                          core::TdEventClock::time_point decision_now) {
        ArbitrationDecision decision;
        const auto publication_lock = client_.lock_event_publication();
        record_auth(client_.auth_state());
        auto auth = auth_terminal(sent);
        if (!response_consumed_ && response.wait_for(0ms) == std::future_status::ready) {
            decision.selected = consume_response(response, sent, chat_id, sending_id);
            auth = auth_terminal(sent);
        }
        decision.selected = earliest(std::move(decision.selected), std::move(auth));
        const auto* temporary = temporary_ ? &*temporary_ : nullptr;
        if (temporary != nullptr) {
            decision.selected = earliest(std::move(decision.selected), update_terminal(*temporary));
            if (!temporary_notified_ &&
                (!decision.selected || !authorization_precedes_temporary(*decision.selected))) {
                temporary_notified_ = true;
                decision.notify_temporary = true;
                decision.selected.reset();
            }
        }
        if (!decision.selected && deadline_expired(session_.deadline(), decision_now)) {
            decision.selected = StampedOutcome{
                .sequence = std::numeric_limits<std::uint64_t>::max(),
                .outcome = SingleSendTimedOut{.temporary = temporary_,
                                              .mutation_state = SingleSendMutationState::Possible}};
        }
        return decision;
    }

    std::optional<SingleSendOutcome> notify_temporary() {
        const auto* temporary = temporary_ ? &*temporary_ : nullptr;
        if (temporary == nullptr) {
            return SingleSendMalformed{};
        }
        try {
            if (on_temporary_id_) {
                on_temporary_id_(*temporary);
            }
        } catch (const std::exception&) {
            return SingleSendMalformed{.temporary = temporary_, .tdlib_type_id = std::nullopt};
        }
        return std::nullopt;
    }

    std::uint64_t wake_sequence() const {
        const std::lock_guard lock(event_mutex_);
        return wake_sequence_;
    }

    void wait_for_event(std::uint64_t wake_sequence) {
        if (before_wait_) {
            before_wait_();
        }
        if (wait_) {
            wait_(session_.deadline(), session_.cancellation_token());
            return;
        }
        std::unique_lock lock(event_mutex_);
        const auto changed = [this, wake_sequence] { return wake_sequence_ != wake_sequence; };
        if (const auto expires_at = session_.deadline().expires_at) {
            static_cast<void>(event_condition_.wait_until(lock, session_.cancellation_token(),
                                                          *expires_at, changed));
        } else {
            static_cast<void>(event_condition_.wait(lock, session_.cancellation_token(), changed));
        }
    }

    SingleSendOutcome wait_for_terminal(std::future<core::TdValue>& response,
                                        const core::AuthStateSnapshot& sent, std::int64_t chat_id,
                                        std::int32_t sending_id) {
        for (;;) {
            const auto observed_wake_sequence = wake_sequence();
            if (before_event_arbitration_) {
                before_event_arbitration_();
            }
            const auto decision_now = now_();
            auto decision = arbitrate_visible(response, sent, chat_id, sending_id, decision_now);
            if (decision.notify_temporary) {
                if (auto failure = notify_temporary()) {
                    return std::move(*failure);
                }
                continue;
            }
            if (decision.selected) {
                return std::move(decision.selected->outcome);
            }
            if (session_.cancellation_requested()) {
                return SingleSendCancelled{.temporary = temporary_,
                                           .mutation_state = SingleSendMutationState::Possible};
            }
            wait_for_event(observed_wake_sequence);
        }
    }

    core::TdClient& client_;
    RequestSession& session_;
    std::function<core::TdEventClock::time_point()> now_;
    std::function<void(const RequestDeadline&, const std::stop_token&)> wait_;
    std::function<void()> before_request_;
    std::function<void()> before_event_arbitration_;
    std::function<void()> before_wait_;
    std::function<void(const SingleSendTemporaryId&)> on_temporary_id_;
    bool executed_ = false;
    bool response_consumed_ = false;
    bool temporary_notified_ = false;
    std::optional<SingleSendTemporaryId> temporary_;
    std::optional<std::uint64_t> temporary_sequence_;
    mutable std::mutex event_mutex_;
    std::condition_variable_any event_condition_;
    std::uint64_t wake_sequence_ = 0;
    std::vector<StampedUpdate> updates_;
    std::vector<std::shared_ptr<const core::AuthStateSnapshot>> auth_events_;
    std::shared_ptr<const core::AuthStateSnapshot> latest_auth_;
    std::uint64_t update_subscription_ = 0;
    std::uint64_t response_subscription_ = 0;
    std::uint64_t auth_subscription_ = 0;
};

SingleSendCoordinator::SingleSendCoordinator(core::TdClient& client, RequestSession& session,
                                             SingleSendHooks hooks)
    : impl_(std::make_unique<Impl>(client, session, std::move(hooks))) {}

SingleSendCoordinator::~SingleSendCoordinator() = default;

SingleSendOutcome SingleSendCoordinator::execute(
    core::TdSendMessageRequest request,
    const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
    return impl_->execute(std::move(request), authorization);
}

} // namespace tgcli::daemon
