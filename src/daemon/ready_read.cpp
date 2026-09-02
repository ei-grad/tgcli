#include "daemon/ready_read.hpp"

#include "daemon/request_session.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;

enum class ReadyChangeKind { None, Advanced, Lost, Invalid };

struct ReadyChange {
    ReadyChangeKind kind = ReadyChangeKind::None;
    std::shared_ptr<const core::AuthStateSnapshot> snapshot;
};

enum class WaitStatus { Response, AuthorizationLost, ReadyAdvanced, TimedOut, Cancelled, Failed };

struct WaitResult {
    WaitStatus status = WaitStatus::Failed;
    core::TdValue value;
    std::optional<core::TdAuthorizationFailure> authorization_failure;
    std::shared_ptr<const core::AuthStateSnapshot> snapshot;
};

ReadyReadResult empty_result(ReadyReadStatus status) {
    return {status, {}, std::nullopt, nullptr};
}

bool ready_retry_failure(core::TdAuthorizationFailure failure) {
    switch (failure) {
    case core::TdAuthorizationFailure::GenerationMismatch:
    case core::TdAuthorizationFailure::AuthSequenceMismatch:
    case core::TdAuthorizationFailure::GenerationClosed:
        return true;
    case core::TdAuthorizationFailure::FunctionMismatch:
    case core::TdAuthorizationFailure::TierMismatch:
    case core::TdAuthorizationFailure::OwnerMismatch:
    case core::TdAuthorizationFailure::AuthStateMismatch:
    case core::TdAuthorizationFailure::FunctionDenied:
        return false;
    }
    return false;
}

} // namespace

