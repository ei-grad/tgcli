#include "daemon/stream_subscription.hpp"

#include "common/exit_codes.hpp"
#include "daemon/request_session.hpp"

#include <array>
#include <thread>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace detail {

class StreamSubscriptionState {
  public:
    StreamSubscriptionState(std::shared_ptr<StreamIngressHub> hub,
                            StreamIngressReservation reservation,
                            StreamIngressPreparedActivation prepared, StreamOperation operation,
                            testing::StreamActivationProbe probe)
        : hub_(std::move(hub)), reservation_(std::move(reservation)),
          prepared_(std::move(prepared)), scratch_(kStreamQueueItemBytes), operation_(operation),
          probe_context_(probe.context), subscription_probe_(probe.subscription_hook) {}

    StreamSubscriptionState(const StreamSubscriptionState&) = delete;
    StreamSubscriptionState& operator=(const StreamSubscriptionState&) = delete;
    StreamSubscriptionState(StreamSubscriptionState&&) = delete;
    StreamSubscriptionState& operator=(StreamSubscriptionState&&) = delete;
    ~StreamSubscriptionState() = default;

    void adopt_activity(ActivityTracker::Token activity) noexcept {
        const std::lock_guard lock(ownership_mutex_);
        activity_ = std::move(activity);
    }

