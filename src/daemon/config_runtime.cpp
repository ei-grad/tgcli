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

ConfigAdmissionResult ConfigRuntime::admit(std::string_view account, Clock::time_point deadline,
                                           const std::stop_token& cancellation) {
    std::unique_lock lock(mutex_);
    if (stopped_) {
        return {ConfigRefreshStatus::Stopped, std::nullopt};
    }
    if (cancellation.stop_requested()) {
        return {ConfigRefreshStatus::Cancelled, std::nullopt};
    }
    if (Clock::now() >= deadline) {
        return {ConfigRefreshStatus::TimedOut, std::nullopt};
    }

    const auto refresh = ++requested_refresh_;
    condition_.notify_all();
    const std::stop_callback cancellation_wakeup(cancellation, [this] { condition_.notify_all(); });

    while (completed_refresh_ < refresh) {
        if (stopped_) {
            return {ConfigRefreshStatus::Stopped, std::nullopt};
        }
        if (cancellation.stop_requested()) {
            return {ConfigRefreshStatus::Cancelled, std::nullopt};
        }
        if (Clock::now() >= deadline) {
            return {ConfigRefreshStatus::TimedOut, std::nullopt};
        }
        if (deadline == Clock::time_point::max()) {
            condition_.wait(lock);
        } else {
            condition_.wait_until(lock, deadline);
        }
    }

    if (cancellation.stop_requested()) {
        return {ConfigRefreshStatus::Cancelled, std::nullopt};
    }
    if (Clock::now() >= deadline) {
        return {ConfigRefreshStatus::TimedOut, std::nullopt};
    }
    if (stopped_) {
        return {ConfigRefreshStatus::Stopped, std::nullopt};
    }
    return {ConfigRefreshStatus::Completed, admission_decision(account)};
}

ConfigRuntimeSnapshot ConfigRuntime::current(std::string_view account) const {
    return current_locked(account);
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

void ConfigRuntime::run(std::stop_token stop) {
    const std::stop_callback stop_wakeup(stop, [this] { condition_.notify_all(); });
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested() && !stopped_) {
        const auto current_time = now();
        const bool forced = requested_refresh_ > completed_refresh_;
        const bool poll_due = current_time >= next_poll_;
        if (!forced && !poll_due) {
            wait_for_work(lock, next_poll_, [&] {
                return stop.stop_requested() || stopped_ || requested_refresh_ > completed_refresh_;
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
        static_cast<void>(snapshots_.reload());
        const auto published = snapshots_.current();
        lock.lock();

        ++generation_;
        publication_.store(std::make_shared<const RuntimePublication>(
            RuntimePublication{.snapshot = published, .generation = generation_}));
        if (forced) {
            completed_refresh_ = std::max(completed_refresh_, refresh_target);
        }
        condition_.notify_all();
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
    if (published && published->error.has_value()) {
        const auto reason = published->error.value_or(config::ConfigError{}).reason;
        return ConfigAdmissionDenied{
            .state = published->snapshot ? ConfigAdmissionState::ConfigInvalidWithLastGood
                                         : ConfigAdmissionState::ConfigInvalidWithoutLastGood,
            .account = std::string(account),
            .snapshot_identity = published->snapshot
                                     ? std::optional<std::string>(published->snapshot->identity)
                                     : std::nullopt,
            .generation = generation,
            .reload_diagnostic = ReloadDiagnostic{reason},
        };
    }
    if (!published || !published->snapshot) {
        return ConfigAdmissionDenied{
            .state = ConfigAdmissionState::ConfigInvalidWithoutLastGood,
            .account = std::string(account),
            .snapshot_identity = std::nullopt,
            .generation = generation,
            .reload_diagnostic = std::nullopt,
        };
    }

    const auto& snapshot = *published->snapshot;
    if (const auto selected = snapshot.accounts.find(account);
        selected != snapshot.accounts.end()) {
        return std::make_shared<const AdmittedAccountConfig>(AdmittedAccountConfig{
            .state = ConfigAdmissionState::Ready,
            .account = std::string(account),
            .settings =
                {
                    .allow_write = selected->second.allow_write,
                    .idle_exit = selected->second.idle_exit,
                },
            .snapshot_identity = snapshot.identity,
            .generation = generation,
            .is_default = snapshot.default_account == account,
            .standing_write_grants_valid = published->standing_write_grants_valid,
        });
    }
    if (snapshot.accounts.empty() && account == "main") {
        return std::make_shared<const AdmittedAccountConfig>(AdmittedAccountConfig{
            .state = ConfigAdmissionState::ImplicitMain,
            .account = "main",
            .settings = {},
            .snapshot_identity = snapshot.identity,
            .generation = generation,
            .is_default = true,
            .standing_write_grants_valid = published->standing_write_grants_valid,
        });
    }
    return ConfigAdmissionDenied{
        .state = ConfigAdmissionState::AccountMissing,
        .account = std::string(account),
        .snapshot_identity = snapshot.identity,
        .generation = generation,
        .reload_diagnostic = std::nullopt,
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
