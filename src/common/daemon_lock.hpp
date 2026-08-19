#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

namespace tgcli::daemon_lock {

inline constexpr std::string_view kIdentityRecordTag = "tgcli-lock-v1";
inline constexpr std::size_t kControlTokenHexLength = 32;

enum class OwnerStatus { Held, Released, Transition, Invalid };

struct Identity {
    pid_t pid = -1;
    std::string process_start;
    std::string control_token;

    friend bool operator==(const Identity&, const Identity&) = default;
};

class LifetimeLease final {
  public:
    LifetimeLease(const LifetimeLease&) = delete;
    LifetimeLease& operator=(const LifetimeLease&) = delete;
    LifetimeLease(LifetimeLease&&) = delete;
    LifetimeLease& operator=(LifetimeLease&&) = delete;
    ~LifetimeLease();

    [[nodiscard]] const std::string& path() const {
        return path_;
    }
    [[nodiscard]] const Identity& identity() const {
        return identity_;
    }
    [[nodiscard]] std::uint64_t device() const {
        return device_;
    }
    [[nodiscard]] std::uint64_t inode() const {
        return inode_;
    }
    bool validate(uid_t expected_uid, std::string& error) const;

  private:
    friend std::shared_ptr<LifetimeLease> acquire_lifetime(const std::string& lock_path,
                                                           Identity& identity, std::string& error);
    LifetimeLease(int fd, int parent_fd, std::string path, std::string parent_path,
                  std::string basename, Identity identity, std::uint64_t device,
                  std::uint64_t inode, std::uint64_t parent_device, std::uint64_t parent_inode)
        : fd_(fd), parent_fd_(parent_fd), path_(std::move(path)),
          parent_path_(std::move(parent_path)), basename_(std::move(basename)),
          identity_(std::move(identity)), device_(device), inode_(inode),
          parent_device_(parent_device), parent_inode_(parent_inode) {}

    int fd_ = -1;
    int parent_fd_ = -1;
    std::string path_;
    std::string parent_path_;
    std::string basename_;
    Identity identity_;
    std::uint64_t device_ = 0;
    std::uint64_t inode_ = 0;
    std::uint64_t parent_device_ = 0;
    std::uint64_t parent_inode_ = 0;
};

namespace detail {

enum class ObservationStatus { Stable, Transition, Invalid };
enum class AcquireStage { BootstrapLocked, OwnerLocked, RecordTruncated, RecordPublished };
using AcquireObserver = void (*)(AcquireStage stage, void* context);

struct AcquireHooks {
    AcquireObserver observer = nullptr;
    void* context = nullptr;
};

// Pure decision seam for the two kernel-owner samples and the record/live
// snapshots gathered between them.
ObservationStatus classify_owner_observation(pid_t initial_owner, bool final_held,
                                             pid_t final_owner,
                                             std::optional<std::string_view> initial_record,
                                             std::optional<std::string_view> final_record,
                                             const Identity* parsed_identity,
                                             std::optional<std::string_view> live_process_start,
                                             std::string& error);

} // namespace detail

// Keeps the exact lock-file inode used to verify an owner open so
// shutdown polling cannot be redirected through a replaced path.
class OwnerWatch {
  public:
    OwnerWatch(const OwnerWatch&) = delete;
    OwnerWatch& operator=(const OwnerWatch&) = delete;
    OwnerWatch(OwnerWatch&& other) noexcept;
    OwnerWatch& operator=(OwnerWatch&& other) noexcept;
    ~OwnerWatch();

    [[nodiscard]] const Identity& identity() const {
        return identity_;
    }

    [[nodiscard]] pid_t observed_pid() const {
        return observed_pid_;
    }

    // `released` becomes true when the verified owner no longer holds this
    // lock inode. A different, valid owner also means the old owner released
    // it; spawn races are resolved by the normal connection path.
    bool owner_released(bool& released, std::string& error) const;

  private:
    friend std::optional<OwnerWatch> verify_owner(const std::string& lock_path, uid_t expected_uid,
                                                  std::string& error);
    friend OwnerStatus inspect_owner(const std::string& lock_path, uid_t expected_uid,
                                     std::optional<OwnerWatch>& owner, std::string& error);
    OwnerWatch(int fd, Identity identity, pid_t observed_pid)
        : fd_(fd), identity_(std::move(identity)), observed_pid_(observed_pid) {}

    int fd_ = -1;
    Identity identity_;
    pid_t observed_pid_ = -1;
};

// Acquires the per-account lifetime lock, generates a control token and writes
// a strict identity record. The returned fd must remain open for the owner's
// lifetime.
int acquire(const std::string& lock_path, Identity& identity, std::string& error,
            const detail::AcquireHooks* hooks = nullptr);

// Typed lifetime owner for consumers that must retain proof of the current
// process's record lock without exposing an unlock-capable descriptor.
std::shared_ptr<LifetimeLease> acquire_lifetime(const std::string& lock_path, Identity& identity,
                                                std::string& error);

// Frozen bootstrap-record parser. Kept public so compatibility fixtures can
// pin the record independently from the main JSONL protocol.
bool parse_identity_record(std::string_view record, Identity& identity, std::string& error);

// Validates the kernel record-lock owner field, including PID 1.
bool owner_pid_matches(pid_t record_pid, pid_t kernel_pid, std::string& error);

// Verifies the kernel-reported record-lock owner against the on-disk identity
// and the live process instance. The returned token addresses the daemon's
// version-independent control socket; no numeric-PID signal is used.
std::optional<OwnerWatch> verify_owner(const std::string& lock_path, uid_t expected_uid,
                                       std::string& error);

// Distinguishes a verified live owner from an already released lock and an
// invalid bootstrap surface, allowing concurrent restart clients to converge.
OwnerStatus inspect_owner(const std::string& lock_path, uid_t expected_uid,
                          std::optional<OwnerWatch>& owner, std::string& error);

} // namespace tgcli::daemon_lock
