#pragma once

#include "common/config.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace tgcli::daemon {

enum class ConfigAdmissionState {
    Ready,
    ImplicitMain,
    AccountMissing,
    ConfigInvalidWithLastGood,
    ConfigInvalidWithoutLastGood,
};

struct ReloadDiagnostic {
    config::ConfigReason reason;
};

struct AdmittedAccountSettings {
    bool allow_write = false;
    std::optional<std::chrono::seconds> idle_exit;
};

// A request receives only account-local, non-secret settings. The complete
// multi-account snapshot and credential/hook sources stay inside ConfigRuntime.
struct AdmittedAccountConfig {
    ConfigAdmissionState state = ConfigAdmissionState::Ready;
    std::string account;
    AdmittedAccountSettings settings;
    std::string snapshot_identity;
    std::uint64_t generation = 0;
    bool is_default = false;
    bool standing_write_grants_valid = true;
};

struct ConfigAdmissionDenied {
    ConfigAdmissionState state = ConfigAdmissionState::AccountMissing;
    std::string account;
    std::optional<std::string> snapshot_identity;
    std::uint64_t generation = 0;
    std::optional<ReloadDiagnostic> reload_diagnostic;
    std::optional<AdmittedAccountSettings> last_good_settings;
    bool last_good_account_present = false;
};

using ConfigAdmissionDecision =
    std::variant<std::shared_ptr<const AdmittedAccountConfig>, ConfigAdmissionDenied>;

enum class ConfigRefreshStatus { Completed, TimedOut, Cancelled, Stopped };

struct ConfigAdmissionResult {
    ConfigRefreshStatus refresh_status = ConfigRefreshStatus::Stopped;
    std::optional<ConfigAdmissionDecision> decision;
};

struct ConfigRuntimeSnapshot {
    ConfigAdmissionState state = ConfigAdmissionState::AccountMissing;
    std::string account;
    std::optional<std::chrono::seconds> idle_exit;
    std::optional<std::string> snapshot_identity;
    std::uint64_t generation = 0;
    std::optional<ReloadDiagnostic> reload_diagnostic;
};

namespace testing {

struct ConfigRuntimeHooks {
    using Clock = std::chrono::steady_clock;
    using Predicate = std::function<bool()>;
    using WaitUntil = std::function<void(std::condition_variable&, std::unique_lock<std::mutex>&,
                                         Clock::time_point, const Predicate&)>;

    std::function<Clock::time_point()> now;
    WaitUntil wait_until;
    std::function<void(bool forced)> before_reload;
    std::function<void(bool forced)> after_reload;
};

} // namespace testing

class ConfigRuntime {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kPollInterval = std::chrono::seconds(1);

    explicit ConfigRuntime(std::string config_path,
                           std::shared_ptr<const testing::ConfigRuntimeHooks> hooks = {},
                           uid_t expected_uid = static_cast<uid_t>(-1));
    ~ConfigRuntime();

    ConfigRuntime(const ConfigRuntime&) = delete;
    ConfigRuntime& operator=(const ConfigRuntime&) = delete;
    ConfigRuntime(ConfigRuntime&&) = delete;
    ConfigRuntime& operator=(ConfigRuntime&&) = delete;

    [[nodiscard]] ConfigAdmissionResult admit(std::string_view account,
                                              Clock::time_point deadline = Clock::time_point::max(),
                                              const std::stop_token& cancellation = {});
    [[nodiscard]] ConfigRuntimeSnapshot current(std::string_view account) const;

  private:
    struct RuntimePublication {
        std::shared_ptr<const config::PublishedSnapshot> snapshot;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] Clock::time_point now() const;
    void wait_for_work(std::unique_lock<std::mutex>& lock, Clock::time_point deadline,
                       const testing::ConfigRuntimeHooks::Predicate& predicate);
    void run(std::stop_token stop);
    [[nodiscard]] ConfigAdmissionDecision admission_decision(std::string_view account) const;
    [[nodiscard]] ConfigRuntimeSnapshot current_locked(std::string_view account) const;

    config::Store store_;
    config::SnapshotManager snapshots_;
    std::shared_ptr<const testing::ConfigRuntimeHooks> hooks_;
    std::atomic<std::shared_ptr<const RuntimePublication>> publication_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_ = false;
    std::uint64_t requested_refresh_ = 0;
    std::uint64_t completed_refresh_ = 0;
    std::uint64_t generation_ = 1;
    Clock::time_point next_poll_;
    std::jthread worker_;
};

} // namespace tgcli::daemon
