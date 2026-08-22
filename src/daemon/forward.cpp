#include "daemon/forward.hpp"

#include "daemon/request_session.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;
using ForwardUpdate = std::variant<core::TdUpdateMessageSendSucceeded,
                                   core::TdUpdateMessageSendFailed, core::TdUpdateDeleteMessages>;

struct StampedUpdate {
    std::uint64_t sequence = 0;
    std::optional<core::TdEventClock::time_point> observed_at;
    ForwardUpdate update;
};

struct StampedAuth {
    std::uint64_t sequence = 0;
    std::shared_ptr<const core::AuthStateSnapshot> snapshot;
};

ForwardMutationState mutation_state(const std::vector<ForwardItem>& items) {
    if (std::ranges::any_of(items, [](const ForwardItem& item) {
            return std::holds_alternative<ForwardSent>(item);
        })) {
        return ForwardMutationState::Confirmed;
    }
    if (std::ranges::any_of(items, [](const ForwardItem& item) {
            const auto* failed = std::get_if<ForwardFailed>(&item);
            return std::holds_alternative<ForwardPending>(item) ||
                   (failed != nullptr &&
                    failed->reason == ForwardFailureReason::DeletedBeforeConfirmation);
        })) {
        return ForwardMutationState::Possible;
    }
    return ForwardMutationState::None;
}

bool complete(const std::vector<ForwardItem>& items) {
    return std::ranges::none_of(items, [](const ForwardItem& item) {
        return std::holds_alternative<ForwardPending>(item);
    });
}

std::optional<std::int32_t> tdlib_type_id(const SingleSendOutcome& outcome) {
    const auto* malformed = std::get_if<SingleSendMalformed>(&outcome);
    return malformed == nullptr ? std::nullopt : malformed->tdlib_type_id;
}

ForwardFailed failed_from(std::int64_t source_id, const SingleSendOutcome& outcome) {
    if (const auto* failure = std::get_if<SingleSendFailed>(&outcome)) {
        return {.source_id = source_id,
                .reason = ForwardFailureReason::TdlibError,
                .tdlib_code = failure->error.code,
                .retry_after = std::nullopt};
    }
    const auto& limited = std::get<SingleSendRateLimited>(outcome);
    return {.source_id = source_id,
            .reason = ForwardFailureReason::TdlibError,
            .tdlib_code = limited.error.code,
            .retry_after = limited.retry_after};
}

} // namespace

class ForwardCoordinator::Impl {
  public:
    Impl(core::TdClient& client, RequestSession& session, ForwardHooks hooks)
        : client_(client), session_(session),
          now_(hooks.now ? std::move(hooks.now) : [] { return core::TdEventClock::now(); }),
          wait_(std::move(hooks.wait)), before_request_(std::move(hooks.before_request)),
          before_submit_(std::move(hooks.before_submit)),
          before_event_arbitration_(std::move(hooks.before_event_arbitration)),
          before_wait_(std::move(hooks.before_wait)),
          on_temporary_ids_(std::move(hooks.on_temporary_ids)),
          on_progress_(std::move(hooks.on_progress)) {
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
        settle_in_flight();
        client_.unsubscribe_send_updates(update_subscription_);
        client_.unsubscribe_response_completions(response_subscription_);
        client_.unsubscribe_auth_states(auth_subscription_);
    }

