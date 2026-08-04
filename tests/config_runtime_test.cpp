#include "daemon/config_runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using tgcli::daemon::AdmittedAccountSettings;
using tgcli::daemon::ConfigAdmissionDenied;
using tgcli::daemon::ConfigAdmissionResult;
using tgcli::daemon::ConfigAdmissionState;
using tgcli::daemon::ConfigRefreshStatus;
using tgcli::daemon::ConfigRuntime;

namespace {

class TempConfig {
  public:
    TempConfig() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-runtime-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        parent_ = root_ / "xdg";
        directory_ = parent_ / "tgcli";
        REQUIRE(std::filesystem::create_directory(parent_));
        REQUIRE(::chmod(parent_.c_str(), 0700) == 0);
        REQUIRE(std::filesystem::create_directory(directory_));
        REQUIRE(::chmod(directory_.c_str(), 0700) == 0);
    }

    ~TempConfig() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempConfig(const TempConfig&) = delete;
    TempConfig& operator=(const TempConfig&) = delete;
    TempConfig(TempConfig&&) = delete;
    TempConfig& operator=(TempConfig&&) = delete;

    [[nodiscard]] std::string file() const {
        return (directory_ / "config.toml").string();
    }

    [[nodiscard]] const std::filesystem::path& dir() const {
        return directory_;
    }

    void write_initial(std::string_view bytes) const {
        write_file(file(), bytes);
    }

    void replace(std::string_view bytes) const {
        const auto replacement = directory_ / "runtime-replacement.toml";
        write_file(replacement, bytes);
        REQUIRE(::rename(replacement.c_str(), file().c_str()) == 0);
    }

  private:
    static void write_file(const std::filesystem::path& file, std::string_view bytes) {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(file.c_str(), 0600) == 0);
    }

    std::filesystem::path root_;
    std::filesystem::path parent_;
    std::filesystem::path directory_;
};

class HeldTransaction {
  public:
    explicit HeldTransaction(const TempConfig& temp)
        : marker_(temp.dir() / ".config.toml.transaction") {
        const auto lock_file = temp.dir() / "config.lock";
        lock_fd_ = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        REQUIRE(lock_fd_ >= 0);
        REQUIRE(::fchmod(lock_fd_, 0600) == 0);
        REQUIRE(::flock(lock_fd_, LOCK_EX | LOCK_NB) == 0);
        std::ofstream marker(marker_, std::ios::binary | std::ios::trunc);
        REQUIRE(marker.good());
        marker << "pending-present\n"
               << std::string(64, '0') << '\n'
               << std::string(64, '1') << '\n';
        marker.close();
        REQUIRE(::chmod(marker_.c_str(), 0600) == 0);
    }

    ~HeldTransaction() {
        std::error_code ignored;
        std::filesystem::remove(marker_, ignored);
        if (lock_fd_ >= 0) {
            static_cast<void>(::flock(lock_fd_, LOCK_UN));
            static_cast<void>(::close(lock_fd_));
        }
    }

    HeldTransaction(const HeldTransaction&) = delete;
    HeldTransaction& operator=(const HeldTransaction&) = delete;
    HeldTransaction(HeldTransaction&&) = delete;
    HeldTransaction& operator=(HeldTransaction&&) = delete;

  private:
    std::filesystem::path marker_;
    int lock_fd_ = -1;
};

std::string two_accounts(std::string_view default_account = "main",
                         std::string_view main_idle = "5", std::string_view work_idle = "9") {
    return "default_account = \"" + std::string(default_account) +
           "\"\n[accounts.main]\n"
           "api_id = 12345\n"
           "api_hash = \"application-hash\"\n"
           "password_cmd = \"secret-provider\"\n"
           "allow_write = true\n"
           "idle_exit = " +
           std::string(main_idle) +
           "\n[accounts.work]\n"
           "api_id_cmd = \"id-provider\"\n"
           "api_hash_cmd = \"hash-provider\"\n"
           "bot_token_cmd = \"bot-provider\"\n"
           "allow_write = false\n"
           "idle_exit = " +
           std::string(work_idle) + "\n";
}

std::string one_account(std::string_view account, bool allow_write, std::string_view idle_exit) {
    return "default_account = \"" + std::string(account) + "\"\n[accounts." + std::string(account) +
           "]\nallow_write = " + (allow_write ? "true" : "false") +
           "\nidle_exit = " + std::string(idle_exit) + "\n";
}

const tgcli::daemon::AdmittedAccountConfig& admitted(const ConfigAdmissionResult& result) {
    REQUIRE(result.refresh_status == ConfigRefreshStatus::Completed);
    REQUIRE(result.decision);
    REQUIRE(std::holds_alternative<std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig>>(
        *result.decision));
    const auto& account =
        std::get<std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig>>(*result.decision);
    REQUIRE(account);
    return *account;
}

