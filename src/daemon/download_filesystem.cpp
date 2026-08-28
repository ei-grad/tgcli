#include "daemon/download_filesystem.hpp"

#include "daemon/download_domain.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/random.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/random.h>
#endif

namespace tgcli::daemon {

namespace {

constexpr std::size_t kCopyBufferSize = static_cast<std::size_t>(64) * 1024U;

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
    [[nodiscard]] int get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ >= 0;
    }

  private:
    int value_ = -1;
};

struct SplitPath {
    std::vector<std::string> parents;
    std::string leaf;
};

struct SourceIdentity {
    dev_t device = 0;
    ino_t inode = 0;
    mode_t mode = 0;
    off_t size = 0;
    timespec modified{};
    timespec changed{};

    bool operator==(const SourceIdentity& other) const {
        return device == other.device && inode == other.inode && mode == other.mode &&
               size == other.size && modified.tv_sec == other.modified.tv_sec &&
               modified.tv_nsec == other.modified.tv_nsec &&
               changed.tv_sec == other.changed.tv_sec && changed.tv_nsec == other.changed.tv_nsec;
    }
};

struct OpenedParent {
    Descriptor descriptor;
    std::string leaf;
};

DownloadFilesystemError error(DownloadFilesystemReason reason, std::string final_path) {
    return {.kind = DownloadFilesystemErrorKind::OutputUnavailable,
            .reason = reason,
            .final_path = std::move(final_path)};
}

DownloadFilesystemError exists(std::string final_path) {
    return {.kind = DownloadFilesystemErrorKind::OutputExists,
            .reason = DownloadFilesystemReason::InvalidPath,
            .final_path = std::move(final_path)};
}

DownloadFilesystemError stopped(std::string final_path) {
    return {.kind = DownloadFilesystemErrorKind::Stopped,
            .reason = DownloadFilesystemReason::InvalidPath,
            .final_path = std::move(final_path)};
}

bool injected(const std::shared_ptr<const testing::DownloadFilesystemHooks>& hooks,
              DownloadFilesystemStage stage) {
    if (hooks && hooks->observe) {
        hooks->observe(stage);
    }
    return hooks && hooks->fail && hooks->fail(stage);
}

