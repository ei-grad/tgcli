#include "daemon/removal_filesystem.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <linux/stat.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <utility>
#include <vector>

namespace tgcli::daemon {

namespace {

class Descriptor final {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                ::close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const {
        return value_;
    }
    explicit operator bool() const {
        return value_ >= 0;
    }

  private:
    int value_;
};

struct SplitPath {
    std::string parent;
    std::string name;
};

bool valid_invocation_id(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

std::optional<SplitPath> split_path(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    const std::filesystem::path candidate(value);
    if (!candidate.is_absolute() || candidate == candidate.root_path() ||
        candidate.lexically_normal().generic_string() != value) {
        return std::nullopt;
    }
    const auto name = candidate.filename().string();
    const auto parent = candidate.parent_path().string();
    if (name.empty() || name == "." || name == ".." || parent.empty()) {
        return std::nullopt;
    }
    return SplitPath{parent, name};
}

void notify(const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks,
            RemovalFilesystemStage stage, std::string_view path) {
    if (hooks && hooks->at_stage) {
        hooks->at_stage(stage, path);
    }
}

bool injected(const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks,
              RemovalFilesystemStage stage, std::string_view path) {
    return hooks && hooks->should_fail && hooks->should_fail(stage, path);
}

bool forced_device_boundary(const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks,
                            std::string_view path) {
    return hooks && hooks->force_device_boundary && hooks->force_device_boundary(path);
}

std::optional<std::uint64_t>
mount_identity(int descriptor,
               const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks) {
    if (hooks && hooks->mount_identity) {
        return hooks->mount_identity(descriptor);
    }
#if defined(__linux__) && defined(SYS_statx) && defined(STATX_MNT_ID) && defined(AT_EMPTY_PATH)
    struct statx metadata {};
    if (::syscall(SYS_statx, descriptor, "", AT_EMPTY_PATH, STATX_MNT_ID, &metadata) != 0 ||
        (metadata.stx_mask & STATX_MNT_ID) == 0) {
        return std::nullopt;
    }
    return metadata.stx_mnt_id;
#else
    // Device identity cannot distinguish bind mounts. Callers fail closed when no equivalent
    // per-mount identity is available on the host platform.
    static_cast<void>(descriptor);
    return std::nullopt;
#endif
}

bool same_mount(int parent, int child,
                const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks,
                RemovalFilesystemFailure& failure) {
    const auto parent_identity = mount_identity(parent, hooks);
    const auto child_identity = mount_identity(child, hooks);
    if (!parent_identity || !child_identity || *parent_identity != *child_identity) {
        failure.reason = "mount_boundary";
        return false;
    }
    return true;
}

bool valid_parent(int descriptor, uid_t expected_uid) {
    struct stat metadata {};
    return ::fstat(descriptor, &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
           metadata.st_uid == expected_uid && (metadata.st_mode & 07777) == 0700;
}

bool same_identity(const struct stat& metadata, const proto::RootIdentity& expected) {
    return static_cast<std::uint64_t>(metadata.st_dev) == expected.device &&
           static_cast<std::uint64_t>(metadata.st_ino) == expected.inode &&
           static_cast<std::uint64_t>(metadata.st_uid) == expected.owner;
}

bool absent_or_identity(int parent, const std::string& name,
                        const std::optional<proto::RootIdentity>& expected, bool& absent,
                        RemovalFilesystemFailure& failure,
                        const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks) {
    struct stat metadata {};
    if (::fstatat(parent, name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            absent = true;
            if (expected) {
                failure.reason = "path_changed";
                return false;
            }
            return true;
        }
        failure.reason = "io_error";
        return false;
    }
    absent = false;
    if (!expected || !S_ISDIR(metadata.st_mode) || metadata.st_uid != expected->owner ||
        !same_identity(metadata, *expected)) {
        failure.reason = "path_changed";
        return false;
    }
    const Descriptor opened(
        ::openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat opened_metadata {};
    struct stat parent_metadata {};
    if (!opened || ::fstat(opened.get(), &opened_metadata) != 0 ||
        ::fstat(parent, &parent_metadata) != 0 || !same_identity(opened_metadata, *expected) ||
        opened_metadata.st_dev != metadata.st_dev || opened_metadata.st_ino != metadata.st_ino) {
        failure.reason = "path_changed";
        return false;
    }
    if (metadata.st_dev != parent_metadata.st_dev ||
        !same_mount(parent, opened.get(), hooks, failure)) {
        failure.reason = "mount_boundary";
        return false;
    }
    return true;
}

bool rename_exclusive(int parent, const std::string& from, const std::string& to) {
#if defined(__linux__)
    return ::syscall(SYS_renameat2, parent, from.c_str(), parent, to.c_str(), RENAME_NOREPLACE) ==
           0;
#elif defined(__APPLE__)
    return ::renameatx_np(parent, from.c_str(), parent, to.c_str(), RENAME_EXCL) == 0;
#else
    errno = ENOTSUP;
    return false;
#endif
}

// Descriptor-relative recursion is bounded by the filesystem's directory depth and keeps every
// descent pinned to the identity validated by openat/fstat. The checks stay together so no entry
// can pass from observation to unlink without the device and inode validation.
// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
bool remove_entries(int directory, dev_t root_device, std::uint64_t root_mount_identity,
                    const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks,
                    std::string_view root_path, RemovalFilesystemFailure& failure) {
    const int duplicate = ::dup(directory);
    if (duplicate < 0) {
        failure.reason = "io_error";
        return false;
    }
    DIR* stream = ::fdopendir(duplicate);
    if (stream == nullptr) {
        ::close(duplicate);
        failure.reason = "io_error";
        return false;
    }
    std::vector<std::string> entries;
    errno = 0;
    while (const auto* entry = ::readdir(stream)) {
        const std::string_view name(entry->d_name);
        if (name != "." && name != "..") {
            entries.emplace_back(name);
        }
    }
    const int read_error = errno;
    ::closedir(stream);
    if (read_error != 0) {
        failure.reason = "io_error";
        return false;
    }
    std::ranges::sort(entries);

    for (const auto& name : entries) {
        notify(hooks, RemovalFilesystemStage::BeforeEntryRemoval, root_path);
        if (injected(hooks, RemovalFilesystemStage::BeforeEntryRemoval, root_path)) {
            failure.reason = "io_error";
            return false;
        }
        struct stat metadata {};
        if (::fstatat(directory, name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            failure.reason = "io_error";
            return false;
        }
        if (metadata.st_dev != root_device || forced_device_boundary(hooks, root_path)) {
            failure.reason = "mount_boundary";
            return false;
        }
#if defined(__linux__)
        const Descriptor entry(::openat(directory, name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
#else
        const Descriptor entry;
#endif
        struct stat entry_metadata {};
        const auto entry_mount = entry ? mount_identity(entry.get(), hooks) : std::nullopt;
        if (!entry || ::fstat(entry.get(), &entry_metadata) != 0 ||
            entry_metadata.st_dev != metadata.st_dev || entry_metadata.st_ino != metadata.st_ino) {
            failure.reason = "path_changed";
            return false;
        }
        if (!entry_mount || *entry_mount != root_mount_identity) {
            failure.reason = "mount_boundary";
            return false;
        }
        if (S_ISDIR(metadata.st_mode)) {
            const Descriptor child(
                ::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            struct stat opened {};
            const auto child_mount = child ? mount_identity(child.get(), hooks) : std::nullopt;
            if (!child || ::fstat(child.get(), &opened) != 0 || !S_ISDIR(opened.st_mode) ||
                opened.st_dev != root_device || opened.st_dev != metadata.st_dev ||
                opened.st_ino != metadata.st_ino) {
                failure.reason = opened.st_dev != root_device ? "mount_boundary" : "path_changed";
                return false;
            }
            if (!child_mount || *child_mount != root_mount_identity) {
                failure.reason = "mount_boundary";
                return false;
            }
            if (!remove_entries(child.get(), root_device, root_mount_identity, hooks, root_path,
                                failure)) {
                return false;
            }
            struct stat final_metadata {};
            if (::fstatat(directory, name.c_str(), &final_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
                final_metadata.st_dev != opened.st_dev || final_metadata.st_ino != opened.st_ino ||
                ::unlinkat(directory, name.c_str(), AT_REMOVEDIR) != 0) {
                failure.reason = "path_changed";
                return false;
            }
            continue;
        }
        if (::unlinkat(directory, name.c_str(), 0) != 0) {
            failure.reason = errno == EISDIR ? "path_changed" : "io_error";
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<CapturedRemovalRoot>
capture_removal_root(std::string path, uid_t expected_uid, RemovalFilesystemFailure& failure,
                     const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks) {
    const auto split = split_path(path);
    if (!split) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    const Descriptor parent(
        ::open(split->parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!parent) {
        if (errno == ENOENT) {
            failure.reason.clear();
            return CapturedRemovalRoot{std::move(path), std::nullopt};
        }
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    if (!valid_parent(parent.get(), expected_uid)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    notify(hooks, RemovalFilesystemStage::AfterParentOpen, path);
    if (injected(hooks, RemovalFilesystemStage::AfterParentOpen, path)) {
        failure.reason = "io_error";
        return std::nullopt;
    }

    struct stat parent_metadata {};
    struct stat root_metadata {};
    if (::fstat(parent.get(), &parent_metadata) != 0) {
        failure.reason = "io_error";
        return std::nullopt;
    }
    if (::fstatat(parent.get(), split->name.c_str(), &root_metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            failure.reason.clear();
            return CapturedRemovalRoot{std::move(path), std::nullopt};
        }
        failure.reason = "io_error";
        return std::nullopt;
    }
    if (!S_ISDIR(root_metadata.st_mode) || root_metadata.st_uid != expected_uid) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    const Descriptor root(::openat(parent.get(), split->name.c_str(),
                                   O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat opened_metadata {};
    if (!root || ::fstat(root.get(), &opened_metadata) != 0 ||
        opened_metadata.st_dev != root_metadata.st_dev ||
        opened_metadata.st_ino != root_metadata.st_ino) {
        failure.reason = "path_changed";
        return std::nullopt;
    }
    if (root_metadata.st_dev != parent_metadata.st_dev || forced_device_boundary(hooks, path) ||
        !same_mount(parent.get(), root.get(), hooks, failure)) {
        failure.reason = "mount_boundary";
        return std::nullopt;
    }
    failure.reason.clear();
    return CapturedRemovalRoot{
        path, proto::RootIdentity{std::move(path), static_cast<std::uint64_t>(root_metadata.st_dev),
                                  static_cast<std::uint64_t>(root_metadata.st_ino),
                                  static_cast<std::uint64_t>(root_metadata.st_uid)}};
}

bool revalidate_removal_root(const CapturedRemovalRoot& root, uid_t expected_uid,
                             RemovalFilesystemFailure& failure,
                             const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks) {
    const auto split = split_path(root.path);
    if (!split) {
        failure.reason = "path_invalid";
        return false;
    }
    const Descriptor parent(
        ::open(split->parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!parent) {
        if (errno == ENOENT && !root.identity) {
            failure.reason.clear();
            return true;
        }
        failure.reason = root.identity ? "path_changed" : "path_invalid";
        return false;
    }
    if (!valid_parent(parent.get(), expected_uid)) {
        failure.reason = "path_invalid";
        return false;
    }
    notify(hooks, RemovalFilesystemStage::BeforeRootRevalidation, root.path);
    if (injected(hooks, RemovalFilesystemStage::BeforeRootRevalidation, root.path)) {
        failure.reason = "io_error";
        return false;
    }
    bool absent = false;
    const bool valid =
        absent_or_identity(parent.get(), split->name, root.identity, absent, failure, hooks);
    if (valid) {
        failure.reason.clear();
    }
    return valid;
}

// The complete original/staging identity matrix is kept in one closed decision table so recovery
// cannot accidentally classify an unvalidated replacement as removable.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<RemovalRootObservation> observe_removal_root(const CapturedRemovalRoot& root,
                                                           std::string_view invocation_id,
                                                           std::string_view label,
                                                           uid_t expected_uid,
                                                           RemovalFilesystemFailure& failure) {
    const auto split = split_path(root.path);
    if (!split || !valid_invocation_id(invocation_id) || (label != "data" && label != "state")) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    const Descriptor parent(
        ::open(split->parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!parent) {
        if (errno == ENOENT) {
            failure.reason.clear();
            return root.identity ? RemovalRootObservation::Absent
                                 : RemovalRootObservation::PlannedAbsent;
        }
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    if (!valid_parent(parent.get(), expected_uid)) {
        failure.reason = "path_invalid";
        return std::nullopt;
    }
    const std::string staged =
        ".tgcli-removal-" + std::string(invocation_id) + "-" + std::string(label);
    struct stat original {};
    const bool original_present =
        ::fstatat(parent.get(), split->name.c_str(), &original, AT_SYMLINK_NOFOLLOW) == 0;
    if (!original_present && errno != ENOENT) {
        failure.reason = "io_error";
        return std::nullopt;
    }
    struct stat staged_metadata {};
    const bool staged_present =
        ::fstatat(parent.get(), staged.c_str(), &staged_metadata, AT_SYMLINK_NOFOLLOW) == 0;
    if (!staged_present && errno != ENOENT) {
        failure.reason = "io_error";
        return std::nullopt;
    }
    if (!root.identity) {
        failure.reason.clear();
        return original_present || staged_present ? RemovalRootObservation::Changed
                                                  : RemovalRootObservation::PlannedAbsent;
    }
    if (original_present && staged_present) {
        failure.reason.clear();
        return RemovalRootObservation::Changed;
    }
    if (!original_present && !staged_present) {
        failure.reason.clear();
        return RemovalRootObservation::Absent;
    }
    const auto& metadata = original_present ? original : staged_metadata;
    const auto& observed_name = original_present ? split->name : staged;
    struct stat parent_metadata {};
    if (!S_ISDIR(metadata.st_mode) || !same_identity(metadata, *root.identity) ||
        ::fstat(parent.get(), &parent_metadata) != 0 || metadata.st_dev != parent_metadata.st_dev) {
        failure.reason.clear();
        return RemovalRootObservation::Changed;
    }
    const Descriptor observed(::openat(parent.get(), observed_name.c_str(),
                                       O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat opened_metadata {};
    if (!observed || ::fstat(observed.get(), &opened_metadata) != 0 ||
        !same_identity(opened_metadata, *root.identity) ||
        opened_metadata.st_dev != metadata.st_dev || opened_metadata.st_ino != metadata.st_ino) {
        failure.reason.clear();
        return RemovalRootObservation::Changed;
    }
    if (!same_mount(parent.get(), observed.get(), {}, failure)) {
        return std::nullopt;
    }
    failure.reason.clear();
    return original_present ? RemovalRootObservation::Captured : RemovalRootObservation::Staged;
}

// This is the filesystem deletion transaction: validate, exclusive stage, descriptor-relative
// erase, parent fsync, and replacement check must remain visibly ordered as one safety boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool delete_removal_root(const CapturedRemovalRoot& root, std::string_view invocation_id,
                         std::string_view label, uid_t expected_uid,
                         RemovalFilesystemFailure& failure,
                         const std::shared_ptr<const testing::RemovalFilesystemHooks>& hooks) {
    const auto split = split_path(root.path);
    if (!split || !valid_invocation_id(invocation_id) || (label != "data" && label != "state")) {
        failure.reason = "path_invalid";
        return false;
    }
    const Descriptor parent(
        ::open(split->parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!parent) {
        if (errno == ENOENT && !root.identity) {
            failure.reason.clear();
            return true;
        }
        failure.reason = root.identity ? "path_changed" : "path_invalid";
        return false;
    }
    if (!valid_parent(parent.get(), expected_uid)) {
        failure.reason = "path_invalid";
        return false;
    }
    const std::string staged =
        ".tgcli-removal-" + std::string(invocation_id) + "-" + std::string(label);
    struct stat original_metadata {};
    const bool original_present =
        ::fstatat(parent.get(), split->name.c_str(), &original_metadata, AT_SYMLINK_NOFOLLOW) == 0;
    if (!original_present && errno != ENOENT) {
        failure.reason = "io_error";
        return false;
    }
    struct stat staged_metadata {};
    bool staged_present =
        ::fstatat(parent.get(), staged.c_str(), &staged_metadata, AT_SYMLINK_NOFOLLOW) == 0;
    if (!staged_present && errno != ENOENT) {
        failure.reason = "io_error";
        return false;
    }

    if (!root.identity) {
        if (original_present || staged_present) {
            failure.reason = "path_changed";
            return false;
        }
        failure.reason.clear();
        return true;
    }
    if (original_present && staged_present) {
        failure.reason = "path_changed";
        return false;
    }
    if (original_present) {
        bool ignored_absent = false;
        if (!absent_or_identity(parent.get(), split->name, root.identity, ignored_absent, failure,
                                hooks)) {
            return false;
        }
        notify(hooks, RemovalFilesystemStage::BeforeStageRename, root.path);
        if (injected(hooks, RemovalFilesystemStage::BeforeStageRename, root.path)) {
            failure.reason = "io_error";
            return false;
        }
        if (!rename_exclusive(parent.get(), split->name, staged)) {
            failure.reason = errno == EXDEV ? "mount_boundary" : "path_changed";
            return false;
        }
        notify(hooks, RemovalFilesystemStage::AfterStageRename, root.path);
        if (injected(hooks, RemovalFilesystemStage::AfterStageRename, root.path)) {
            failure.reason = "io_error";
            return false;
        }
        staged_present = true;
    }
    if (!staged_present) {
        failure.reason.clear();
        return true;
    }

    const Descriptor directory(
        ::openat(parent.get(), staged.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat opened {};
    struct stat parent_metadata {};
    if (!directory || ::fstat(directory.get(), &opened) != 0 ||
        ::fstat(parent.get(), &parent_metadata) != 0 || !S_ISDIR(opened.st_mode) ||
        !same_identity(opened, *root.identity)) {
        failure.reason = "path_changed";
        return false;
    }
    if (opened.st_dev != parent_metadata.st_dev || forced_device_boundary(hooks, root.path)) {
        failure.reason = "mount_boundary";
        return false;
    }
    const auto root_mount = mount_identity(directory.get(), hooks);
    const auto parent_mount = mount_identity(parent.get(), hooks);
    if (!root_mount || !parent_mount || *root_mount != *parent_mount) {
        failure.reason = "mount_boundary";
        return false;
    }
    if (!remove_entries(directory.get(), opened.st_dev, *root_mount, hooks, root.path, failure)) {
        return false;
    }
    notify(hooks, RemovalFilesystemStage::BeforeRootRemoval, root.path);
    if (injected(hooks, RemovalFilesystemStage::BeforeRootRemoval, root.path)) {
        failure.reason = "io_error";
        return false;
    }
    struct stat final_metadata {};
    if (::fstatat(parent.get(), staged.c_str(), &final_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_identity(final_metadata, *root.identity) ||
        ::unlinkat(parent.get(), staged.c_str(), AT_REMOVEDIR) != 0) {
        failure.reason = "path_changed";
        return false;
    }
    notify(hooks, RemovalFilesystemStage::BeforeParentSync, root.path);
    if (injected(hooks, RemovalFilesystemStage::BeforeParentSync, root.path) ||
        ::fsync(parent.get()) != 0) {
        failure.reason = "sync_error";
        return false;
    }
    struct stat replacement {};
    if (::fstatat(parent.get(), split->name.c_str(), &replacement, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT) {
        failure.reason = "path_changed";
        return false;
    }
    failure.reason.clear();
    return true;
}

} // namespace tgcli::daemon