const ConfigAdmissionDenied& denied(const ConfigAdmissionResult& result) {
    REQUIRE(result.refresh_status == ConfigRefreshStatus::Completed);
    REQUIRE(result.decision);
    REQUIRE(std::holds_alternative<ConfigAdmissionDenied>(*result.decision));
    return std::get<ConfigAdmissionDenied>(*result.decision);
}

template <typename Value>
concept ExposesCredential = requires(Value value) {
    value.api_hash;
    value.password_cmd;
};

template <typename Value>
concept ExposesAdmittedSettings = requires(Value value) { value.settings; };

class ManualRuntimeClock {
  public:
    using Clock = ConfigRuntime::Clock;

    [[nodiscard]] Clock::time_point now() const {
        return Clock::time_point(std::chrono::nanoseconds(ticks_.load(std::memory_order_relaxed)));
    }

    void install(tgcli::daemon::testing::ConfigRuntimeHooks& hooks) {
        hooks.now = [this] { return now(); };
        hooks.wait_until =
            [this](std::condition_variable& condition, std::unique_lock<std::mutex>& lock,
                   Clock::time_point deadline,
                   const tgcli::daemon::testing::ConfigRuntimeHooks::Predicate& predicate) {
                condition_.store(&condition, std::memory_order_release);
                condition.wait(lock, [&] { return predicate() || now() >= deadline; });
            };
    }

    void advance(std::chrono::nanoseconds amount) {
        ticks_.fetch_add(amount.count(), std::memory_order_relaxed);
        if (auto* condition = condition_.load(std::memory_order_acquire)) {
            condition->notify_all();
        }
    }

  private:
    std::atomic<std::int64_t> ticks_ = 0;
    std::atomic<std::condition_variable*> condition_ = nullptr;
};

class ReloadGate {
  public:
    void wait(bool forced) {
        if (!forced) {
            return;
        }
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return entered_; });
    }

    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

} // namespace

static_assert(!ExposesCredential<AdmittedAccountSettings>);
static_assert(!ExposesAdmittedSettings<ConfigAdmissionDenied>);

TEST_CASE("config runtime admits only immutable account-local safe settings",
          "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial(two_accounts());
    ConfigRuntime runtime(temp.file());

    const auto main_result = runtime.admit("main");
    const auto& main = admitted(main_result);
    CHECK(main.state == ConfigAdmissionState::Ready);
    CHECK(main.account == "main");
    CHECK(main.is_default);
    CHECK(main.standing_write_grants_valid);
    CHECK(main.settings.allow_write);
    CHECK(main.settings.idle_exit == 5s);

    temp.replace(two_accounts("work", "7", "11"));
    const auto later_result = runtime.admit("main");
    const auto& later = admitted(later_result);
    CHECK(later.state == ConfigAdmissionState::Ready);
    CHECK_FALSE(later.is_default);
    CHECK(later.settings.idle_exit == 7s);
    CHECK(later.generation > main.generation);

    CHECK(main.is_default);
    CHECK(main.settings.idle_exit == 5s);

    const auto work_result = runtime.admit("work");
    const auto& work = admitted(work_result);
    CHECK(work.state == ConfigAdmissionState::Ready);
    CHECK(work.is_default);
    CHECK_FALSE(work.settings.allow_write);
    CHECK(work.settings.idle_exit == 11s);
}

TEST_CASE("config runtime classifies implicit main and missing accounts",
          "[daemon][config-runtime]") {
    const TempConfig temp;
    ConfigRuntime runtime(temp.file());

    const auto main_result = runtime.admit("main");
    const auto& main = admitted(main_result);
    CHECK(main.state == ConfigAdmissionState::ImplicitMain);
    CHECK(main.snapshot_identity == "missing");
    CHECK(main.is_default);
    CHECK(main.standing_write_grants_valid);
    CHECK_FALSE(main.settings.allow_write);
    CHECK_FALSE(main.settings.idle_exit);

    const auto work_result = runtime.admit("work");
    const auto& work = denied(work_result);
    CHECK(work.state == ConfigAdmissionState::AccountMissing);
    CHECK_FALSE(std::holds_alternative<std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig>>(
        *work_result.decision));
}

