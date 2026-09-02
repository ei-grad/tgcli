#include "daemon/activity_tracker.hpp"

#include <stdexcept>
#include <utility>

namespace tgcli::daemon {

namespace {

void invoke_expiry_callback(const std::function<void()>& callback) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback();
    } catch (...) {
        // Callback callables are unconstrained std::functions, so their exception
        // types cannot be enumerated. Expiry is already final and is not retried.
        return;
    }
}

} // namespace

struct ActivityTracker::State {
    explicit State(std::function<void()> callback,
                   std::shared_ptr<const testing::ActivityTrackerHooks> test_hooks)
        : on_expired(std::move(callback)), hooks(std::move(test_hooks)) {}

    [[nodiscard]] Clock::time_point now() const {
        if (hooks && hooks->now) {
            return hooks->now();
        }
        return Clock::now();
    }

    [[nodiscard]] static std::optional<Clock::time_point>
    idle_deadline(std::optional<Clock::time_point> zero, const IdleExit& policy) {
        if (!zero || !policy) {
            return std::nullopt;
        }
        const auto maximum_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max());
        if (*policy > maximum_seconds) {
            return Clock::time_point::max();
        }
        const auto duration = std::chrono::duration_cast<Clock::duration>(*policy);
        if (*zero > Clock::time_point::max() - duration) {
            return Clock::time_point::max();
        }
        return *zero + duration;
    }

    void recompute_deadline_locked() {
        deadline = idle_deadline(zero_since, idle_exit);
    }

    [[nodiscard]] bool claim_expiry_locked(Clock::time_point current_time,
                                           std::function<void()>& callback) {
        if (!ready || expired || requests != 0 || subscriptions != 0 || !deadline ||
            current_time < *deadline) {
            return false;
        }
        expired = true;
        if (!callback_claimed) {
            callback_claimed = true;
            callback = on_expired;
        }
        return true;
    }

    void release(Kind kind) {
        {
            const std::lock_guard lock(mutex);
            if (kind == Kind::Request) {
                if (requests != 0) {
                    --requests;
                }
            } else if (subscriptions != 0) {
                --subscriptions;
            }
            if (ready && requests == 0 && subscriptions == 0 && !expired) {
                zero_since = now();
                recompute_deadline_locked();
            }
            ++revision;
        }
        condition.notify_all();
    }

    [[nodiscard]] bool promote_request(void* context, Token::PromotionCommit commit) {
        {
            const std::lock_guard lock(mutex);
            if (requests == 0) {
                return false;
            }
            if (commit != nullptr && !commit(context)) {
                return false;
            }
            --requests;
            ++subscriptions;
            ++revision;
        }
        condition.notify_all();
        return true;
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t revision = 0;
    bool ready = false;
    bool expired = false;
    bool callback_claimed = false;
    std::size_t requests = 0;
    std::size_t subscriptions = 0;
    IdleExit idle_exit;
    std::optional<Clock::time_point> zero_since;
    std::optional<Clock::time_point> deadline;
    std::function<void()> on_expired;
    std::shared_ptr<const testing::ActivityTrackerHooks> hooks;
};

ActivityTracker::Token::Token(std::shared_ptr<State> state, Kind kind)
    : state_(std::move(state)), kind_(kind) {}

ActivityTracker::Token::~Token() {
    reset();
}

ActivityTracker::Token::Token(Token&& other) noexcept
    : state_(std::move(other.state_)), kind_(other.kind_) {}

ActivityTracker::Token& ActivityTracker::Token::operator=(Token&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        kind_ = other.kind_;
    }
    return *this;
}

bool ActivityTracker::Token::promote_to_subscription() {
    return promote_to_subscription(nullptr, nullptr);
}

bool ActivityTracker::Token::promote_to_subscription(void* context, PromotionCommit commit) {
    if (!state_ || kind_ != Kind::Request || !state_->promote_request(context, commit)) {
        return false;
    }
    kind_ = Kind::Subscription;
    return true;
}