class ReadyReadSession::Impl {
  public:
    Impl(core::TdClient& client, RequestSession& session, ReadyReadHooks hooks)
        : client_(client), session_(session),
          now_(hooks.now ? std::move(hooks.now) : [] { return core::TdEventClock::now(); }),
          wait_(std::move(hooks.wait)),
          before_event_arbitration_(std::move(hooks.before_event_arbitration)),
          before_wait_(std::move(hooks.before_wait)) {
        response_subscription_ =
            client_.subscribe_response_completions([this](std::uint64_t) { wake(); });
        subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
                observe(snapshot);
            });
        observe(client_.auth_state());
    }

    ~Impl() {
        client_.unsubscribe_response_completions(response_subscription_);
        client_.unsubscribe_auth_states(subscription_);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] std::shared_ptr<const core::AuthStateSnapshot> current() const {
        const std::lock_guard lock(mutex_);
        return latest_;
    }

    [[nodiscard]] std::shared_ptr<const core::AuthStateSnapshot>
    first_non_ready_after(const core::AuthStateSnapshot& snapshot) const {
        const std::lock_guard lock(mutex_);
        const auto found = std::ranges::find_if(pending_, [&](const auto& candidate) {
            return candidate && candidate->client_id == snapshot.client_id &&
                   candidate->client_generation == snapshot.client_generation &&
                   candidate->auth_sequence > snapshot.auth_sequence &&
                   candidate->data.state != core::AuthState::Ready;
        });
        return found == pending_.end() ? nullptr : *found;
    }

    ReadyReadResult read(const ReadyReadStart& start,
                         std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
        for (;;) {
            if (deadline_expired(session_.deadline(), now_())) {
                return empty_result(ReadyReadStatus::TimedOut);
            }
            if (!session_.reserve_direct_in_flight()) {
                return empty_result(ReadyReadStatus::Cancelled);
            }
            if (deadline_expired(session_.deadline(), now_())) {
                session_.settle_in_flight();
                return empty_result(ReadyReadStatus::TimedOut);
            }
            auto response = start(snapshot);
            auto waited = wait_response(response, *snapshot);
            session_.settle_in_flight();
            if (waited.status != WaitStatus::ReadyAdvanced) {
                return external_result(std::move(waited));
            }
            if (session_.cancellation_requested()) {
                return empty_result(ReadyReadStatus::Cancelled);
            }
            if (!waited.snapshot || waited.snapshot->data.state != core::AuthState::Ready) {
                return empty_result(ReadyReadStatus::Failed);
            }
            snapshot = std::move(waited.snapshot);
        }
    }

    ReadyReadResult read_exact(const ReadyReadStart& start,
                               const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
        if (!snapshot || deadline_expired(session_.deadline(), now_())) {
            return empty_result(snapshot ? ReadyReadStatus::TimedOut : ReadyReadStatus::Failed);
        }
        if (!session_.reserve_direct_in_flight()) {
            return empty_result(ReadyReadStatus::Cancelled);
        }
        if (deadline_expired(session_.deadline(), now_())) {
            session_.settle_in_flight();
            return empty_result(ReadyReadStatus::TimedOut);
        }
        auto response = start(snapshot);
        auto waited = wait_response(response, *snapshot);
        session_.settle_in_flight();
        if (waited.status == WaitStatus::ReadyAdvanced) {
            return {
                ReadyReadStatus::AuthorizationLost, {}, std::nullopt, std::move(waited.snapshot)};
        }
        return external_result(std::move(waited));
    }

  private:
    static ReadyReadResult external_result(WaitResult waited) {
        switch (waited.status) {
        case WaitStatus::Response:
            return {.status = ReadyReadStatus::Response,
                    .value = std::move(waited.value),
                    .authorization_failure = waited.authorization_failure,
                    .snapshot = std::move(waited.snapshot)};
        case WaitStatus::AuthorizationLost:
            return {
                ReadyReadStatus::AuthorizationLost, {}, std::nullopt, std::move(waited.snapshot)};
        case WaitStatus::TimedOut:
            return empty_result(ReadyReadStatus::TimedOut);
        case WaitStatus::Cancelled:
            return empty_result(ReadyReadStatus::Cancelled);
        case WaitStatus::ReadyAdvanced:
        case WaitStatus::Failed:
            return {ReadyReadStatus::Failed,
                    {},
                    waited.authorization_failure,
                    std::move(waited.snapshot)};
        }
        return empty_result(ReadyReadStatus::Failed);
    }

    ReadyChange ready_change_after(const core::AuthStateSnapshot& sent) {
        observe(client_.auth_state());
        const std::lock_guard lock(mutex_);
        const auto after_sent = [&](const auto& candidate) {
            return candidate && std::tie(candidate->client_generation, candidate->auth_sequence) >
                                    std::tie(sent.client_generation, sent.auth_sequence);
        };
        std::shared_ptr<const core::AuthStateSnapshot> latest_ready;
        for (const auto& candidate : pending_) {
            if (!after_sent(candidate)) {
                continue;
            }
            if (candidate->receive_event_sequence == 0 || !candidate->receive_observed_at) {
                return {ReadyChangeKind::Invalid, candidate};
            }
            if (!event_precedes_deadline(candidate->receive_observed_at, session_.deadline())) {
                continue;
            }
            if (candidate->data.state != core::AuthState::Ready) {
                return {ReadyChangeKind::Lost, candidate};
            }
            latest_ready = candidate;
        }
        if (latest_ready) {
            return {ReadyChangeKind::Advanced, std::move(latest_ready)};
        }
        return {};
    }

    WaitResult consume_response(std::future<core::TdValue>& response,
                                const core::AuthStateSnapshot& sent) {
        try {
            auto value = response.get();
            const auto change = ready_change_after(sent);
            const auto observed_at = value.receive_observed_at();
            const auto response_sequence = value.receive_event_sequence();
            if (change.kind == ReadyChangeKind::Invalid) {
                return {WaitStatus::Failed, {}, std::nullopt, change.snapshot};
            }
            if (change.kind == ReadyChangeKind::Lost && change.snapshot &&
                change.snapshot->receive_event_sequence != 0 &&
                (response_sequence == 0 ||
                 change.snapshot->receive_event_sequence < response_sequence)) {
                return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
            }
            if (response_sequence == 0 || !observed_at) {
                return {WaitStatus::Failed, {}, std::nullopt, nullptr};
            }
            if (!event_precedes_deadline(observed_at, session_.deadline())) {
                return {WaitStatus::TimedOut, {}, std::nullopt, nullptr};
            }
            return {WaitStatus::Response, std::move(value), std::nullopt, nullptr};
        } catch (const core::TdAuthorizationError& error) {
            const auto change = ready_change_after(sent);
            if (change.kind == ReadyChangeKind::Lost) {
                return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
            }
            if (change.kind == ReadyChangeKind::Advanced && ready_retry_failure(error.failure())) {
                return {WaitStatus::ReadyAdvanced, {}, std::nullopt, change.snapshot};
            }
            if (change.kind == ReadyChangeKind::Invalid) {
                return {WaitStatus::Failed, {}, std::nullopt, change.snapshot};
            }
            return {WaitStatus::Failed, {}, error.failure(), change.snapshot};
        } catch (const std::exception&) {
            const auto change = ready_change_after(sent);
            if (change.kind == ReadyChangeKind::Lost) {
                return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
            }
            if (change.kind == ReadyChangeKind::Invalid) {
                return {WaitStatus::Failed, {}, std::nullopt, change.snapshot};
            }
            return {WaitStatus::Failed, {}, std::nullopt, nullptr};
        }
    }

    WaitResult wait_response(std::future<core::TdValue>& response,
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
                    return consume_response(response, sent);
                }
                const auto change = ready_change_after(sent);
                if (change.kind == ReadyChangeKind::Lost) {
                    return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
                }
                if (change.kind == ReadyChangeKind::Invalid) {
                    return {WaitStatus::Failed, {}, std::nullopt, change.snapshot};
                }
                if (deadline_expired(session_.deadline(), now_())) {
                    return {WaitStatus::TimedOut, {}, std::nullopt, nullptr};
                }
            }
            if (session_.cancellation_requested()) {
                return {WaitStatus::Cancelled, {}, std::nullopt, nullptr};
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

    void wake() {
        const std::lock_guard lock(wait_mutex_);
        ++wake_sequence_;
        wait_condition_.notify_all();
    }

    bool record(const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
        const std::lock_guard lock(mutex_);
        if (!snapshot ||
            (latest_ && std::tie(snapshot->client_generation, snapshot->auth_sequence) <=
                            std::tie(latest_->client_generation, latest_->auth_sequence))) {
            return false;
        }
        latest_ = snapshot;
        pending_.push_back(snapshot);
        return true;
    }

    void observe(const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
        if (record(snapshot)) {
            session_.supersede(snapshot->client_generation, snapshot->auth_sequence);
            wake();
        }
    }

    core::TdClient& client_;
    RequestSession& session_;
    std::function<core::TdEventClock::time_point()> now_;
    std::function<void(const RequestDeadline&, const cancellation::Token&)> wait_;
    std::function<void()> before_event_arbitration_;
    std::function<void()> before_wait_;
    std::mutex wait_mutex_;
    std::condition_variable_any wait_condition_;
    std::uint64_t wake_sequence_ = 0;
    mutable std::mutex mutex_;
    std::shared_ptr<const core::AuthStateSnapshot> latest_;
    std::deque<std::shared_ptr<const core::AuthStateSnapshot>> pending_;
    std::uint64_t response_subscription_ = 0;
    std::uint64_t subscription_ = 0;
};

ReadyReadSession::ReadyReadSession(core::TdClient& client, RequestSession& session,
                                   ReadyReadHooks hooks)
    : impl_(std::make_unique<Impl>(client, session, std::move(hooks))) {}

ReadyReadSession::~ReadyReadSession() = default;

std::shared_ptr<const core::AuthStateSnapshot> ReadyReadSession::current() const {
    return impl_->current();
}

std::shared_ptr<const core::AuthStateSnapshot>
ReadyReadSession::first_non_ready_after(const core::AuthStateSnapshot& snapshot) const {
    return impl_->first_non_ready_after(snapshot);
}

ReadyReadResult ReadyReadSession::read(const ReadyReadStart& start,
                                       std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
    return impl_->read(start, snapshot);
}

ReadyReadResult
ReadyReadSession::read_exact(const ReadyReadStart& start,
                             const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
    return impl_->read_exact(start, snapshot);
}

} // namespace tgcli::daemon