TEST_CASE("invalid reload denies admission while retaining last-good non-safety settings",
          "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", true, "5"));
    ConfigRuntime runtime(temp.file());

    const auto old_result = runtime.admit("main");
    static_cast<void>(admitted(old_result));
    const auto old =
        std::get<std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig>>(*old_result.decision);
    CHECK(old->state == ConfigAdmissionState::Ready);

    temp.replace("[accounts.main\n");
    const auto invalid_result = runtime.admit("main");
    const auto& invalid = denied(invalid_result);
    CHECK(invalid.state == ConfigAdmissionState::ConfigInvalidWithLastGood);
    CHECK_FALSE(std::holds_alternative<std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig>>(
        *invalid_result.decision));
    REQUIRE(invalid.reload_diagnostic);
    CHECK(invalid.reload_diagnostic->reason == tgcli::config::ConfigReason::ParseError);

    const auto policy = runtime.current("main");
    CHECK(policy.state == ConfigAdmissionState::ConfigInvalidWithLastGood);
    CHECK(policy.idle_exit == 5s);

    CHECK(old->state == ConfigAdmissionState::Ready);
    CHECK(old->standing_write_grants_valid);

    temp.replace(one_account("main", false, "13"));
    const auto recovered_result = runtime.admit("main");
    const auto& recovered = admitted(recovered_result);
    CHECK(recovered.state == ConfigAdmissionState::Ready);
    CHECK(recovered.standing_write_grants_valid);
    CHECK_FALSE(recovered.settings.allow_write);
    CHECK(recovered.settings.idle_exit == 13s);
}

TEST_CASE("invalid initial config has no last-good admission", "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial("[accounts.main\n");
    ConfigRuntime runtime(temp.file());

    const auto result = runtime.admit("main");
    const auto& admission = denied(result);
    CHECK(admission.state == ConfigAdmissionState::ConfigInvalidWithoutLastGood);
    CHECK_FALSE(admission.snapshot_identity);
    REQUIRE(admission.reload_diagnostic);
    CHECK(admission.reload_diagnostic->reason == tgcli::config::ConfigReason::ParseError);
}

TEST_CASE("account removal is visible only to later forced admissions",
          "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial(two_accounts());
    ConfigRuntime runtime(temp.file());
    const auto old_result = runtime.admit("main");
    static_cast<void>(admitted(old_result));
    const auto old =
        std::get<std::shared_ptr<const tgcli::daemon::AdmittedAccountConfig>>(*old_result.decision);

    temp.replace(one_account("work", false, "12"));
    const auto removed_result = runtime.admit("main");
    const auto& removed = denied(removed_result);
    CHECK(removed.state == ConfigAdmissionState::AccountMissing);

    CHECK(old->state == ConfigAdmissionState::Ready);
    CHECK(old->settings.allow_write);
}

TEST_CASE("forced admission reload does not wait for the background poll",
          "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", false, "1"));
    ManualRuntimeClock clock;
    auto hooks = std::make_shared<tgcli::daemon::testing::ConfigRuntimeHooks>();
    clock.install(*hooks);
    ConfigRuntime runtime(temp.file(), hooks);
    const auto initial = runtime.current("main");
    CHECK(initial.idle_exit == 1s);

    temp.replace(one_account("main", false, "17"));
    const auto result = runtime.admit("main");
    const auto& admission = admitted(result);
    CHECK(admission.generation > initial.generation);
    CHECK(admission.settings.idle_exit == 17s);
}

TEST_CASE("runtime snapshot and admission change at one publication boundary",
          "[daemon][config-runtime][race]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", false, "5"));
    ReloadGate gate;
    auto hooks = std::make_shared<tgcli::daemon::testing::ConfigRuntimeHooks>();
    hooks->before_reload = [&gate](bool forced) { gate.wait(forced); };
    ConfigRuntime runtime(temp.file(), hooks);
    const auto initial = runtime.current("main");
    CHECK(initial.idle_exit == 5s);

    temp.replace(one_account("main", false, "11"));
    ConfigAdmissionResult result;
    std::thread caller([&] { result = runtime.admit("main"); });
    gate.wait_until_entered();
    const auto while_reloading = runtime.current("main");
    CHECK(while_reloading.generation == initial.generation);
    CHECK(while_reloading.idle_exit == 5s);

    gate.release();
    caller.join();
    const auto& account = admitted(result);
    CHECK(account.generation > initial.generation);
    CHECK(account.settings.idle_exit == 11s);
}