    ForwardOutcome execute(core::TdForwardMessagesRequest request,
                           const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
        auto preparation = prepare(std::move(request), authorization);
        return std::visit(
            [this](auto&& outcome) -> ForwardOutcome {
                using Outcome = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<Outcome, ForwardPrepared>) {
                    return execute_prepared();
                } else {
                    return std::forward<decltype(outcome)>(outcome);
                }
            },
            std::move(preparation));
    }

    ForwardPreparationOutcome
    prepare(core::TdForwardMessagesRequest request,
            const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
        if (executed_ || !authorization || authorization->data.state != core::AuthState::Ready ||
            !core::valid_td_forward_messages_request(request)) {
            return ForwardRejected{};
        }
        executed_ = true;
        if (deadline_expired(session_.deadline(), now_())) {
            return ForwardTimedOut{.items = {}, .mutation_state = ForwardMutationState::None};
        }
        try {
            if (before_request_) {
                before_request_();
            }
        } catch (const std::exception&) {
            return ForwardRejected{};
        }
        if (deadline_expired(session_.deadline(), now_())) {
            return ForwardTimedOut{.items = {}, .mutation_state = ForwardMutationState::None};
        }
        if (session_.cancellation_requested()) {
            return ForwardCancelled{.items = {}, .mutation_state = ForwardMutationState::None};
        }
        if (!session_.reserve_direct_in_flight()) {
            return ForwardCancelled{.items = {}, .mutation_state = ForwardMutationState::None};
        }
        in_flight_ = true;
        source_ids_ = request.message_ids;
        to_chat_id_ = request.to_chat_id;
        sending_id_ = request.sending_id;
        authorization_ = authorization;
        prepared_write_ = client_.prepare_forward_messages(authorization, std::move(request));
        if (!prepared_write_) {
            const auto failure = prepared_write_.authorization_failure();
            auto current = client_.auth_state();
            settle_in_flight();
            if (!current || current->client_generation != authorization->client_generation ||
                current->data.state == core::AuthState::Closed) {
                return ForwardGenerationClosed{.items = {},
                                               .client_generation =
                                                   authorization->client_generation,
                                               .mutation_state = ForwardMutationState::None};
            }
            if (current->data.state != core::AuthState::Ready) {
                return ForwardAuthorizationLost{.items = {},
                                                .client_generation = current->client_generation,
                                                .auth_sequence = current->auth_sequence,
                                                .state = current->data.state,
                                                .mutation_state = ForwardMutationState::None};
            }
            return ForwardRejected{.items = {},
                                   .authorization_failure = failure,
                                   .mutation_state = ForwardMutationState::None};
        }
        if (deadline_expired(session_.deadline(), now_())) {
            prepared_write_ = {};
            settle_in_flight();
            return ForwardTimedOut{.items = {}, .mutation_state = ForwardMutationState::None};
        }
        if (session_.cancellation_requested()) {
            prepared_write_ = {};
            settle_in_flight();
            return ForwardCancelled{.items = {}, .mutation_state = ForwardMutationState::None};
        }
        prepared_ = true;
        return ForwardPrepared{};
    }

    ForwardOutcome execute_prepared() {
        if (!prepared_ || !prepared_write_ || !authorization_) {
            return ForwardRejected{.items = {},
                                   .authorization_failure = std::nullopt,
                                   .mutation_state = ForwardMutationState::Possible};
        }
        prepared_ = false;
        try {
            if (before_submit_) {
                before_submit_();
            }
        } catch (const std::exception&) {
            prepared_write_ = {};
            return finish(ForwardRejected{.items = {},
                                          .authorization_failure = std::nullopt,
                                          .mutation_state = ForwardMutationState::Possible});
        }
        if (deadline_expired(session_.deadline(), now_())) {
            prepared_write_ = {};
            return finish(
                ForwardTimedOut{.items = {}, .mutation_state = ForwardMutationState::Possible});
        }
        if (session_.cancellation_requested()) {
            prepared_write_ = {};
            return finish(
                ForwardCancelled{.items = {}, .mutation_state = ForwardMutationState::Possible});
        }
        std::future<core::TdValue> response;
        try {
            response = client_.send(std::move(prepared_write_));
        } catch (const std::exception&) {
            return finish(ForwardRejected{.items = {},
                                          .authorization_failure = std::nullopt,
                                          .mutation_state = ForwardMutationState::Possible});
        }
        return finish(wait_for_terminal(response, *authorization_));
    }

  private:
    struct ParsedResponse {
        std::uint64_t sequence = 0;
        std::optional<ForwardOutcome> terminal;
        bool vector_available = false;
    };

    void settle_in_flight() {
        if (in_flight_) {
            session_.settle_in_flight();
            in_flight_ = false;
        }
    }

    ForwardOutcome finish(ForwardOutcome outcome) {
        settle_in_flight();
        return outcome;
    }

    void record_update(const core::TdValue& value) {
        std::optional<ForwardUpdate> update;
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

    std::optional<StampedAuth> auth_terminal(const core::AuthStateSnapshot& sent) const {
        const std::lock_guard lock(event_mutex_);
        std::optional<StampedAuth> result;
        for (const auto& snapshot : auth_events_) {
            if (!snapshot ||
                std::tie(snapshot->client_generation, snapshot->auth_sequence) <=
                    std::tie(sent.client_generation, sent.auth_sequence) ||
                snapshot->receive_event_sequence == 0 || !snapshot->receive_observed_at ||
                !event_precedes_deadline(snapshot->receive_observed_at, session_.deadline()) ||
                snapshot->data.state == core::AuthState::Ready) {
                continue;
            }
            if (!result || snapshot->receive_event_sequence < result->sequence) {
                result =
                    StampedAuth{.sequence = snapshot->receive_event_sequence, .snapshot = snapshot};
            }
        }
        return result;
    }

    ForwardOutcome auth_outcome(const StampedAuth& event) const {
        const auto state = items_.empty() ? ForwardMutationState::Possible : mutation_state(items_);
        if (!event.snapshot ||
            event.snapshot->client_generation != authorization_->client_generation ||
            event.snapshot->data.state == core::AuthState::Closed) {
            return ForwardGenerationClosed{.items = items_,
                                           .client_generation = authorization_->client_generation,
                                           .mutation_state = state == ForwardMutationState::None
                                                                 ? ForwardMutationState::Possible
                                                                 : state};
        }
        return ForwardAuthorizationLost{.items = items_,
                                        .client_generation = event.snapshot->client_generation,
                                        .auth_sequence = event.snapshot->auth_sequence,
                                        .state = event.snapshot->data.state,
                                        .mutation_state = state == ForwardMutationState::None
                                                              ? ForwardMutationState::Possible
                                                              : state};
    }

    ParsedResponse consume_response(std::future<core::TdValue>& response,
                                    const core::AuthStateSnapshot& sent) {
        try {
            auto value = response.get();
            response_consumed_ = true;
            const auto sequence = value.receive_event_sequence();
            const auto observed_at = value.receive_observed_at();
            if (sequence == 0 || !observed_at) {
                return {.sequence = std::numeric_limits<std::uint64_t>::max(),
                        .terminal =
                            ForwardMalformed{.items = {},
                                             .tdlib_type_id = std::nullopt,
                                             .mutation_state = ForwardMutationState::Possible}};
            }
            if (!event_precedes_deadline(observed_at, session_.deadline())) {
                return {.sequence = std::numeric_limits<std::uint64_t>::max(),
                        .terminal = ForwardTimedOut{
                            .items = {}, .mutation_state = ForwardMutationState::Possible}};
            }
            if (const auto* error = value.get_if<core::TdError>()) {
                return {.sequence = sequence,
                        .terminal = ForwardTopLevelError{
                            .error = *error, .mutation_state = ForwardMutationState::Possible}};
            }
            if (const auto* malformed = value.get_if<core::TdDirectConversionError>()) {
                return {.sequence = sequence,
                        .terminal =
                            ForwardMalformed{.items = {},
                                             .tdlib_type_id = malformed->tdlib_type_id,
                                             .mutation_state = ForwardMutationState::Possible}};
            }
            const auto* messages = value.get_if<core::TdForwardMessages>();
            if (messages == nullptr || messages->messages.size() != source_ids_.size()) {
                return {.sequence = sequence,
                        .terminal =
                            ForwardMalformed{.items = {},
                                             .tdlib_type_id = std::nullopt,
                                             .mutation_state = ForwardMutationState::Possible}};
            }
            std::vector<ForwardItem> parsed;
            std::vector<std::optional<SingleSendTemporaryId>> temporaries;
            std::set<std::int64_t> unique_temporary_ids;
            parsed.reserve(source_ids_.size());
            temporaries.reserve(source_ids_.size());
            for (std::size_t index = 0; index < source_ids_.size(); ++index) {
                const auto source_id = source_ids_[index];
                if (!messages->messages[index]) {
                    parsed.emplace_back(ForwardFailed{.source_id = source_id,
                                                      .reason = ForwardFailureReason::UpstreamNull,
                                                      .tdlib_code = std::nullopt,
                                                      .retry_after = std::nullopt});
                    temporaries.emplace_back(std::nullopt);
                    continue;
                }
                auto classified = send_arbitration::classify_immediate(
                    *messages->messages[index], sent.client_generation, to_chat_id_, sending_id_);
                if (const auto* pending = std::get_if<SingleSendImmediatePending>(&classified)) {
                    if (!unique_temporary_ids.emplace(pending->temporary.temporary_message_id)
                             .second) {
                        return {.sequence = sequence,
                                .terminal = ForwardMalformed{.items = {},
                                                             .tdlib_type_id = std::nullopt,
                                                             .mutation_state =
                                                                 ForwardMutationState::Possible}};
                    }
                    parsed.emplace_back(ForwardPending{
                        .source_id = source_id,
                        .temporary_message_id = pending->temporary.temporary_message_id});
                    temporaries.emplace_back(pending->temporary);
                    continue;
                }
                auto& outcome = std::get<SingleSendOutcome>(classified);
                if (const auto* success = std::get_if<SingleSendSucceeded>(&outcome)) {
                    parsed.emplace_back(
                        ForwardSent{.source_id = source_id, .message = success->result});
                    temporaries.emplace_back(std::nullopt);
                } else if (std::holds_alternative<SingleSendFailed>(outcome) ||
                           std::holds_alternative<SingleSendRateLimited>(outcome)) {
                    parsed.emplace_back(failed_from(source_id, outcome));
                    temporaries.emplace_back(std::nullopt);
                } else {
                    return {.sequence = sequence,
                            .terminal =
                                ForwardMalformed{.items = {},
                                                 .tdlib_type_id = tdlib_type_id(outcome),
                                                 .mutation_state = ForwardMutationState::Possible}};
                }
            }
            items_ = std::move(parsed);
            temporaries_ = std::move(temporaries);
            return {.sequence = sequence, .terminal = std::nullopt, .vector_available = true};
        } catch (const core::TdAuthorizationError& error) {
            response_consumed_ = true;
            return {.sequence = std::numeric_limits<std::uint64_t>::max(),
                    .terminal = ForwardRejected{.items = {},
                                                .authorization_failure = error.failure(),
                                                .mutation_state = ForwardMutationState::Possible}};
        } catch (const std::exception&) {
            response_consumed_ = true;
            return {.sequence = std::numeric_limits<std::uint64_t>::max(),
                    .terminal = ForwardMalformed{.items = {},
                                                 .tdlib_type_id = std::nullopt,
                                                 .mutation_state = ForwardMutationState::Possible}};
        }
    }

    bool publish_initial() {
        std::vector<std::int64_t> temporary_ids;
        for (const auto& temporary : temporaries_) {
            if (temporary) {
                temporary_ids.push_back(temporary->temporary_message_id);
            }
        }
        try {
            if (!temporary_ids.empty() && on_temporary_ids_) {
                on_temporary_ids_(temporary_ids);
            }
            if (on_progress_) {
                on_progress_(items_);
            }
            initial_published_ = true;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    std::optional<std::size_t> matching_update_index(const StampedUpdate& stamped) const {
        if (processed_update_sequences_.contains(stamped.sequence) || stamped.sequence == 0 ||
            !stamped.observed_at ||
            !event_precedes_deadline(stamped.observed_at, session_.deadline())) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < temporaries_.size(); ++index) {
            if (!temporaries_[index] || !std::holds_alternative<ForwardPending>(items_[index])) {
                continue;
            }
            auto value = std::visit([](const auto& update) { return core::TdValue::from(update); },
                                    stamped.update);
            if (send_arbitration::classify_update(value, *temporaries_[index])) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::optional<StampedUpdate> earliest_update() const {
        const std::lock_guard lock(event_mutex_);
        std::optional<StampedUpdate> result;
        for (const auto& update : updates_) {
            if (matching_update_index(update) && (!result || update.sequence < result->sequence)) {
                result = update;
            }
        }
        return result;
    }

    std::optional<ForwardOutcome> apply_update(const StampedUpdate& stamped) {
        processed_update_sequences_.insert(stamped.sequence);
        bool changed = false;
        for (std::size_t index = 0; index < temporaries_.size(); ++index) {
            if (!temporaries_[index] || !std::holds_alternative<ForwardPending>(items_[index])) {
                continue;
            }
            auto value = std::visit([](const auto& update) { return core::TdValue::from(update); },
                                    stamped.update);
            auto classified = send_arbitration::classify_update(value, *temporaries_[index]);
            if (!classified) {
                continue;
            }
            const auto source_id = source_ids_[index];
            if (const auto* success = std::get_if<SingleSendSucceeded>(&*classified)) {
                items_[index] = ForwardSent{.source_id = source_id, .message = success->result};
            } else if (std::holds_alternative<SingleSendFailed>(*classified) ||
                       std::holds_alternative<SingleSendRateLimited>(*classified)) {
                items_[index] = failed_from(source_id, *classified);
            } else if (std::holds_alternative<SingleSendDeletedBeforeConfirmation>(*classified)) {
                items_[index] =
                    ForwardFailed{.source_id = source_id,
                                  .reason = ForwardFailureReason::DeletedBeforeConfirmation,
                                  .tdlib_code = std::nullopt,
                                  .retry_after = std::nullopt};
            } else {
                return ForwardMalformed{.items = items_,
                                        .tdlib_type_id = tdlib_type_id(*classified),
                                        .mutation_state = ForwardMutationState::Possible};
            }
            temporaries_[index].reset();
            changed = true;
        }
        if (!changed) {
            return std::nullopt;
        }
        try {
            if (on_progress_) {
                on_progress_(items_);
            }
        } catch (const std::exception&) {
            return ForwardMalformed{.items = items_,
                                    .tdlib_type_id = std::nullopt,
                                    .mutation_state = ForwardMutationState::Possible};
        }
        if (complete(items_)) {
            return ForwardCompleted{.items = items_, .mutation_state = mutation_state(items_)};
        }
        return std::nullopt;
    }

    std::uint64_t wake_sequence() const {
        const std::lock_guard lock(event_mutex_);
        return wake_sequence_;
    }

    void wait_for_event(std::uint64_t sequence) {
        if (before_wait_) {
            before_wait_();
        }
        if (wait_) {
            wait_(session_.deadline(), session_.cancellation_token());
            return;
        }
        std::unique_lock lock(event_mutex_);
        const auto changed = [this, sequence] { return wake_sequence_ != sequence; };
        if (const auto expires_at = session_.deadline().expires_at) {
            static_cast<void>(event_condition_.wait_until(lock, session_.cancellation_token(),
                                                          *expires_at, changed));
        } else {
            static_cast<void>(event_condition_.wait(lock, session_.cancellation_token(), changed));
        }
    }

    ForwardOutcome wait_for_terminal(std::future<core::TdValue>& response,
                                     const core::AuthStateSnapshot& sent) {
        std::optional<std::uint64_t> response_sequence;
        for (;;) {
            const auto observed_wake_sequence = wake_sequence();
            if (before_event_arbitration_) {
                before_event_arbitration_();
            }
            const auto decision_now = now_();
            {
                auto publication_lock = client_.lock_event_publication();
                record_auth(client_.auth_state());
                if (!response_consumed_ && response.wait_for(0ms) == std::future_status::ready) {
                    auto parsed = consume_response(response, sent);
                    response_sequence = parsed.sequence;
                    auto auth = auth_terminal(sent);
                    if (parsed.terminal) {
                        if (auth && auth->sequence < parsed.sequence) {
                            return auth_outcome(*auth);
                        }
                        return std::move(*parsed.terminal);
                    }
                    if (!parsed.vector_available) {
                        return ForwardMalformed{.items = {},
                                                .tdlib_type_id = std::nullopt,
                                                .mutation_state = ForwardMutationState::Possible};
                    }
                }
                if (response_sequence && !initial_published_) {
                    auto auth = auth_terminal(sent);
                    auto update = earliest_update();
                    if (auth && auth->sequence < *response_sequence &&
                        (!update || auth->sequence < update->sequence)) {
                        return auth_outcome(*auth);
                    }
                    publication_lock.unlock();
                    if (!publish_initial()) {
                        return ForwardMalformed{.items = items_,
                                                .tdlib_type_id = std::nullopt,
                                                .mutation_state = ForwardMutationState::Possible};
                    }
                    if (complete(items_)) {
                        return ForwardCompleted{.items = items_,
                                                .mutation_state = mutation_state(items_)};
                    }
                    continue;
                }
                if (initial_published_) {
                    auto auth = auth_terminal(sent);
                    auto update = earliest_update();
                    if (update && (!auth || update->sequence < auth->sequence)) {
                        publication_lock.unlock();
                        if (auto terminal = apply_update(*update)) {
                            return std::move(*terminal);
                        }
                        continue;
                    }
                    if (auth) {
                        return auth_outcome(*auth);
                    }
                }
                if (deadline_expired(session_.deadline(), decision_now)) {
                    return ForwardTimedOut{.items = items_,
                                           .mutation_state = items_.empty()
                                                                 ? ForwardMutationState::Possible
                                                                 : mutation_state(items_)};
                }
            }
            if (session_.cancellation_requested()) {
                const auto state =
                    items_.empty() ? ForwardMutationState::Possible : mutation_state(items_);
                return ForwardCancelled{.items = items_,
                                        .mutation_state = state == ForwardMutationState::None
                                                              ? ForwardMutationState::Possible
                                                              : state};
            }
            wait_for_event(observed_wake_sequence);
        }
    }

    core::TdClient& client_;
    RequestSession& session_;
    std::function<core::TdEventClock::time_point()> now_;
    std::function<void(const RequestDeadline&, const std::stop_token&)> wait_;
    std::function<void()> before_request_;
    std::function<void()> before_submit_;
    std::function<void()> before_event_arbitration_;
    std::function<void()> before_wait_;
    std::function<void(const std::vector<std::int64_t>&)> on_temporary_ids_;
    std::function<void(const std::vector<ForwardItem>&)> on_progress_;
    bool executed_ = false;
    bool prepared_ = false;
    bool in_flight_ = false;
    bool response_consumed_ = false;
    bool initial_published_ = false;
    std::int64_t to_chat_id_ = 0;
    std::int32_t sending_id_ = 0;
    std::vector<std::int64_t> source_ids_;
    std::vector<ForwardItem> items_;
    std::vector<std::optional<SingleSendTemporaryId>> temporaries_;
    std::set<std::uint64_t> processed_update_sequences_;
    core::TdPreparedWrite prepared_write_;
    std::shared_ptr<const core::AuthStateSnapshot> authorization_;
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

ForwardCoordinator::ForwardCoordinator(core::TdClient& client, RequestSession& session,
                                       ForwardHooks hooks)
    : impl_(std::make_unique<Impl>(client, session, std::move(hooks))) {}

ForwardCoordinator::~ForwardCoordinator() = default;

ForwardOutcome
ForwardCoordinator::execute(core::TdForwardMessagesRequest request,
                            const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
    return impl_->execute(std::move(request), authorization);
}

ForwardPreparationOutcome
ForwardCoordinator::prepare(core::TdForwardMessagesRequest request,
                            const std::shared_ptr<const core::AuthStateSnapshot>& authorization) {
    return impl_->prepare(std::move(request), authorization);
}

ForwardOutcome ForwardCoordinator::execute_prepared() {
    return impl_->execute_prepared();
}

} // namespace tgcli::daemon