    [[nodiscard]] bool commit_promotion() noexcept {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return false;
        }
        return hub_->commit_activation_promotion(prepared_);
    }

    static bool commit_promotion(void* context) noexcept {
        return static_cast<StreamSubscriptionState*>(context)->commit_promotion();
    }

    void publish() noexcept {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return;
        }
        hub_->publish_prepared(reservation_, prepared_);
    }

    [[nodiscard]] bool claim(StreamTerminalPayload payload) noexcept {
        notify(testing::StreamSubscriptionProbePoint::ClaimWaiting);
        const std::lock_guard lock(ownership_mutex_);
        notify(testing::StreamSubscriptionProbePoint::ClaimOwned);
        if (retired_) {
            return false;
        }
        notify(testing::StreamSubscriptionProbePoint::ClaimForwarding);
        payload.operation = operation_;
        return hub_->claim(reservation_, payload);
    }

    [[nodiscard]] std::optional<StreamTerminalPayload> claim_terminal() noexcept {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return std::nullopt;
        }
        return hub_->claim_terminal(reservation_);
    }

    [[nodiscard]] std::optional<StreamTerminalPayload> terminal_snapshot() const noexcept {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return std::nullopt;
        }
        return hub_->terminal_snapshot(reservation_);
    }

    [[nodiscard]] std::optional<StreamActivationProjection> activation_projection() const noexcept {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return std::nullopt;
        }
        return hub_->activation_projection(reservation_);
    }

    [[nodiscard]] bool begin_item() noexcept {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return false;
        }
        return hub_->begin_item_delivery(reservation_);
    }

    [[nodiscard]] std::optional<StreamCopiedItem> copy_front() {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return std::nullopt;
        }
        CopyContext context{
            .scratch = &scratch_, .descriptor = std::nullopt, .size = 0, .valid = true};
        const auto result = hub_->visit_front(reservation_, &context, &copy_item);
        if (result == StreamIngressFrontResult::Empty) {
            return std::nullopt;
        }
        if (result != StreamIngressFrontResult::Visited || !context.valid || context.size == 0 ||
            scratch_.at(context.size - 1) != '\n') {
            throw std::runtime_error("invalid stream ingress item");
        }
        using Difference = std::vector<char>::difference_type;
        return StreamCopiedItem{
            .descriptor = *context.descriptor,
            .data = nlohmann::json::parse(
                scratch_.begin(),
                scratch_.begin() + static_cast<Difference>(context.size - std::size_t{1})),
            .wire_bytes = context.size};
    }

    [[nodiscard]] bool consume_front() {
        const std::lock_guard lock(ownership_mutex_);
        if (retired_) {
            return false;
        }
        return hub_->visit_front(reservation_, nullptr, &consume_item) ==
               StreamIngressFrontResult::Consumed;
    }

    void retire() noexcept {
        notify(testing::StreamSubscriptionProbePoint::RetireWaiting);
        const std::unique_lock lock(ownership_mutex_);
        notify(testing::StreamSubscriptionProbePoint::RetireOwned);
        if (retired_) {
            return;
        }
        const auto state = hub_->activation_state(reservation_);
        if (state == StreamIngressState::Reserved || state == StreamIngressState::Free) {
            retired_ = true;
            return;
        }
        static_cast<void>(hub_->detach(reservation_));
        StreamPollSchedule schedule(StreamPollSchedule::Clock::now());
        while (!hub_->poll_reclaim(reservation_)) {
            const auto wake = schedule.next(std::nullopt);
            std::this_thread::sleep_until(wake);
            schedule.advance(StreamPollSchedule::Clock::now());
        }
        retired_ = true;
    }

    void release_activity() noexcept {
        ActivityTracker::Token activity;
        {
            const std::lock_guard lock(ownership_mutex_);
            if (activity_released_) {
                return;
            }
            activity = std::move(activity_);
            activity_released_ = true;
        }
        activity.reset();
    }

    void teardown() noexcept {
        retire();
        release_activity();
    }

  private:
    void notify(testing::StreamSubscriptionProbePoint point) const noexcept {
        if (subscription_probe_ != nullptr) {
            subscription_probe_(probe_context_, point);
        }
    }

    struct CopyContext {
        std::vector<char>* scratch = nullptr;
        std::optional<StreamIngressDescriptor> descriptor;
        std::size_t size = 0;
        bool valid = true;
    };

    static void copy_spans(void* raw_context, const StreamIngressBorrowedSpan& first,
                           const StreamIngressBorrowedSpan& second) noexcept {
        auto& context = *static_cast<CopyContext*>(raw_context);
        const std::array spans{&first, &second};
        for (const auto* borrowed : spans) {
            const auto size = borrowed->size();
            if (!size || *size > context.scratch->size() - context.size) {
                context.valid = false;
                return;
            }
            context.valid =
                context.valid &&
                borrowed->copy_to(std::span<char>{*context.scratch}.subspan(context.size, *size));
            context.size += *size;
        }
    }

    static StreamIngressFrontAction copy_item(void* raw_context,
                                              const StreamIngressFrontCursor& cursor) {
        auto& context = *static_cast<CopyContext*>(raw_context);
        const auto descriptor = cursor.descriptor();
        if (!descriptor || descriptor->json_size > context.scratch->size() ||
            !cursor.visit_spans(raw_context, &copy_spans)) {
            context.valid = false;
            return StreamIngressFrontAction::Keep;
        }
        context.descriptor = descriptor;
        return StreamIngressFrontAction::Keep;
    }

    static StreamIngressFrontAction consume_item(void* context,
                                                 const StreamIngressFrontCursor& cursor) {
        static_cast<void>(context);
        static_cast<void>(cursor);
        return StreamIngressFrontAction::Consume;
    }

    std::shared_ptr<StreamIngressHub> hub_;
    StreamIngressReservation reservation_;
    StreamIngressPreparedActivation prepared_;
    std::vector<char> scratch_;
    StreamOperation operation_ = StreamOperation::Listen;
    mutable std::mutex ownership_mutex_;
    ActivityTracker::Token activity_;
    void* probe_context_ = nullptr;
    testing::StreamSubscriptionProbeHook subscription_probe_ = nullptr;
    bool retired_ = false;
    bool activity_released_ = false;
};

bool stream_subscription_claim(const std::shared_ptr<StreamSubscriptionState>& state,
                               StreamTerminalPayload payload) noexcept {
    return state && state->claim(payload);
}

void stream_subscription_teardown(const std::shared_ptr<StreamSubscriptionState>& state) noexcept {
    if (state) {
        state->teardown();
    }
}

std::optional<StreamActivationProjection> stream_subscription_activation_projection(
    const std::shared_ptr<StreamSubscriptionState>& state) noexcept {
    return state ? state->activation_projection() : std::nullopt;
}

} // namespace detail

StreamSubscriptionWorker::StreamSubscriptionWorker(
    std::shared_ptr<detail::StreamSubscriptionState> state)
    : state_(std::move(state)) {}

StreamSubscriptionWorker::operator bool() const noexcept {
    return state_ != nullptr;
}