TEST_CASE("forced admission honors deadline and cancellation", "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", false, "5"));

    SECTION("deadline") {
        ReloadGate gate;
        auto hooks = std::make_shared<tgcli::daemon::testing::ConfigRuntimeHooks>();
        hooks->before_reload = [&gate](bool forced) { gate.wait(forced); };
        ConfigRuntime runtime(temp.file(), hooks);
        const auto result = runtime.admit("main", ConfigRuntime::Clock::now() + 40ms);
        CHECK(result.refresh_status == ConfigRefreshStatus::TimedOut);
        CHECK_FALSE(result.decision);
        gate.release();
    }

    SECTION("cancellation") {
        ReloadGate gate;
        auto hooks = std::make_shared<tgcli::daemon::testing::ConfigRuntimeHooks>();
        hooks->before_reload = [&gate](bool forced) { gate.wait(forced); };
        ConfigRuntime runtime(temp.file(), hooks);
        std::stop_source stop;
        ConfigAdmissionResult result;
        std::thread caller([&] {
            result =
                runtime.admit("main", ConfigRuntime::Clock::time_point::max(), stop.get_token());
        });
        gate.wait_until_entered();
        stop.request_stop();
        caller.join();
        CHECK(result.refresh_status == ConfigRefreshStatus::Cancelled);
        CHECK_FALSE(result.decision);
        gate.release();
    }
}

TEST_CASE("background config poll observes an atomic replacement within two seconds",
          "[daemon][config-runtime][integration]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", false, "1"));
    auto hooks = std::make_shared<tgcli::daemon::testing::ConfigRuntimeHooks>();
    std::mutex mutex;
    std::condition_variable condition;
    int polls = 0;
    hooks->after_reload = [&](bool forced) {
        if (!forced) {
            const std::lock_guard lock(mutex);
            ++polls;
            condition.notify_all();
        }
    };
    const ConfigRuntime runtime(temp.file(), hooks);
    const auto initial = runtime.current("main");
    CHECK(initial.state == ConfigAdmissionState::Ready);
    CHECK(initial.idle_exit == 1s);

    temp.replace(one_account("main", false, "19"));
    const auto replaced_at = ConfigRuntime::Clock::now();
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 1900ms, [&] { return polls != 0; }));
    }
    CHECK(ConfigRuntime::Clock::now() - replaced_at < 2s);
    const auto observed = runtime.current("main");
    CHECK(observed.state == ConfigAdmissionState::Ready);
    CHECK(observed.generation > initial.generation);
    CHECK(observed.idle_exit == 19s);
}

TEST_CASE("background config poll rejects invalid replacement within two seconds",
          "[daemon][config-runtime][integration]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", true, "23"));
    auto hooks = std::make_shared<tgcli::daemon::testing::ConfigRuntimeHooks>();
    std::mutex mutex;
    std::condition_variable condition;
    int polls = 0;
    hooks->after_reload = [&](bool forced) {
        if (!forced) {
            const std::lock_guard lock(mutex);
            ++polls;
            condition.notify_all();
        }
    };
    const ConfigRuntime runtime(temp.file(), hooks);
    const auto initial = runtime.current("main");
    CHECK(initial.state == ConfigAdmissionState::Ready);
    CHECK(initial.idle_exit == 23s);

    temp.replace("[accounts.main\n");
    const auto replaced_at = ConfigRuntime::Clock::now();
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 1900ms, [&] { return polls != 0; }));
    }
    CHECK(ConfigRuntime::Clock::now() - replaced_at < 2s);
    const auto observed = runtime.current("main");
    CHECK(observed.state == ConfigAdmissionState::ConfigInvalidWithLastGood);
    CHECK(observed.generation > initial.generation);
    CHECK(observed.idle_exit == 23s);
    REQUIRE(observed.reload_diagnostic);
    CHECK(observed.reload_diagnostic->reason == tgcli::config::ConfigReason::ParseError);
}

TEST_CASE("config runtime stop interrupts the one-second condition wait",
          "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", false, "5"));
    const auto started = ConfigRuntime::Clock::now();
    { const ConfigRuntime runtime(temp.file()); }
    CHECK(ConfigRuntime::Clock::now() - started < 250ms);
}

TEST_CASE("config runtime stop interrupts a reload blocked by an active transaction",
          "[daemon][config-runtime]") {
    const TempConfig temp;
    temp.write_initial(one_account("main", false, "5"));
    std::mutex mutex;
    std::condition_variable condition;
    bool armed = false;
    bool reload_started = false;
    auto hooks = std::make_shared<tgcli::daemon::testing::ConfigRuntimeHooks>();
    hooks->before_reload = [&](bool) {
        const std::lock_guard lock(mutex);
        if (armed) {
            reload_started = true;
            condition.notify_all();
        }
    };
    auto runtime = std::make_unique<ConfigRuntime>(temp.file(), hooks);
    const HeldTransaction transaction(temp);
    {
        const std::lock_guard lock(mutex);
        armed = true;
    }
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 1900ms, [&] { return reload_started; }));
    }
    std::this_thread::sleep_for(20ms);

    const auto started = ConfigRuntime::Clock::now();
    runtime.reset();
    CHECK(ConfigRuntime::Clock::now() - started < 500ms);
}
