#include "daemon/config_runtime.hpp"

#include <algorithm>
#include <utility>

namespace tgcli::daemon {

ConfigRuntime::ConfigRuntime(std::string config_path,
                             std::shared_ptr<const testing::ConfigRuntimeHooks> hooks,
                             uid_t expected_uid)
    : store_(std::move(config_path), expected_uid), snapshots_(store_), hooks_(std::move(hooks)) {
    const auto initialized_at = now();
    static_cast<void>(snapshots_.initialize(initialized_at));
    publication_.store(std::make_shared<const RuntimePublication>(
        RuntimePublication{.snapshot = snapshots_.current(), .generation = generation_}));
    next_poll_ = initialized_at + kPollInterval;
    worker_ = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

ConfigRuntime::~ConfigRuntime() {
    {
        const std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

ConfigAdmissionResult ConfigRuntime::admit(std::string_view account,
                                           const RequestDeadline& deadline,
                                           const std::stop_token& cancellation) {
    if (hooks_ && hooks_->admission_deadline) {
        hooks_->admission_deadline(deadline);
    }
    std::unique_lock lock(mutex_);
    const auto finish = [&](ConfigRefreshStatus status,
                            std::optional<ConfigAdmissionDecision> decision = std::nullopt) {
        lock.unlock();
        if (hooks_ && hooks_->admission_finished) {
            hooks_->admission_finished(status);
        }
        return ConfigAdmissionResult{status, std::move(decision)};
    };
    if (stopped_) {
        return finish(ConfigRefreshStatus::Stopped);
    }
    if (cancellation.stop_requested()) {
        return finish(ConfigRefreshStatus::Cancelled);
    }
    if (deadline_expired(deadline, now())) {
        return finish(ConfigRefreshStatus::TimedOut);
    }

    const auto refresh = ++requested_refresh_;
    condition_.notify_all();
    const std::stop_callback cancellation_wakeup(cancellation, [this] { condition_.notify_all(); });

    while (completed_refresh_ < refresh) {
        if (stopped_) {
            return finish(ConfigRefreshStatus::Stopped);
        }
        if (cancellation.stop_requested()) {
            return finish(ConfigRefreshStatus::Cancelled);
        }
        if (deadline_expired(deadline, now())) {
            return finish(ConfigRefreshStatus::TimedOut);
        }
        const auto changed = [&] {
            return completed_refresh_ >= refresh || stopped_ || cancellation.stop_requested();
        };
        if (deadline.expires_at) {
            wait_for_work(lock, *deadline.expires_at, changed);
        } else {
            condition_.wait(lock, changed);
        }
    }

    if (cancellation.stop_requested()) {
        return finish(ConfigRefreshStatus::Cancelled);
    }
    if (deadline_expired(deadline, now())) {
        return finish(ConfigRefreshStatus::TimedOut);
    }
    if (stopped_) {
        return finish(ConfigRefreshStatus::Stopped);
    }
    return finish(ConfigRefreshStatus::Completed, admission_decision(account));
}

ConfigRuntimeSnapshot ConfigRuntime::current(std::string_view account) const {
    return current_locked(account);
}

void ConfigRuntime::set_publication_observer(PublicationObserver observer) {
    std::unique_lock lock(mutex_);
    const bool replacing_from_callback =
        publication_callbacks_ != 0 && publication_callback_thread_ == std::this_thread::get_id();
    if (!replacing_from_callback) {
        publication_observer_ = {};
        publication_notification_pending_ = false;
        ++publication_observer_revision_;
        condition_.wait(lock, [this] { return publication_callbacks_ == 0; });
    }
    publication_observer_ = std::move(observer);
    publication_notification_pending_ = static_cast<bool>(publication_observer_);
    ++publication_observer_revision_;
    condition_.notify_all();
}

ConfigRuntime::Clock::time_point ConfigRuntime::now() const {
    if (hooks_ && hooks_->now) {
        return hooks_->now();
    }
    return Clock::now();
}

void ConfigRuntime::wait_for_work(std::unique_lock<std::mutex>& lock, Clock::time_point deadline,
                                  const testing::ConfigRuntimeHooks::Predicate& predicate) {
    if (hooks_ && hooks_->wait_until) {
        hooks_->wait_until(condition_, lock, deadline, predicate);
        return;
    }
    static_cast<void>(condition_.wait_until(lock, deadline, predicate));
}

void ConfigRuntime::dispatch_publication_observer(std::unique_lock<std::mutex>& lock) {
    if (!publication_notification_pending_ || !publication_observer_) {
        return;
    }
    const auto observer = publication_observer_;
    const auto observer_revision = publication_observer_revision_;
    publication_notification_pending_ = false;
    ++publication_callbacks_;
    publication_callback_thread_ = std::this_thread::get_id();
    lock.unlock();

    bool callback_failed = false;
    try {
        observer();
    } catch (...) {
        // Observer callables are unconstrained std::functions, so their exception
        // types cannot be enumerated. A failed observer is detached below.
        callback_failed = true;
    }

    lock.lock();
    --publication_callbacks_;
    publication_callback_thread_ = {};
    if (callback_failed && publication_observer_revision_ == observer_revision) {
        publication_observer_ = {};
        publication_notification_pending_ = false;
        ++publication_observer_revision_;
    }
    condition_.notify_all();
}

void ConfigRuntime::run(std::stop_token stop) {
    const std::stop_callback stop_wakeup(stop, [this] { condition_.notify_all(); });
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested() && !stopped_) {
        if (publication_notification_pending_) {
            dispatch_publication_observer(lock);
            continue;
        }
        const auto current_time = now();
        const bool forced = requested_refresh_ > completed_refresh_;
        const bool poll_due = current_time >= next_poll_;
        if (!forced && !poll_due) {
            wait_for_work(lock, next_poll_, [&] {
                return stop.stop_requested() || stopped_ ||
                       requested_refresh_ > completed_refresh_ || publication_notification_pending_;
            });
            continue;
        }

        const auto refresh_target = requested_refresh_;
        if (poll_due) {
            next_poll_ = current_time + kPollInterval;
        }

        lock.unlock();
        if (hooks_ && hooks_->before_reload) {
            hooks_->before_reload(forced);
        }
        const config::MutationControl reload_control{
            Clock::now() + config::SnapshotManager::kLoadTimeout, stop};
        static_cast<void>(snapshots_.reload(reload_control));
        const auto published = snapshots_.current();
        lock.lock();

        ++generation_;
        publication_.store(std::make_shared<const RuntimePublication>(
            RuntimePublication{.snapshot = published, .generation = generation_}));
        publication_notification_pending_ = static_cast<bool>(publication_observer_);
        if (forced) {
            completed_refresh_ = std::max(completed_refresh_, refresh_target);
        }
        condition_.notify_all();
        dispatch_publication_observer(lock);
        lock.unlock();
        if (hooks_ && hooks_->after_reload) {
            hooks_->after_reload(forced);
        }
        lock.lock();
    }
    condition_.notify_all();
}

ConfigAdmissionDecision ConfigRuntime::admission_decision(std::string_view account) const {
    const auto runtime = publication_.load();
    const auto published = runtime ? runtime->snapshot : nullptr;
    const auto generation = runtime ? runtime->generation : 0;
    if (!published || !published->snapshot) {
        return ConfigAdmissionDenied{
            .state = ConfigAdmissionState::ConfigInvalidWithoutLastGood,
            .account = std::string(account),
            .snapshot_identity = std::nullopt,
            .generation = generation,
            .reload_diagnostic = published && published->error
                                     ? std::optional<ReloadDiagnostic>{ReloadDiagnostic{
                                           published->error.value_or(config::ConfigError{}).reason}}
                                     : std::nullopt,
            .last_good_settings = std::nullopt,
            .last_good_account_present = false,
        };
    }

    const auto& snapshot = *published->snapshot;
    const auto invalid_diagnostic =
        published->error ? std::optional<ReloadDiagnostic>{ReloadDiagnostic{
                               published->error.value_or(config::ConfigError{}).reason}}
                         : std::nullopt;
    const auto admission_state = published->error ? ConfigAdmissionState::ConfigInvalidWithLastGood
                                                  : ConfigAdmissionState::Ready;
    if (const auto selected = snapshot.accounts.find(account);
        selected != snapshot.accounts.end()) {
        auto account_snapshot = std::make_shared<config::ConfigSnapshot>();
        account_snapshot->identity = snapshot.identity;
        if (snapshot.default_account == account) {
            account_snapshot->default_account = std::string(account);
        }
        account_snapshot->accounts.emplace(std::string(account), selected->second);
        return std::make_shared<const AdmittedAccountConfig>(AdmittedAccountConfig{
            .state = admission_state,
            .account = std::string(account),
            .settings =
                {
                    .allow_write = selected->second.allow_write,
                    .idle_exit = selected->second.idle_exit,
                },
            .account_snapshot = std::move(account_snapshot),
            .snapshot_identity = snapshot.identity,
            .generation = generation,
            .is_default = snapshot.default_account == account,
            .standing_write_grants_valid = published->standing_write_grants_valid,
            .last_good_account_present = true,
            .reload_diagnostic = invalid_diagnostic,
        });
    }
    if (snapshot.accounts.empty() && account == "main") {
        auto account_snapshot = std::make_shared<config::ConfigSnapshot>();
        account_snapshot->identity = snapshot.identity;
        return std::make_shared<const AdmittedAccountConfig>(AdmittedAccountConfig{
            .state = published->error ? ConfigAdmissionState::ConfigInvalidWithLastGood
                                      : ConfigAdmissionState::ImplicitMain,
            .account = "main",
            .settings = {},
            .account_snapshot = std::move(account_snapshot),
            .snapshot_identity = snapshot.identity,
            .generation = generation,
            .is_default = true,
            .standing_write_grants_valid = published->standing_write_grants_valid,
            .last_good_account_present = false,
            .reload_diagnostic = invalid_diagnostic,
        });
    }
    return ConfigAdmissionDenied{
        .state = published->error ? ConfigAdmissionState::ConfigInvalidWithLastGood
                                  : ConfigAdmissionState::AccountMissing,
        .account = std::string(account),
        .snapshot_identity = snapshot.identity,
        .generation = generation,
        .reload_diagnostic = invalid_diagnostic,
        .last_good_settings = std::nullopt,
        .last_good_account_present = false,
    };
}

ConfigRuntimeSnapshot ConfigRuntime::current_locked(std::string_view account) const {
    const auto runtime = publication_.load();
    const auto published = runtime ? runtime->snapshot : nullptr;
    ConfigRuntimeSnapshot result{
        .account = std::string(account),
        .idle_exit = std::nullopt,
        .snapshot_identity = std::nullopt,
        .generation = runtime ? runtime->generation : 0,
        .reload_diagnostic = std::nullopt,
    };
    if (published && published->snapshot) {
        const auto& snapshot = *published->snapshot;
        result.snapshot_identity = snapshot.identity;
        if (const auto selected = snapshot.accounts.find(account);
            selected != snapshot.accounts.end()) {
            result.idle_exit = selected->second.idle_exit;
            result.state = ConfigAdmissionState::Ready;
        } else if (snapshot.accounts.empty() && account == "main") {
            result.state = ConfigAdmissionState::ImplicitMain;
        } else {
            result.state = ConfigAdmissionState::AccountMissing;
        }
    }
    if (published && published->error.has_value()) {
        const auto reason = published->error.value_or(config::ConfigError{}).reason;
        result.state = published->snapshot ? ConfigAdmissionState::ConfigInvalidWithLastGood
                                           : ConfigAdmissionState::ConfigInvalidWithoutLastGood;
        result.reload_diagnostic = ReloadDiagnostic{reason};
    } else if (!published || !published->snapshot) {
        result.state = ConfigAdmissionState::ConfigInvalidWithoutLastGood;
    }
    return result;
}

} // namespace tgcli::daemon
