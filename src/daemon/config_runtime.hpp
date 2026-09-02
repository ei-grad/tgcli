#pragma once

#include "common/cancellation.hpp"
#include "common/config.hpp"
#include "common/deadline.hpp"
#include "common/shared_publication.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

// A request receives only the selected account, including its credential and
// hook sources. Other accounts and the raw multi-account config stay inside
// ConfigRuntime.
struct AdmittedAccountConfig {
    ConfigAdmissionState state = ConfigAdmissionState::Ready;
    std::string account;
    AdmittedAccountSettings settings;
    std::shared_ptr<const config::ConfigSnapshot> account_snapshot;
    std::string snapshot_identity;
    std::uint64_t generation = 0;
    bool is_default = false;
    bool standing_write_grants_valid = true;
    bool last_good_account_present = true;
    std::optional<ReloadDiagnostic> reload_diagnostic;
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
    std::function<void(const RequestDeadline&)> admission_deadline;
    std::function<void(ConfigRefreshStatus)> admission_finished;
    std::function<void(bool forced)> before_reload;
    std::function<void(bool forced)> after_reload;
};

} // namespace testing

class ConfigRuntime {
  public:
    using Clock = std::chrono::steady_clock;
    using PublicationObserver = std::function<void()>;
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
                                              const RequestDeadline& deadline = {},
                                              const cancellation::Token& cancellation = {});
    [[nodiscard]] ConfigRuntimeSnapshot current(std::string_view account) const;
    // Publication callbacks are serialized on the runtime worker. A callback
    // failure detaches that observer; replacement from inside a callback is
    // queued for delivery after the current callback returns.
    void set_publication_observer(PublicationObserver observer);
    [[nodiscard]] const std::string& config_path() const {
        return store_.path();
    }

  private:
    struct RuntimePublication {
        std::shared_ptr<const config::PublishedSnapshot> snapshot;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] Clock::time_point now() const;
    void wait_for_work(std::unique_lock<std::mutex>& lock, Clock::time_point deadline,
                       const testing::ConfigRuntimeHooks::Predicate& predicate,
                       const cancellation::Token& cancellation);
    void dispatch_publication_observer(std::unique_lock<std::mutex>& lock);
    void run(cancellation::Token stop);
    [[nodiscard]] ConfigAdmissionDecision admission_decision(std::string_view account) const;
    [[nodiscard]] ConfigRuntimeSnapshot current_locked(std::string_view account) const;

    config::Store store_;
    config::SnapshotManager snapshots_;
    std::shared_ptr<const testing::ConfigRuntimeHooks> hooks_;
    SharedPublication<const RuntimePublication> publication_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_ = false;
    PublicationObserver publication_observer_;
    bool publication_notification_pending_ = false;
    std::size_t publication_callbacks_ = 0;
    std::uint64_t publication_observer_revision_ = 0;
    std::thread::id publication_callback_thread_;
    std::uint64_t requested_refresh_ = 0;
    std::uint64_t completed_refresh_ = 0;
    std::uint64_t generation_ = 1;
    Clock::time_point next_poll_;
    cancellation::Thread worker_;
};

} // namespace tgcli::daemon