std::optional<SplitPath> split_absolute(std::string_view value, bool strict_components) {
    if (value.empty() || value.front() != '/' || value.find('\0') != std::string_view::npos ||
        value == "/") {
        return std::nullopt;
    }
    SplitPath result;
    std::size_t start = 1;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part =
            value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
        if (part.empty() || part == "." || part == "..") {
            if (strict_components || !part.empty()) {
                return std::nullopt;
            }
        } else if (end == std::string_view::npos) {
            result.leaf = std::string(part);
            break;
        } else {
            result.parents.emplace_back(part);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (result.leaf.empty()) {
        return std::nullopt;
    }
    return result;
}

std::optional<OpenedParent> open_parent(std::string_view absolute_path) {
    const auto split = split_absolute(absolute_path, true);
    if (!split) {
        return std::nullopt;
    }
    Descriptor current(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (!current) {
        return std::nullopt;
    }
    for (const auto& component : split->parents) {
        Descriptor next(::openat(current.get(), component.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        if (!next) {
            return std::nullopt;
        }
        current = std::move(next);
    }
    return OpenedParent{.descriptor = std::move(current), .leaf = split->leaf};
}

bool directory_is_safe(std::string_view absolute_path) {
    const auto split = split_absolute(absolute_path, true);
    if (!split) {
        return false;
    }
    const auto parent = open_parent(absolute_path);
    if (!parent) {
        return false;
    }
    const Descriptor directory(::openat(parent->descriptor.get(), parent->leaf.c_str(),
                                        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    return static_cast<bool>(directory);
}

std::optional<std::string> absolute_lexical(std::string_view value, std::string_view cwd) {
    if (value.empty() || value.find('\0') != std::string_view::npos || cwd.empty() ||
        cwd.front() != '/') {
        return std::nullopt;
    }
    std::filesystem::path candidate(value);
    if (!candidate.is_absolute()) {
        candidate = std::filesystem::path(cwd) / candidate;
    }
    candidate = candidate.lexically_normal();
    auto result = candidate.string();
    if (result.empty() || result.front() != '/' || result == "/") {
        return std::nullopt;
    }
    return result;
}

std::string dirname(std::string_view value) {
    return std::filesystem::path(value).parent_path().string();
}

std::string basename(std::string_view value) {
    return std::filesystem::path(value).filename().string();
}

bool path_is_existing_directory(std::string_view absolute_path) {
    const auto parent = open_parent(absolute_path);
    if (!parent) {
        return false;
    }
    struct stat metadata {};
    return ::fstatat(parent->descriptor.get(), parent->leaf.c_str(), &metadata,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISDIR(metadata.st_mode);
}

std::string random_hex() {
    std::array<unsigned char, 16> bytes{};
#if defined(__linux__)
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {};
        }
        if (count == 0) {
            return {};
        }
        offset += static_cast<std::size_t>(count);
    }
#elif defined(__APPLE__)
    ::arc4random_buf(bytes.data(), bytes.size());
#else
    return {};
#endif
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(32, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result.at(index * 2) = digits[bytes.at(index) >> 4U];
        result.at(index * 2 + 1) = digits[bytes.at(index) & 0x0FU];
    }
    return result;
}

SourceIdentity identity(const struct stat& value) {
#if defined(__APPLE__)
    const auto modified = value.st_mtimespec;
    const auto changed = value.st_ctimespec;
#else
    const auto modified = value.st_mtim;
    const auto changed = value.st_ctim;
#endif
    return {.device = value.st_dev,
            .inode = value.st_ino,
            .mode = value.st_mode,
            .size = value.st_size,
            .modified = modified,
            .changed = changed};
}

bool rename_exclusive(int directory, const std::string& temporary, const std::string& final) {
#if defined(__linux__)
    return ::syscall(SYS_renameat2, directory, temporary.c_str(), directory, final.c_str(),
                     RENAME_NOREPLACE) == 0;
#elif defined(__APPLE__)
    return ::renameatx_np(directory, temporary.c_str(), directory, final.c_str(), RENAME_EXCL) == 0;
#else
    errno = ENOTSUP;
    return false;
#endif
}

bool valid_temp_token(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

bool cleanup_temp(int directory, const std::string& temporary,
                  const std::shared_ptr<const testing::DownloadFilesystemHooks>& hooks) {
    return !injected(hooks, DownloadFilesystemStage::Cleanup) &&
           ::unlinkat(directory, temporary.c_str(), 0) == 0 && ::fsync(directory) == 0;
}

} // namespace

DownloadDestinationOutcome
prepare_download_destination(const std::optional<std::string>& output,
                             const std::optional<std::string>& media_directory,
                             std::string_view frozen_cwd) {
    const bool trailing_slash = output && !output->empty() && output->back() == '/';
    std::string_view selected = frozen_cwd;
    if (media_directory) {
        selected = *media_directory;
    }
    if (output) {
        selected = *output;
    }
    const auto absolute = absolute_lexical(selected, frozen_cwd);
    if (!absolute) {
        return error(DownloadFilesystemReason::InvalidPath, std::string(frozen_cwd));
    }
    const bool directory_mode = !output || trailing_slash || path_is_existing_directory(*absolute);
    if (directory_mode) {
        if (!directory_is_safe(*absolute)) {
            return error(DownloadFilesystemReason::InvalidPath, *absolute);
        }
        return DownloadDestination{
            .directory = *absolute, .leaf = std::nullopt, .directory_mode = true};
    }
    const auto parent = dirname(*absolute);
    const auto leaf = basename(*absolute);
    if (parent.empty() || !safe_download_leaf(leaf)) {
        return error(DownloadFilesystemReason::InvalidPath, *absolute);
    }
    return DownloadDestination{.directory = parent, .leaf = leaf, .directory_mode = false};
}

DownloadDestinationOutcome apply_suggested_file_name(DownloadDestination destination,
                                                     std::string suggested_name) {
    if (!destination.directory_mode || destination.leaf || !safe_download_leaf(suggested_name)) {
        return error(DownloadFilesystemReason::InvalidPath, destination.directory);
    }
    destination.leaf = std::move(suggested_name);
    return destination;
}

// NOLINTBEGIN(readability-function-cognitive-complexity): ordered durability protocol.
DownloadPublishOutcome
publish_download_file(const core::TdFile& completed_file, const DownloadDestination& destination,
                      const DownloadPublishControl& may_publish,
                      const std::shared_ptr<const testing::DownloadFilesystemHooks>& hooks) {
    const auto final_path =
        destination.leaf
            ? (std::filesystem::path(destination.directory) / *destination.leaf).string()
            : destination.directory;
    if (!destination.leaf || !safe_download_leaf(*destination.leaf) ||
        !directory_is_safe(destination.directory) || !completed_file.local ||
        completed_file.local->path.empty()) {
        return error(DownloadFilesystemReason::InvalidPath, final_path);
    }
    const auto source_parent = open_parent(completed_file.local->path);
    if (!source_parent) {
        return error(DownloadFilesystemReason::InvalidPath, final_path);
    }
    const Descriptor source(::openat(source_parent->descriptor.get(), source_parent->leaf.c_str(),
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat source_before {};
    if (!source || ::fstat(source.get(), &source_before) != 0) {
        return error(DownloadFilesystemReason::OpenFailed, final_path);
    }
    if (!S_ISREG(source_before.st_mode) || source_before.st_uid != ::getuid() ||
        source_before.st_size < 0 || source_before.st_size > core::kTdInt53Max) {
        return error(DownloadFilesystemReason::SourceChanged, final_path);
    }
    const auto destination_parent = open_parent(final_path);
    if (!destination_parent) {
        return error(DownloadFilesystemReason::InvalidPath, final_path);
    }
    struct stat existing {};
    if (::fstatat(destination_parent->descriptor.get(), destination_parent->leaf.c_str(), &existing,
                  AT_SYMLINK_NOFOLLOW) == 0) {
        return exists(final_path);
    }
    if (errno != ENOENT) {
        return error(DownloadFilesystemReason::OpenFailed, final_path);
    }
    const auto token = hooks && hooks->random_hex ? hooks->random_hex() : random_hex();
    if (!valid_temp_token(token)) {
        return error(DownloadFilesystemReason::OpenFailed, final_path);
    }
    const auto temporary = "." + destination_parent->leaf + ".tgcli-download." + token + ".tmp";
    const Descriptor output(::openat(destination_parent->descriptor.get(), temporary.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!output) {
        return error(DownloadFilesystemReason::OpenFailed, final_path);
    }
    if (injected(hooks, DownloadFilesystemStage::TempCreated)) {
        return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                   ? error(DownloadFilesystemReason::WriteFailed, final_path)
                   : error(DownloadFilesystemReason::CleanupFailed, final_path);
    }
    std::array<std::byte, kCopyBufferSize> buffer{};
    std::uint64_t copied = 0;
    while (true) {
        const auto count = ::read(source.get(), buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                       ? error(DownloadFilesystemReason::SourceChanged, final_path)
                       : error(DownloadFilesystemReason::CleanupFailed, final_path);
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(count)) {
            const auto written = ::write(output.get(), buffer.data() + offset,
                                         static_cast<std::size_t>(count) - offset);
            if (written <= 0) {
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                           ? error(DownloadFilesystemReason::WriteFailed, final_path)
                           : error(DownloadFilesystemReason::CleanupFailed, final_path);
            }
            offset += static_cast<std::size_t>(written);
        }
        copied += static_cast<std::uint64_t>(count);
    }
    if (injected(hooks, DownloadFilesystemStage::CopyComplete)) {
        return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                   ? error(DownloadFilesystemReason::WriteFailed, final_path)
                   : error(DownloadFilesystemReason::CleanupFailed, final_path);
    }
    struct stat source_after {};
    struct stat output_status {};
    const auto stable =
        ::fstat(source.get(), &source_after) == 0 &&
        identity(source_before) == identity(source_after) &&
        copied == static_cast<std::uint64_t>(source_before.st_size) &&
        ::fstat(output.get(), &output_status) == 0 && S_ISREG(output_status.st_mode) &&
        output_status.st_uid == ::getuid() && output_status.st_nlink == 1 &&
        (output_status.st_mode & 0777) == 0600 && output_status.st_size == source_before.st_size &&
        (completed_file.size <= 0 || copied == static_cast<std::uint64_t>(completed_file.size));
    if (!stable || injected(hooks, DownloadFilesystemStage::SourceRevalidated)) {
        return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                   ? error(DownloadFilesystemReason::SourceChanged, final_path)
                   : error(DownloadFilesystemReason::CleanupFailed, final_path);
    }
    if (::fsync(output.get()) != 0 || injected(hooks, DownloadFilesystemStage::TempSynced)) {
        return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                   ? error(DownloadFilesystemReason::SyncFailed, final_path)
                   : error(DownloadFilesystemReason::CleanupFailed, final_path);
    }
    if (injected(hooks, DownloadFilesystemStage::BeforePublish)) {
        return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                   ? error(DownloadFilesystemReason::WriteFailed, final_path)
                   : error(DownloadFilesystemReason::CleanupFailed, final_path);
    }
    if (may_publish && !may_publish()) {
        return cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)
                   ? stopped(final_path)
                   : error(DownloadFilesystemReason::CleanupFailed, final_path);
    }
    if (!rename_exclusive(destination_parent->descriptor.get(), temporary,
                          destination_parent->leaf)) {
        const auto rename_error = errno;
        if (!cleanup_temp(destination_parent->descriptor.get(), temporary, hooks)) {
            return error(DownloadFilesystemReason::CleanupFailed, final_path);
        }
        return rename_error == EEXIST ? exists(final_path)
                                      : error(DownloadFilesystemReason::WriteFailed, final_path);
    }
    if (injected(hooks, DownloadFilesystemStage::Published) ||
        ::fsync(destination_parent->descriptor.get()) != 0 ||
        injected(hooks, DownloadFilesystemStage::DirectorySynced)) {
        return error(DownloadFilesystemReason::SyncFailed, final_path);
    }
    return PublishedDownload{.path = final_path, .bytes = copied};
}
// NOLINTEND(readability-function-cognitive-complexity)

std::string_view download_filesystem_reason_name(DownloadFilesystemReason reason) {
    switch (reason) {
    case DownloadFilesystemReason::InvalidPath:
        return "invalid_path";
    case DownloadFilesystemReason::OpenFailed:
        return "open_failed";
    case DownloadFilesystemReason::WriteFailed:
        return "write_failed";
    case DownloadFilesystemReason::SyncFailed:
        return "sync_failed";
    case DownloadFilesystemReason::SourceChanged:
        return "source_changed";
    case DownloadFilesystemReason::CleanupFailed:
        return "cleanup_failed";
    }
    return "invalid_path";
}

} // namespace tgcli::daemon