std::optional<StreamActivationProjection>
StreamSubscriptionWorker::activation_projection() const noexcept {
    return detail::stream_subscription_activation_projection(state_);
}

std::optional<StreamTerminalPayload> StreamSubscriptionWorker::terminal_snapshot() const noexcept {
    return state_ ? state_->terminal_snapshot() : std::nullopt;
}

bool StreamSubscriptionWorker::claim(StreamTerminalPayload payload) noexcept {
    return detail::stream_subscription_claim(state_, payload);
}

std::optional<StreamCopiedItem> StreamSubscriptionWorker::pop_front() {
    if (!state_) {
        return std::nullopt;
    }
    auto item = state_->copy_front();
    if (!item) {
        return std::nullopt;
    }
    if (!state_->consume_front()) {
        throw std::runtime_error("stream ingress front changed before consume");
    }
    return item;
}

StreamSubscriptionLease::StreamSubscriptionLease(
    std::shared_ptr<detail::StreamSubscriptionState> state)
    : state_(std::move(state)) {}

StreamSubscriptionLease::~StreamSubscriptionLease() {
    detail::stream_subscription_teardown(state_);
}

StreamSubscriptionLease::StreamSubscriptionLease(StreamSubscriptionLease&& other) noexcept
    : state_(std::move(other.state_)) {}

StreamSubscriptionLease&
StreamSubscriptionLease::operator=(StreamSubscriptionLease&& other) noexcept {
    if (this != &other) {
        detail::stream_subscription_teardown(state_);
        state_ = std::move(other.state_);
    }
    return *this;
}

StreamSubscriptionLease::operator bool() const noexcept {
    return state_ != nullptr;
}

StreamSubscriptionActivationResult RequestSession::activate_stream_subscription(
    const std::shared_ptr<StreamIngressHub>& hub, const StreamIngressRequest& request,
    StreamActivityMode activity_mode, testing::StreamActivationProbe probe) {
    if (!hub) {
        return StreamIngressInvalidRequest{};
    }
    auto admission = hub->reserve(request);
    if (auto* failure = std::get_if<StreamIngressAdmissionFailure>(&admission)) {
        return *failure;
    }
    if (std::holds_alternative<StreamIngressInvalidRequest>(admission)) {
        return StreamIngressInvalidRequest{};
    }
    auto reservation = std::move(std::get<StreamIngressReservation>(admission));
    auto prepared = hub->prepare_activation(reservation);
    if (!prepared) {
        const auto terminal = hub->terminal_snapshot(reservation);
        return terminal ? StreamSubscriptionActivationResult{StreamSubscriptionTerminalClaimed{
                              .terminal = *terminal}}
                        : StreamSubscriptionActivationResult{
                              StreamSubscriptionActivationFailure::PublicationFailed};
    }
    auto state = std::make_shared<detail::StreamSubscriptionState>(
        hub, std::move(reservation), std::move(*prepared), request.operation, probe);
    StreamSubscriptionLease lease(state);
    if (probe.hook != nullptr) {
        probe.hook(probe.context, testing::StreamActivationProbePoint::BeforeLifecycle);
    }

    {
        const std::lock_guard lock(activity_mutex_);
        if (activity_state_ != ActivityState::OpenRequest) {
            return StreamSubscriptionActivationFailure::RequestClosed;
        }
        if (deadline_expired(deadline_)) {
            static_cast<void>(
                state->claim({.cause = StreamTerminalCause::Deadline, .metadata_failure = {}}));
            const auto terminal = state->terminal_snapshot();
            return terminal ? StreamSubscriptionActivationResult{StreamSubscriptionTerminalClaimed{
                                  .terminal = *terminal}}
                            : StreamSubscriptionActivationResult{
                                  StreamSubscriptionActivationFailure::PublicationFailed};
        }
        if (cancellation_requested()) {
            static_cast<void>(
                state->claim({.cause = StreamTerminalCause::Disconnected, .metadata_failure = {}}));
            return StreamSubscriptionActivationFailure::RequestClosed;
        }
        if ((activity_mode == StreamActivityMode::TrackedDaemon && !activity_) ||
            (activity_mode == StreamActivityMode::UntrackedNoDaemon && activity_)) {
            return StreamSubscriptionActivationFailure::ActivityUnavailable;
        }
        bool promotion_committed = false;
        if (activity_mode == StreamActivityMode::TrackedDaemon) {
            promotion_committed = activity_.promote_to_subscription(
                state.get(), &detail::StreamSubscriptionState::commit_promotion);
        } else {
            promotion_committed = state->commit_promotion();
        }
        if (!promotion_committed) {
            const auto terminal = state->terminal_snapshot();
            return terminal ? StreamSubscriptionActivationResult{StreamSubscriptionTerminalClaimed{
                                  .terminal = *terminal}}
                            : StreamSubscriptionActivationResult{
                                  StreamSubscriptionActivationFailure::PublicationFailed};
        }
        state->adopt_activity(std::move(activity_));
        activity_state_ = ActivityState::OpenStreamSubscription;
        stream_subscription_.emplace(std::move(lease));
        if (probe.hook != nullptr) {
            probe.hook(probe.context, testing::StreamActivationProbePoint::AfterPromotion);
        }
        state->publish();
    }
    return StreamSubscriptionActivated{};
}