void ActivityTracker::Token::reset() {
    if (state_) {
        auto state = std::move(state_);
        state->release(kind_);
    }
}

ActivityTracker::ActivityTracker(std::function<void()> on_expired,
                                 std::shared_ptr<const testing::ActivityTrackerHooks> hooks)
    : state_(std::make_shared<State>(std::move(on_expired), std::move(hooks))) {}

bool ActivityTracker::daemon_ready(IdleExit idle_exit) {
    if (idle_exit && *idle_exit <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("idle exit must be positive");
    }

    {
        const std::lock_guard lock(state_->mutex);
        if (state_->ready) {
            return false;
        }
        state_->ready = true;
        state_->idle_exit = idle_exit;
        if (state_->requests == 0 && state_->subscriptions == 0) {
            state_->zero_since = state_->now();
            state_->recompute_deadline_locked();
        }
        ++state_->revision;
    }
    state_->condition.notify_all();
    return true;
}

std::optional<ActivityTracker::Token> ActivityTracker::try_request() {
    return try_admit(Kind::Request);
}

std::optional<ActivityTracker::Token> ActivityTracker::try_subscription() {
    return try_admit(Kind::Subscription);
}

bool ActivityTracker::update_idle_exit(IdleExit idle_exit) {
    if (idle_exit && *idle_exit <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("idle exit must be positive");
    }

    std::function<void()> callback;
    bool expired = false;
    {
        const std::lock_guard lock(state_->mutex);
        state_->idle_exit = idle_exit;
        state_->recompute_deadline_locked();
        expired = state_->claim_expiry_locked(state_->now(), callback);
        ++state_->revision;
    }
    state_->condition.notify_all();
    invoke_expiry_callback(callback);
    return expired;
}

bool ActivityTracker::expire_if_due() {
    std::function<void()> callback;
    bool expired = false;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->hooks && state_->hooks->before_expire_locked) {
            state_->hooks->before_expire_locked();
        }
        expired = state_->claim_expiry_locked(state_->now(), callback);
        if (expired) {
            ++state_->revision;
        }
    }
    if (expired) {
        state_->condition.notify_all();
    }
    invoke_expiry_callback(callback);
    return expired;
}

ActivityTracker::Snapshot ActivityTracker::snapshot() const {
    const std::lock_guard lock(state_->mutex);
    return {
        .daemon_ready = state_->ready,
        .expired = state_->expired,
        .requests = state_->requests,
        .subscriptions = state_->subscriptions,
        .idle_exit = state_->idle_exit,
        .zero_since = state_->zero_since,
        .deadline = state_->deadline,
    };
}

void ActivityTracker::watch(const cancellation::Token& stop) {
    const auto state = state_;
    std::unique_lock lock(state->mutex);
    while (!stop.stop_requested() && !state->expired) {
        std::function<void()> callback;
        if (state->claim_expiry_locked(state->now(), callback)) {
            ++state->revision;
            lock.unlock();
            state->condition.notify_all();
            invoke_expiry_callback(callback);
            return;
        }
        const auto revision = state->revision;
        const auto changed = [&] {
            return stop.stop_requested() || state->expired || state->revision != revision;
        };
        if (state->deadline) {
            const auto deadline = state->deadline.value_or(Clock::time_point::max());
            static_cast<void>(
                cancellation::wait_until(state->condition, lock, stop, deadline, changed));
        } else {
            static_cast<void>(cancellation::wait(state->condition, lock, stop, changed));
        }
    }
}

std::optional<ActivityTracker::Token> ActivityTracker::try_admit(Kind kind) {
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->hooks && state_->hooks->before_admit_locked) {
            state_->hooks->before_admit_locked();
        }
        if (!state_->ready || state_->expired) {
            return std::nullopt;
        }
        if (kind == Kind::Request) {
            ++state_->requests;
        } else {
            ++state_->subscriptions;
        }
        state_->zero_since.reset();
        state_->deadline.reset();
        ++state_->revision;
    }
    state_->condition.notify_all();
    return Token(state_, kind);
}

} // namespace tgcli::daemon
