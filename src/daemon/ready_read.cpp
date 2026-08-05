#include "daemon/ready_read.hpp"

#include "daemon/request_session.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace tgcli::daemon {

namespace {

using namespace std::chrono_literals;

enum class ReadyChangeKind { None, Advanced, Lost };

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

} // namespace

class ReadyReadSession::Impl {
  public:
    Impl(core::TdClient& client, RequestSession& session) : client_(client), session_(session) {
        subscription_ = client_.subscribe_auth_states(
            [this](const std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
                observe(snapshot);
            });
        observe(client_.auth_state());
    }

    ~Impl() {
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

    ReadyReadResult read(const ReadyReadStart& start,
                         std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
        for (;;) {
            if (RequestSession::Clock::now() >= session_.deadline()) {
                return empty_result(ReadyReadStatus::TimedOut);
            }
            if (!session_.reserve_direct_in_flight()) {
                return empty_result(ReadyReadStatus::Cancelled);
            }
            if (RequestSession::Clock::now() >= session_.deadline()) {
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
        const auto lost = std::ranges::find_if(pending_, [&](const auto& candidate) {
            return after_sent(candidate) && candidate->data.state != core::AuthState::Ready;
        });
        if (lost != pending_.end()) {
            return {ReadyChangeKind::Lost, *lost};
        }
        if (after_sent(latest_) && latest_->data.state == core::AuthState::Ready) {
            return {ReadyChangeKind::Advanced, latest_};
        }
        return {};
    }

    WaitResult consume_response(std::future<core::TdValue>& response,
                                const core::AuthStateSnapshot& sent) {
        try {
            auto value = response.get();
            const auto change = ready_change_after(sent);
            if (change.kind == ReadyChangeKind::Lost && change.snapshot &&
                change.snapshot->receive_event_sequence != 0 &&
                (value.receive_event_sequence() == 0 ||
                 change.snapshot->receive_event_sequence < value.receive_event_sequence())) {
                return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
            }
            return {WaitStatus::Response, std::move(value), std::nullopt, nullptr};
        } catch (const core::TdAuthorizationError& error) {
            const auto change = ready_change_after(sent);
            if (change.kind == ReadyChangeKind::Lost) {
                return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
            }
            if (change.kind == ReadyChangeKind::Advanced) {
                return {WaitStatus::ReadyAdvanced, {}, std::nullopt, change.snapshot};
            }
            return {WaitStatus::Failed, {}, error.failure(), nullptr};
        } catch (const std::exception&) {
            const auto change = ready_change_after(sent);
            if (change.kind == ReadyChangeKind::Lost) {
                return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
            }
            if (change.kind == ReadyChangeKind::Advanced) {
                return {WaitStatus::ReadyAdvanced, {}, std::nullopt, change.snapshot};
            }
            return {WaitStatus::Failed, {}, std::nullopt, nullptr};
        }
    }

    WaitResult wait_response(std::future<core::TdValue>& response,
                             const core::AuthStateSnapshot& sent) {
        for (;;) {
            if (response.wait_for(0ms) == std::future_status::ready) {
                return consume_response(response, sent);
            }
            const auto change = ready_change_after(sent);
            if (change.kind == ReadyChangeKind::Lost) {
                return {WaitStatus::AuthorizationLost, {}, std::nullopt, change.snapshot};
            }
            if (RequestSession::Clock::now() >= session_.deadline()) {
                return {WaitStatus::TimedOut, {}, std::nullopt, nullptr};
            }
            if (session_.cancellation_requested() &&
                session_.in_flight_state() != InFlightState::Orphaned) {
                return {WaitStatus::Cancelled, {}, std::nullopt, nullptr};
            }
            std::this_thread::sleep_for(1ms);
        }
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
        }
    }

    core::TdClient& client_;
    RequestSession& session_;
    mutable std::mutex mutex_;
    std::shared_ptr<const core::AuthStateSnapshot> latest_;
    std::deque<std::shared_ptr<const core::AuthStateSnapshot>> pending_;
    std::uint64_t subscription_ = 0;
};

ReadyReadSession::ReadyReadSession(core::TdClient& client, RequestSession& session)
    : impl_(std::make_unique<Impl>(client, session)) {}

ReadyReadSession::~ReadyReadSession() = default;

std::shared_ptr<const core::AuthStateSnapshot> ReadyReadSession::current() const {
    return impl_->current();
}

ReadyReadResult ReadyReadSession::read(const ReadyReadStart& start,
                                       std::shared_ptr<const core::AuthStateSnapshot>& snapshot) {
    return impl_->read(start, snapshot);
}

} // namespace tgcli::daemon