std::optional<StreamActivationProjection> RequestSession::stream_activation_projection() const {
    std::shared_ptr<detail::StreamSubscriptionState> state;
    {
        const std::lock_guard lock(activity_mutex_);
        if (activity_state_ != ActivityState::OpenStreamSubscription || !stream_subscription_ ||
            !stream_subscription_->state_) {
            return std::nullopt;
        }
        state = stream_subscription_->state_;
    }
    return detail::stream_subscription_activation_projection(state);
}

StreamSubscriptionWorker RequestSession::stream_worker() const {
    const std::lock_guard lock(activity_mutex_);
    if (activity_state_ != ActivityState::OpenStreamSubscription || !stream_subscription_ ||
        !stream_subscription_->state_) {
        return {};
    }
    return StreamSubscriptionWorker(stream_subscription_->state_);
}

class detail::StreamDeliveryRunner {
  public:
    StreamDeliveryRunner(RequestSession& session, std::shared_ptr<StreamSubscriptionState> state,
                         const StreamDeliveryOptions& options)
        : session_(&session), state_(std::move(state)), item_options_(&options),
          deadline_(session.deadline().expires_at), schedule_(current_time()) {}

    StreamDeliveryRunner(RequestSession& session, std::shared_ptr<StreamSubscriptionState> state,
                         const StreamMatchDeliveryOptions& options)
        : session_(&session), state_(std::move(state)), match_options_(&options),
          deadline_(session.deadline().expires_at), schedule_(current_time()) {}

    [[nodiscard]] StreamDeliveryStatus run() {
        if (match_options_ != nullptr && match_options_->initial_match) {
            if (state_->claim(
                    {.cause = StreamTerminalCause::PlannedSuccess, .metadata_failure = {}})) {
                matched_ = match_options_->initial_match;
            }
        }
        for (;;) {
            sleep_until(schedule_.next(deadline_));
            const auto current = current_time();
            claim_scheduled_terminal(current);
            if (const auto* const hook = hooks(); hook != nullptr && hook->probe != nullptr) {
                hook->probe(hook->probe_context,
                            testing::StreamDeliveryHooks::ProbePoint::AfterScheduledTerminalClaim);
            }
            if (auto status = deliver_terminal()) {
                return status.value();
            }
            if (auto status = deliver_item()) {
                return status.value();
            }
            schedule_.advance(current);
        }
    }

  private:
    [[nodiscard]] StreamPollSchedule::Clock::time_point current_time() const {
        const auto* const hook = hooks();
        return hook != nullptr && hook->now ? hook->now() : StreamPollSchedule::Clock::now();
    }

    void sleep_until(StreamPollSchedule::Clock::time_point wake) const {
        const auto* const hook = hooks();
        if (hook != nullptr && hook->sleep_until) {
            hook->sleep_until(wake);
            return;
        }
        std::this_thread::sleep_until(wake);
    }

    void claim_scheduled_terminal(StreamPollSchedule::Clock::time_point current) noexcept {
        if (deadline_ && current >= deadline_.value()) {
            static_cast<void>(
                state_->claim({.cause = StreamTerminalCause::Deadline, .metadata_failure = {}}));
            return;
        }
        if (session_->cancellation_requested()) {
            static_cast<void>(state_->claim(
                {.cause = StreamTerminalCause::Disconnected, .metadata_failure = {}}));
        }
    }

