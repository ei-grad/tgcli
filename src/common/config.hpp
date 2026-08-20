#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

namespace tgcli::config {

namespace testing {
struct StoreHooks;
}

inline constexpr std::size_t kMaxConfigBytes = std::size_t{1024} * 1024;

enum class ConfigReason {
    PathInvalid,
    WrongOwner,
    WrongType,
    WrongMode,
    WrongLinkCount,
    TooLarge,
    ParseError,
    TypeError,
    InvalidDefault,
    ConflictingCredentials,
    IoError,
    SyncError,
};

std::string_view reason_name(ConfigReason reason);

struct ConfigError {
    ConfigReason reason = ConfigReason::IoError;
    std::string message;
};

struct AccountConfig {
    std::optional<std::int32_t> api_id;
    std::optional<std::string> api_hash;
    std::optional<std::string> api_id_cmd;
    std::optional<std::string> api_hash_cmd;
    std::optional<std::string> db_key_cmd;
    std::optional<std::string> password_cmd;
    std::optional<std::string> bot_token_cmd;
    bool allow_write = false;
    std::optional<std::chrono::seconds> idle_exit;
};

struct ConfigSnapshot {
    std::string identity;
    std::optional<std::string> default_account;
    std::map<std::string, AccountConfig, std::less<>> accounts;
    std::string raw_bytes;
};

struct LoadResult {
    std::shared_ptr<const ConfigSnapshot> snapshot;
    std::optional<ConfigError> error;
    bool timed_out = false;
    bool cancelled = false;

    explicit operator bool() const {
        return snapshot != nullptr && !error && !timed_out && !cancelled;
    }
};

enum class MutationStatus {
    Applied,
    Conflict,
    Invalid,
    IoError,
    TimedOut,
    Cancelled,
    PreconditionFailed,
    DurabilityUnknown,
};

struct MutationControl {
    MutationControl() = default;
    MutationControl(std::optional<std::chrono::steady_clock::time_point> deadline_value,
                    std::stop_token cancellation_value)
        : deadline(deadline_value), cancellation(std::move(cancellation_value)) {}

    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::stop_token cancellation;
    std::function<bool()> pre_commit;
    std::function<bool()> commit_admission;
    std::function<bool(const ConfigSnapshot&)> already_committed_admission;
    std::function<bool()> post_commit;
};

struct MutationResult {
    MutationStatus status = MutationStatus::IoError;
    std::shared_ptr<const ConfigSnapshot> snapshot;
    std::optional<ConfigError> error;
};

struct PromptedAppCredentials {
    std::optional<std::int32_t> api_id;
    std::optional<std::string> api_hash;
};

// Secure config store. Reads never create files. Every mutation holds the
// global lock and compares the complete serialized snapshot identity before
// replacing config.toml in the same directory.
class Store {
  public:
    explicit Store(std::string config_path, uid_t expected_uid = static_cast<uid_t>(-1));
    Store(std::string config_path, std::shared_ptr<const testing::StoreHooks> hooks,
          uid_t expected_uid = static_cast<uid_t>(-1));

    [[nodiscard]] LoadResult load(const MutationControl& control = {}) const;
    [[nodiscard]] MutationResult add_account(std::string_view expected_identity,
                                             std::string_view account,
                                             const MutationControl& control = {}) const;
    [[nodiscard]] MutationResult use_account(std::string_view expected_identity,
                                             std::string_view account,
                                             const MutationControl& control = {}) const;
    [[nodiscard]] MutationResult remove_account(std::string_view expected_identity,
                                                std::string_view account,
                                                std::optional<std::string_view> reassign_default,
                                                const MutationControl& control = {}) const;
    [[nodiscard]] MutationResult
    materialize_implicit_main(std::string_view expected_identity,
                              const PromptedAppCredentials& prompted,
                              const MutationControl& control = {}) const;
    [[nodiscard]] MutationResult replace_app_credentials(std::string_view expected_identity,
                                                         std::string_view account,
                                                         const PromptedAppCredentials& prompted,
                                                         const MutationControl& control = {}) const;

    [[nodiscard]] const std::string& path() const {
        return config_path_;
    }

    // Internal typed mutation representation is public only so the translation
    // unit can keep the policy transform separate from filesystem mechanics.
    enum class MutationKind { Add, Use, Remove, MaterializeMain, ReplaceAppCredentials };
    struct Mutation {
        MutationKind kind;
        std::string account;
        std::optional<std::string> reassign_default;
        PromptedAppCredentials prompted;
    };

  private:
    [[nodiscard]] MutationResult mutate(std::string_view expected_identity,
                                        const Mutation& mutation,
                                        const MutationControl& control) const;

    std::string config_path_;
    uid_t expected_uid_;
    std::shared_ptr<const testing::StoreHooks> hooks_;
};

enum class ReloadStatus { Published, Unchanged, Invalid, NotDue };

struct PublishedSnapshot {
    std::shared_ptr<const ConfigSnapshot> snapshot;
    bool standing_write_grants_valid = false;
    std::optional<ConfigError> error;
};

// Single-writer/lock-free-reader publication primitive. Invalid reloads retain
// the last-good immutable snapshot but invalidate standing write grants.
class SnapshotManager {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kPollInterval = std::chrono::seconds(1);
    static constexpr auto kLoadTimeout = std::chrono::seconds(2);

    explicit SnapshotManager(const Store& store);

    bool initialize(Clock::time_point now = Clock::now());
    bool initialize(Clock::time_point now, const MutationControl& control);
    ReloadStatus reload();
    ReloadStatus reload(const MutationControl& control);
    ReloadStatus poll(Clock::time_point now = Clock::now());
    ReloadStatus poll(Clock::time_point now, const MutationControl& control);

    [[nodiscard]] std::shared_ptr<const PublishedSnapshot> current() const;

  private:
    const Store& store_;
    mutable std::mutex reload_mutex_;
    std::atomic<std::shared_ptr<const PublishedSnapshot>> current_;
    Clock::time_point next_poll_;
};

} // namespace tgcli::config