    [[nodiscard]] DeliveryOutcome deliver_terminal_frame(StreamTerminalFrame frame) {
        auto& sink = static_cast<ResponseSink&>(*session_);
        if (auto* result = std::get_if<StreamTerminalResultFrame>(&frame)) {
            return sink.forward_stream_result(std::move(result->data));
        }
        if (auto* error = std::get_if<StreamTerminalErrorFrame>(&frame)) {
            return sink.forward_stream_error(std::move(error->code), std::move(error->message),
                                             std::move(error->details), error->exit_code);
        }
        return DeliveryOutcome::Suppressed;
    }

    [[nodiscard]] static StreamTerminalFrame
    protocol_answer_invalid_frame(const StreamTerminalPayload& terminal) {
        return StreamTerminalErrorFrame{
            .code = "PROTOCOL_ANSWER_INVALID",
            .message = "invalid challenge answer",
            .details = {{"request_id", terminal.protocol_request_id},
                        {"reason", std::string(stream_protocol_answer_invalid_reason_name(
                                       terminal.protocol_reason))}},
            .exit_code = kUsage};
    }

    [[nodiscard]] StreamDeliveryStatus finish(StreamDeliveryStatus status) noexcept {
        std::optional<StreamSubscriptionLease> subscription;
        {
            const std::lock_guard lock(session_->activity_mutex_);
            if (session_->stream_subscription_ &&
                session_->stream_subscription_->state_ == state_) {
                subscription = std::move(session_->stream_subscription_);
                session_->stream_subscription_.reset();
                session_->activity_state_ = RequestSession::ActivityState::Released;
            }
        }
        subscription.reset();
        return status;
    }

    [[nodiscard]] StreamDeliveryStatus transport_failure(StreamDeliveryStatus status) noexcept {
        static_cast<void>(
            state_->claim({.cause = StreamTerminalCause::Disconnected, .metadata_failure = {}}));
        session_->abort_transport();
        session_->cancel_stream_transport();
        session_->disconnect();
        return finish(status);
    }

    [[nodiscard]] std::optional<StreamDeliveryStatus> deliver_terminal() {
        auto terminal = state_->claim_terminal();
        if (!terminal) {
            return std::nullopt;
        }
        state_->retire();
        if (terminal->cause == StreamTerminalCause::Disconnected) {
            return finish(StreamDeliveryStatus::Disconnected);
        }
        try {
            StreamTerminalFrame frame;
            if (terminal->cause == StreamTerminalCause::ProtocolAnswerInvalid) {
                frame = protocol_answer_invalid_frame(*terminal);
            } else if (terminal->cause == StreamTerminalCause::PlannedSuccess &&
                       match_options_ != nullptr) {
                frame =
                    matched_
                        ? StreamTerminalFrame{StreamTerminalResultFrame{*matched_}}
                        : StreamTerminalFrame{StreamTerminalErrorFrame{
                              .code = "INTERNAL",
                              .message = "wait-for completed without a message",
                              .details = {{"operation", "wait_for"}, {"reason", "internal_error"}},
                              .exit_code = kGeneric}};
            } else {
                const auto* const builder = terminal_builder();
                frame =
                    builder != nullptr
                        ? (*builder)(*terminal, delivered_)
                        : StreamTerminalFrame{StreamTerminalErrorFrame{
                              .code = "INTERNAL",
                              .message = "stream terminal mapper is unavailable",
                              .details = {{"operation", "listen"}, {"reason", "internal_error"}},
                              .exit_code = kGeneric}};
            }
            const auto outcome = deliver_terminal_frame(frame);
            if (outcome == DeliveryOutcome::Complete) {
                return StreamDeliveryStatus::TerminalComplete;
            }
            if (outcome == DeliveryOutcome::Disconnected) {
                return transport_failure(StreamDeliveryStatus::Disconnected);
            }
            return finish(StreamDeliveryStatus::Suppressed);
        } catch (...) {
            // The injected terminal builder and transport expose arbitrary exception types. Once
            // terminal delivery starts, none can be retried without risking a duplicate frame.
            return transport_failure(StreamDeliveryStatus::Disconnected);
        }
    }

    [[nodiscard]] std::optional<StreamDeliveryStatus> deliver_item() {
        try {
            auto item = state_->copy_front();
            if (!item) {
                return std::nullopt;
            }
            if (match_options_ != nullptr) {
                auto match = match_options_->item_matcher ? match_options_->item_matcher(*item)
                                                          : std::optional<nlohmann::json>{};
                static_cast<void>(state_->consume_front());
                if (match && state_->claim({.cause = StreamTerminalCause::PlannedSuccess,
                                            .metadata_failure = {}})) {
                    matched_ = std::move(*match);
                }
                return std::nullopt;
            }
            if (!state_->begin_item()) {
                return std::nullopt;
            }
            const auto outcome = session_->item(std::move(item->data));
            if (outcome != DeliveryOutcome::Complete) {
                const auto status = outcome == DeliveryOutcome::Suppressed
                                        ? StreamDeliveryStatus::Suppressed
                                        : StreamDeliveryStatus::Disconnected;
                return transport_failure(status);
            }
            ++delivered_;
            static_cast<void>(state_->consume_front());
            if (item_options_->count && delivered_ == item_options_->count.value()) {
                static_cast<void>(state_->claim(
                    {.cause = StreamTerminalCause::PlannedSuccess, .metadata_failure = {}}));
            }
            return std::nullopt;
        } catch (...) {
            // The transport is an application callback and may throw types outside the standard
            // hierarchy. A begun item cannot be retried without risking a duplicate frame.
            return transport_failure(StreamDeliveryStatus::Disconnected);
        }
    }

    RequestSession* session_ = nullptr;
    std::shared_ptr<StreamSubscriptionState> state_;
    const StreamDeliveryOptions* item_options_ = nullptr;
    const StreamMatchDeliveryOptions* match_options_ = nullptr;
    std::optional<StreamPollSchedule::Clock::time_point> deadline_;
    StreamPollSchedule schedule_;
    std::uint64_t delivered_ = 0;
    std::optional<nlohmann::json> matched_;

    [[nodiscard]] const testing::StreamDeliveryHooks* hooks() const noexcept {
        if (item_options_ != nullptr) {
            return item_options_->hooks.get();
        }
        return match_options_ != nullptr ? match_options_->hooks.get() : nullptr;
    }

    [[nodiscard]] const StreamTerminalBuilder* terminal_builder() const noexcept {
        if (item_options_ != nullptr) {
            return &item_options_->terminal_builder;
        }
        return match_options_ != nullptr ? &match_options_->terminal_builder : nullptr;
    }
};

StreamDeliveryStatus run_stream_delivery(RequestSession& session,
                                         const StreamDeliveryOptions& options) {
    std::shared_ptr<detail::StreamSubscriptionState> state;
    {
        const std::lock_guard lock(session.activity_mutex_);
        if (session.activity_state_ != RequestSession::ActivityState::OpenStreamSubscription ||
            !session.stream_subscription_ || !*session.stream_subscription_) {
            return StreamDeliveryStatus::InvalidLease;
        }
        state = session.stream_subscription_->state_;
    }
    if ((options.count && options.count.value() == 0) || !options.terminal_builder) {
        return StreamDeliveryStatus::InvalidLease;
    }
    return detail::StreamDeliveryRunner(session, std::move(state), options).run();
}

StreamDeliveryStatus run_stream_match_delivery(RequestSession& session,
                                               const StreamMatchDeliveryOptions& options) {
    std::shared_ptr<detail::StreamSubscriptionState> state;
    {
        const std::lock_guard lock(session.activity_mutex_);
        if (session.activity_state_ != RequestSession::ActivityState::OpenStreamSubscription ||
            !session.stream_subscription_ || !*session.stream_subscription_) {
            return StreamDeliveryStatus::InvalidLease;
        }
        state = session.stream_subscription_->state_;
    }
    if (!options.terminal_builder || (!options.item_matcher && !options.initial_match)) {
        return StreamDeliveryStatus::InvalidLease;
    }
    return detail::StreamDeliveryRunner(session, std::move(state), options).run();
}

} // namespace tgcli::daemon
