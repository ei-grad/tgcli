#include "daemon/file_spool.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace tgcli::daemon {

namespace {

constexpr std::size_t kBufferSize = static_cast<std::size_t>(64) * 1024U;
constexpr std::string_view kSpoolName = "spool";
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

class Descriptor final {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        reset();
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset();
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
    void reset() {
        if (value_ >= 0) {
            ::close(value_);
        }
        value_ = -1;
    }

    int value_;
};

struct SourceLocator {
    bool absolute{false};
    std::vector<std::string> components;
    std::string display_path;
    std::string name;
    Descriptor cwd;
};

struct DirectoryEdge {
    Descriptor parent;
    Descriptor child;
    std::string component;
    struct stat child_status {};
};

struct RootHandle {
    Descriptor account_state;
    Descriptor root;
    struct stat root_status {};
    std::string account_state_path;
    bool absent{false};
};

struct InvocationHandle {
    Descriptor descriptor;
    struct stat status {};
};

struct DestinationHandle {
    Descriptor descriptor;
    struct stat status {};
};

enum class SourcePass { First, Second };

FileSpoolError simple_error(FileSpoolErrorKind kind) {
    FileSpoolError result;
    result.kind = kind;
    return result;
}

FileSpoolError source_error(SourceFileReason reason) {
    FileSpoolError result;
    result.kind = FileSpoolErrorKind::SourceUnavailable;
    result.source_reason = reason;
    return result;
}

FileSpoolError durability_error(DurabilityReason reason) {
    FileSpoolError result;
    result.kind = FileSpoolErrorKind::DurabilityFailure;
    result.durability_reason = reason;
    return result;
}

FileSpoolError contradiction_error() {
    FileSpoolError result;
    result.kind = FileSpoolErrorKind::Contradiction;
    result.durability_reason = DurabilityReason::Contradiction;
    return result;
}

FileSpoolError contradiction_at(FilesystemDiagnosticPath path) {
    auto error = contradiction_error();
    error.diagnostic_path = std::move(path);
    return error;
}

std::optional<FileSpoolError> control_error(const FileSpoolControl& control) {
    if (control.deadline && std::chrono::steady_clock::now() >= *control.deadline) {
        return simple_error(FileSpoolErrorKind::TimedOut);
    }
    if (control.stop_token.stop_requested()) {
        return simple_error(FileSpoolErrorKind::Cancelled);
    }
    return std::nullopt;
}

void notify(const std::shared_ptr<const testing::FileSpoolHooks>& hooks, FileSpoolStage stage) {
    if (hooks && hooks->at_stage) {
        hooks->at_stage(stage);
    }
}

std::optional<FileSpoolError>
notify_controlled(const std::shared_ptr<const testing::FileSpoolHooks>& hooks, FileSpoolStage stage,
                  const FileSpoolControl& control) {
    notify(hooks, stage);
    return control_error(control);
}

bool injected(const std::shared_ptr<const testing::FileSpoolHooks>& hooks, FileSpoolStage stage) {
    return hooks && hooks->should_fail && hooks->should_fail(stage);
}

void mutate_metadata(const std::shared_ptr<const testing::FileSpoolHooks>& hooks,
                     FileSpoolMetadata point, struct stat& status) {
    if (hooks && hooks->mutate_metadata) {
        hooks->mutate_metadata(point, status);
    }
}

bool same_inode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool same_file_type(const struct stat& left, const struct stat& right) {
    return (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

bool same_directory(const struct stat& left, const struct stat& right) {
    return same_inode(left, right) && same_file_type(left, right) && S_ISDIR(left.st_mode);
}

bool valid_private_directory(const struct stat& status, uid_t expected_uid) {
    return S_ISDIR(status.st_mode) && status.st_uid == expected_uid &&
           (status.st_mode & 07777) == 0700;
}

bool valid_private_file(const struct stat& status, uid_t expected_uid) {
    return S_ISREG(status.st_mode) && status.st_uid == expected_uid &&
           (status.st_mode & 07777) == 0600 && status.st_nlink == 1;
}

std::vector<std::string> split_components(std::string_view value, bool absolute) {
    std::vector<std::string> result;
    std::size_t begin = absolute ? 1 : 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        if (end == std::string_view::npos) {
            result.emplace_back(value.substr(begin));
            break;
        }
        result.emplace_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    return result;
}

bool has_forbidden_basename_control(std::string_view value) {
    for (std::size_t offset = 0; offset < value.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        if (byte <= 0x1fU || byte == 0x7fU ||
            (byte == 0xc2U && offset + 1 < value.size() &&
             static_cast<unsigned char>(value[offset + 1]) >= 0x80U &&
             static_cast<unsigned char>(value[offset + 1]) <= 0x9fU)) {
            return true;
        }
    }
    return false;
}

bool valid_basename(std::string_view value) {
    return !value.empty() && value.size() <= 255 && value != "." && value != ".." &&
           value.find('/') == std::string_view::npos &&
           value.find('\0') == std::string_view::npos && common::valid_utf8(value) &&
           !has_forbidden_basename_control(value);
}

bool valid_invocation_id(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool unsigned_bytes_less(std::string_view left, std::string_view right) {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](char lhs, char rhs) {
            return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs);
        });
}

std::optional<std::int64_t> timestamp_nanoseconds(time_t seconds, long nanoseconds) {
    if (nanoseconds < 0 || nanoseconds >= static_cast<long>(kNanosecondsPerSecond)) {
        return std::nullopt;
    }
    if (!std::in_range<std::int64_t>(seconds)) {
        return std::nullopt;
    }
    std::int64_t scaled = 0;
    std::int64_t result = 0;
    if (__builtin_mul_overflow(static_cast<std::int64_t>(seconds),
                               static_cast<std::int64_t>(kNanosecondsPerSecond), &scaled) ||
        __builtin_add_overflow(scaled, static_cast<std::int64_t>(nanoseconds), &result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::pair<std::int64_t, std::int64_t>> file_times(const struct stat& status) {
#if defined(__APPLE__)
    const auto modified =
        timestamp_nanoseconds(status.st_mtimespec.tv_sec, status.st_mtimespec.tv_nsec);
    const auto changed =
        timestamp_nanoseconds(status.st_ctimespec.tv_sec, status.st_ctimespec.tv_nsec);
#else
    const auto modified = timestamp_nanoseconds(status.st_mtim.tv_sec, status.st_mtim.tv_nsec);
    const auto changed = timestamp_nanoseconds(status.st_ctim.tv_sec, status.st_ctim.tv_nsec);
#endif
    if (!modified || !changed) {
        return std::nullopt;
    }
    return std::pair{*modified, *changed};
}

std::optional<FileSnapshot> snapshot_from_status(const SourceLocator& locator,
                                                 const struct stat& status) {
    if (status.st_size < 0) {
        return std::nullopt;
    }
    const auto times = file_times(status);
    if (!times) {
        return std::nullopt;
    }
    FileSnapshot result;
    result.path = locator.display_path;
    result.name = locator.name;
    result.size = static_cast<std::uint64_t>(status.st_size);
    result.device = static_cast<std::uint64_t>(status.st_dev);
    result.inode = static_cast<std::uint64_t>(status.st_ino);
    result.mtime_ns = times->first;
    result.ctime_ns = times->second;
    return result;
}

bool same_snapshot_identity(const FileSnapshot& snapshot, const struct stat& status) {
    const auto times = file_times(status);
    return times && status.st_size >= 0 &&
           snapshot.device == static_cast<std::uint64_t>(status.st_dev) &&
           snapshot.inode == static_cast<std::uint64_t>(status.st_ino) &&
           snapshot.size == static_cast<std::uint64_t>(status.st_size) &&
           snapshot.mtime_ns == times->first && snapshot.ctime_ns == times->second;
}

class Sha256 final {
  public:
    void update(const unsigned char* bytes, std::size_t size) {
        total_size_ += size;
        while (size > 0) {
            const auto copied = std::min(size, block_.size() - block_size_);
            std::copy_n(bytes, copied, block_.begin() + static_cast<std::ptrdiff_t>(block_size_));
            block_size_ += copied;
            bytes += copied;
            size -= copied;
            if (block_size_ == block_.size()) {
                transform(block_);
                block_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finish() {
        const auto bit_length = static_cast<std::uint64_t>(total_size_) * 8U;
        block_.at(block_size_++) = 0x80U;
        if (block_size_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), 0);
            transform(block_);
            block_size_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56,
                  0);
        for (std::size_t index = 0; index < 8; ++index) {
            block_.at(56 + index) =
                static_cast<unsigned char>(bit_length >> static_cast<unsigned>((7 - index) * 8));
        }
        transform(block_);
        constexpr std::string_view hex = "0123456789abcdef";
        std::string output(64, '0');
        std::size_t offset = 0;
        for (const auto word : digest_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                output.at(offset++) = hex.at((word >> static_cast<unsigned>(shift)) & 0x0fU);
            }
        }
        return output;
    }

  private:
    void transform(const std::array<unsigned char, 64>& block) {
        constexpr std::array<std::uint32_t, 64> constants = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
            0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
            0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
            0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
            0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4;
            words.at(index) = (static_cast<std::uint32_t>(block.at(offset)) << 24U) |
                              (static_cast<std::uint32_t>(block.at(offset + 1)) << 16U) |
                              (static_cast<std::uint32_t>(block.at(offset + 2)) << 8U) |
                              static_cast<std::uint32_t>(block.at(offset + 3));
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto first = words.at(index - 15);
            const auto second = words.at(index - 2);
            const auto sigma0 = std::rotr(first, 7) ^ std::rotr(first, 18) ^ (first >> 3U);
            const auto sigma1 = std::rotr(second, 17) ^ std::rotr(second, 19) ^ (second >> 10U);
            words.at(index) = words.at(index - 16) + sigma0 + words.at(index - 7) + sigma1;
        }
        auto [a, b, c, d, e, f, g, h] = digest_;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temporary1 = h + sum1 + choose + constants.at(index) + words.at(index);
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        digest_[0] += a;
        digest_[1] += b;
        digest_[2] += c;
        digest_[3] += d;
        digest_[4] += e;
        digest_[5] += f;
        digest_[6] += g;
        digest_[7] += h;
    }

    std::array<std::uint32_t, 8> digest_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                         0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block_{};
    std::size_t block_size_{0};
    std::size_t total_size_{0};
};

std::optional<std::string> canonical_display(std::string_view caller_path,
                                             std::string_view frozen_cwd) {
    const bool absolute = caller_path.starts_with('/');
    std::vector<std::string> display;
    if (!absolute) {
        const auto cwd_components = split_components(frozen_cwd, true);
        for (const auto& component : cwd_components) {
            if (!component.empty()) {
                display.push_back(component);
            }
        }
    }
    for (const auto& component : split_components(caller_path, absolute)) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            if (!display.empty()) {
                display.pop_back();
            }
            continue;
        }
        display.push_back(component);
    }
    std::string result = "/";
    for (std::size_t index = 0; index < display.size(); ++index) {
        if (index != 0) {
            result.push_back('/');
        }
        result += display[index];
    }
    if (result.size() > 4096) {
        return std::nullopt;
    }
    return result;
}

bool canonical_absolute_directory(std::string_view value) {
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string_view::npos ||
        !value.starts_with('/') || !common::valid_utf8(value)) {
        return false;
    }
    if (value != "/" && value.ends_with('/')) {
        return false;
    }
    const auto normalized = canonical_display(value, "/");
    return normalized && *normalized == value;
}

std::optional<SourceLocator> make_locator(std::string_view caller_path,
                                          std::string_view frozen_cwd) {
    if (caller_path.empty() || caller_path.size() > 4096 ||
        caller_path.find('\0') != std::string::npos || caller_path.ends_with('/') ||
        !common::valid_utf8(caller_path)) {
        return std::nullopt;
    }
    const bool absolute = caller_path.starts_with('/');
    if (!absolute && !canonical_absolute_directory(frozen_cwd)) {
        return std::nullopt;
    }
    auto display = canonical_display(caller_path, frozen_cwd);
    if (!display) {
        return std::nullopt;
    }
    auto components = split_components(caller_path, absolute);
    if (components.empty() || !valid_basename(components.back())) {
        return std::nullopt;
    }
    SourceLocator locator;
    locator.absolute = absolute;
    locator.components = std::move(components);
    locator.display_path = std::move(*display);
    locator.name = locator.components.back();
    return locator;
}

FileSpoolError initial_component_error(int error) {
    switch (error) {
    case ENOENT:
        return source_error(SourceFileReason::Missing);
    case ELOOP:
        return source_error(SourceFileReason::Symlink);
    case ENOTDIR:
        return source_error(SourceFileReason::WrongType);
    case EACCES:
    case EPERM:
        return source_error(SourceFileReason::Unreadable);
    default:
        return durability_error(DurabilityReason::OpenFailed);
    }
}

FileSpoolError later_component_error(int error, DurabilityReason fallback) {
    switch (error) {
    case ENOENT:
    case ELOOP:
    case ENOTDIR:
    case EACCES:
    case EPERM:
        return simple_error(FileSpoolErrorKind::InputChanged);
    default:
        return durability_error(fallback);
    }
}

FileSpoolError component_error(SourcePass pass, int error, DurabilityReason fallback) {
    return pass == SourcePass::First ? initial_component_error(error)
                                     : later_component_error(error, fallback);
}

bool stat_entry(int parent, const std::string& component, struct stat& status,
                FileSpoolMetadata metadata,
                const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (::fstatat(parent, component.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return false;
    }
    mutate_metadata(hooks, metadata, status);
    return true;
}

std::variant<Descriptor, FileSpoolError>
capture_cwd(std::string_view frozen_cwd, const FileSpoolControl& control,
            const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    notify(hooks, FileSpoolStage::BeforeCwdCapture);
    if (const auto error = control_error(control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::BeforeCwdCapture)) {
        return simple_error(FileSpoolErrorKind::InvalidInput);
    }
    Descriptor current(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!current) {
        return simple_error(FileSpoolErrorKind::InvalidInput);
    }
    std::vector<DirectoryEdge> edges;
    for (const auto& component : split_components(frozen_cwd, true)) {
        if (component.empty()) {
            continue;
        }
        struct stat entry {};
        if (!stat_entry(current.get(), component, entry, FileSpoolMetadata::CwdEntry, hooks) ||
            !S_ISDIR(entry.st_mode) || S_ISLNK(entry.st_mode)) {
            return simple_error(FileSpoolErrorKind::InvalidInput);
        }
        Descriptor child(::openat(current.get(), component.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        struct stat child_status {};
        if (!child || ::fstat(child.get(), &child_status) != 0) {
            return simple_error(FileSpoolErrorKind::InvalidInput);
        }
        mutate_metadata(hooks, FileSpoolMetadata::CwdDescriptor, child_status);
        if (!same_directory(entry, child_status)) {
            return simple_error(FileSpoolErrorKind::InvalidInput);
        }
        edges.push_back(DirectoryEdge{Descriptor(::dup(current.get())),
                                      Descriptor(::dup(child.get())), component, child_status});
        current = std::move(child);
    }
    for (auto& edge : edges) {
        struct stat entry {};
        struct stat child {};
        if (::fstatat(edge.parent.get(), edge.component.c_str(), &entry, AT_SYMLINK_NOFOLLOW) !=
                0 ||
            ::fstat(edge.child.get(), &child) != 0 || !same_directory(entry, edge.child_status) ||
            !same_directory(child, edge.child_status)) {
            return simple_error(FileSpoolErrorKind::InvalidInput);
        }
    }
    notify(hooks, FileSpoolStage::AfterCwdCapture);
    if (const auto error = control_error(control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::AfterCwdCapture)) {
        return simple_error(FileSpoolErrorKind::InvalidInput);
    }
    return current;
}

std::variant<Descriptor, FileSpoolError> pass_start(const SourceLocator& locator,
                                                    const FileSpoolControl& control) {
    if (const auto error = control_error(control)) {
        return *error;
    }
    const int value = locator.absolute
                          ? ::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)
                          : ::dup(locator.cwd.get());
    if (value < 0) {
        return durability_error(DurabilityReason::OpenFailed);
    }
    return Descriptor(value);
}

std::optional<FileSpoolError>
revalidate_edges(std::vector<DirectoryEdge>& edges, SourcePass pass,
                 const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    for (auto& edge : edges) {
        struct stat entry {};
        struct stat child {};
        if (::fstatat(edge.parent.get(), edge.component.c_str(), &entry, AT_SYMLINK_NOFOLLOW) !=
            0) {
            return component_error(pass, errno, DurabilityReason::ReadFailed);
        }
        mutate_metadata(hooks, FileSpoolMetadata::DirectoryEntry, entry);
        if (::fstat(edge.child.get(), &child) != 0) {
            return durability_error(DurabilityReason::ReadFailed);
        }
        mutate_metadata(hooks, FileSpoolMetadata::DirectoryDescriptor, child);
        if (S_ISLNK(entry.st_mode) || !same_directory(entry, edge.child_status) ||
            !same_directory(child, edge.child_status)) {
            return simple_error(FileSpoolErrorKind::InputChanged);
        }
    }
    return std::nullopt;
}

ssize_t read_bytes(FileSpoolIo operation, int descriptor, void* data, std::size_t size,
                   const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (hooks && hooks->read) {
        return hooks->read(operation, descriptor, data, size);
    }
    return ::read(descriptor, data, size);
}

ssize_t write_bytes(int descriptor, const void* data, std::size_t size,
                    const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (hooks && hooks->write) {
        return hooks->write(descriptor, data, size);
    }
    return ::write(descriptor, data, size);
}

int sync_descriptor(FileSpoolStage stage, int descriptor,
                    const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    notify(hooks, stage);
    if (injected(hooks, stage)) {
        errno = EIO;
        return -1;
    }
    if (hooks && hooks->sync) {
        return hooks->sync(stage, descriptor);
    }
    return ::fsync(descriptor);
}

std::optional<FileSpoolError>
write_all(int descriptor, const unsigned char* bytes, std::size_t size,
          const FileSpoolControl& control,
          const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    std::size_t written = 0;
    while (written < size) {
        if (const auto error = control_error(control)) {
            return error;
        }
        notify(hooks, FileSpoolStage::DuringDestinationWrite);
        if (const auto error = control_error(control)) {
            return error;
        }
        if (injected(hooks, FileSpoolStage::DuringDestinationWrite)) {
            return durability_error(DurabilityReason::WriteFailed);
        }
        const auto result = write_bytes(descriptor, bytes + written, size - written, hooks);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return durability_error(errno == ENOSPC || errno == EDQUOT
                                        ? DurabilityReason::CapacityExhausted
                                        : DurabilityReason::WriteFailed);
        }
        if (result == 0) {
            return durability_error(DurabilityReason::WriteFailed);
        }
        written += static_cast<std::size_t>(result);
    }
    return std::nullopt;
}

// Opening, streaming and post-read revalidation form one fail-closed source-pass boundary.
// NOLINTBEGIN(readability-function-cognitive-complexity)
std::variant<FileSnapshot, FileSpoolError>
run_source_pass(const SourceLocator& locator, SourcePass pass, const FileSnapshot* expected,
                int destination, const FileSpoolControl& control,
                const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    auto start = pass_start(locator, control);
    if (auto* error = std::get_if<FileSpoolError>(&start)) {
        return *error;
    }
    Descriptor current = std::move(std::get<Descriptor>(start));
    struct stat start_status {};
    if (::fstat(current.get(), &start_status) != 0 || !S_ISDIR(start_status.st_mode)) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    std::vector<DirectoryEdge> edges;
    for (std::size_t index = 0; index + 1 < locator.components.size(); ++index) {
        const auto& component = locator.components[index];
        if (component.empty()) {
            continue;
        }
        if (const auto error = control_error(control)) {
            return *error;
        }
        notify(hooks, FileSpoolStage::BeforeSourceEntryStat);
        if (const auto error = control_error(control)) {
            return *error;
        }
        if (injected(hooks, FileSpoolStage::BeforeSourceEntryStat)) {
            return durability_error(DurabilityReason::OpenFailed);
        }
        struct stat entry {};
        if (!stat_entry(current.get(), component, entry, FileSpoolMetadata::DirectoryEntry,
                        hooks)) {
            return component_error(pass, errno, DurabilityReason::OpenFailed);
        }
        notify(hooks, FileSpoolStage::AfterSourceEntryStat);
        if (const auto error = control_error(control)) {
            return *error;
        }
        if (S_ISLNK(entry.st_mode)) {
            return pass == SourcePass::First ? source_error(SourceFileReason::Symlink)
                                             : simple_error(FileSpoolErrorKind::InputChanged);
        }
        if (!S_ISDIR(entry.st_mode)) {
            return pass == SourcePass::First ? source_error(SourceFileReason::WrongType)
                                             : simple_error(FileSpoolErrorKind::InputChanged);
        }
        Descriptor child(::openat(current.get(), component.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        if (!child) {
            return component_error(pass, errno, DurabilityReason::OpenFailed);
        }
        struct stat child_status {};
        if (::fstat(child.get(), &child_status) != 0) {
            return durability_error(DurabilityReason::ReadFailed);
        }
        mutate_metadata(hooks, FileSpoolMetadata::DirectoryDescriptor, child_status);
        if (!same_directory(entry, child_status)) {
            return simple_error(FileSpoolErrorKind::InputChanged);
        }
        edges.push_back(DirectoryEdge{Descriptor(::dup(current.get())),
                                      Descriptor(::dup(child.get())), component, child_status});
        if (!edges.back().parent || !edges.back().child) {
            return durability_error(DurabilityReason::OpenFailed);
        }
        current = std::move(child);
    }

    if (const auto error = control_error(control)) {
        return *error;
    }
    const auto& target = locator.components.back();
    notify(hooks, FileSpoolStage::BeforeSourceEntryStat);
    if (const auto error = control_error(control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::BeforeSourceEntryStat)) {
        return durability_error(DurabilityReason::OpenFailed);
    }
    struct stat entry {};
    if (!stat_entry(current.get(), target, entry, FileSpoolMetadata::SourceEntry, hooks)) {
        return component_error(pass, errno, DurabilityReason::OpenFailed);
    }
    notify(hooks, FileSpoolStage::AfterSourceEntryStat);
    if (const auto error = control_error(control)) {
        return *error;
    }
    if (S_ISLNK(entry.st_mode)) {
        return pass == SourcePass::First ? source_error(SourceFileReason::Symlink)
                                         : simple_error(FileSpoolErrorKind::InputChanged);
    }
    if (!S_ISREG(entry.st_mode)) {
        return pass == SourcePass::First ? source_error(SourceFileReason::WrongType)
                                         : simple_error(FileSpoolErrorKind::InputChanged);
    }
    const Descriptor source(
        ::openat(current.get(), target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!source) {
        return component_error(pass, errno, DurabilityReason::OpenFailed);
    }
    struct stat before {};
    if (::fstat(source.get(), &before) != 0) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    mutate_metadata(hooks, FileSpoolMetadata::SourceBefore, before);
    if (!S_ISREG(before.st_mode) || !same_inode(entry, before)) {
        return simple_error(FileSpoolErrorKind::InputChanged);
    }
    auto maybe_snapshot = snapshot_from_status(locator, before);
    if (!maybe_snapshot) {
        return durability_error(DurabilityReason::SchemaError);
    }
    auto snapshot = std::move(*maybe_snapshot);
    if (pass == SourcePass::First && snapshot.size == 0) {
        return source_error(SourceFileReason::Empty);
    }
    if (expected != nullptr && !same_snapshot_identity(*expected, before)) {
        return simple_error(FileSpoolErrorKind::InputChanged);
    }
    notify(hooks, FileSpoolStage::AfterSourceOpen);
    if (const auto error = control_error(control)) {
        return *error;
    }

    Sha256 digest;
    std::array<unsigned char, kBufferSize> buffer{};
    std::uint64_t total = 0;
    const auto io = pass == SourcePass::First ? FileSpoolIo::Pass1Read : FileSpoolIo::Pass2Read;
    const auto stage = pass == SourcePass::First ? FileSpoolStage::DuringPass1Read
                                                 : FileSpoolStage::DuringPass2Read;
    while (total < snapshot.size) {
        if (const auto error = control_error(control)) {
            return *error;
        }
        notify(hooks, stage);
        if (const auto error = control_error(control)) {
            return *error;
        }
        if (injected(hooks, stage)) {
            return durability_error(DurabilityReason::ReadFailed);
        }
        const auto remaining = snapshot.size - total;
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        const auto count = read_bytes(io, source.get(), buffer.data(), requested, hooks);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return durability_error(DurabilityReason::ReadFailed);
        }
        if (count == 0) {
            return simple_error(FileSpoolErrorKind::InputChanged);
        }
        const auto converted = static_cast<std::size_t>(count);
        if (converted > requested) {
            return durability_error(DurabilityReason::ReadFailed);
        }
        digest.update(buffer.data(), converted);
        if (destination >= 0) {
            if (const auto error =
                    write_all(destination, buffer.data(), converted, control, hooks)) {
                return *error;
            }
        }
        total += converted;
    }
    for (;;) {
        if (const auto error = control_error(control)) {
            return *error;
        }
        notify(hooks, stage);
        if (const auto error = control_error(control)) {
            return *error;
        }
        unsigned char extra = 0;
        const auto count = read_bytes(io, source.get(), &extra, 1, hooks);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return durability_error(DurabilityReason::ReadFailed);
        }
        if (count != 0) {
            return simple_error(FileSpoolErrorKind::InputChanged);
        }
        break;
    }

    notify(hooks, FileSpoolStage::AfterSourceRead);
    if (const auto error = control_error(control)) {
        return *error;
    }
    struct stat after {};
    if (::fstat(source.get(), &after) != 0) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    mutate_metadata(hooks, FileSpoolMetadata::SourceAfter, after);
    if (!same_snapshot_identity(snapshot, after)) {
        return simple_error(FileSpoolErrorKind::InputChanged);
    }

    notify(hooks, FileSpoolStage::BeforeSourceRevalidate);
    if (const auto error = control_error(control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::BeforeSourceRevalidate)) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    if (const auto error = revalidate_edges(edges, pass, hooks)) {
        return *error;
    }
    struct stat final_entry {};
    struct stat final_source {};
    if (::fstatat(current.get(), target.c_str(), &final_entry, AT_SYMLINK_NOFOLLOW) != 0) {
        return component_error(pass, errno, DurabilityReason::ReadFailed);
    }
    mutate_metadata(hooks, FileSpoolMetadata::SourceEntry, final_entry);
    if (::fstat(source.get(), &final_source) != 0) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    mutate_metadata(hooks, FileSpoolMetadata::SourceAfter, final_source);
    if (!S_ISREG(final_entry.st_mode) || !same_inode(final_entry, final_source) ||
        !same_snapshot_identity(snapshot, final_source)) {
        return simple_error(FileSpoolErrorKind::InputChanged);
    }

    snapshot.sha256 = "sha256:" + digest.finish();
    if (expected != nullptr && snapshot != *expected) {
        return simple_error(FileSpoolErrorKind::InputChanged);
    }
    if (const auto error = control_error(control)) {
        return *error;
    }
    return snapshot;
}
// NOLINTEND(readability-function-cognitive-complexity)

DurabilityReason root_reason(const struct stat& status, uid_t expected_uid) {
    if (!S_ISDIR(status.st_mode)) {
        return DurabilityReason::WrongType;
    }
    if (status.st_uid != expected_uid) {
        return DurabilityReason::WrongOwner;
    }
    if ((status.st_mode & 07777) != 0700) {
        return DurabilityReason::WrongMode;
    }
    return DurabilityReason::Contradiction;
}

// Every account-state component is checked before advancing the retained descriptor.
// NOLINTBEGIN(readability-function-cognitive-complexity)
std::variant<Descriptor, FileSpoolError>
open_account_state(std::string_view account_state, uid_t expected_uid,
                   const FileSpoolControl& control,
                   const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (const auto error =
            notify_controlled(hooks, FileSpoolStage::BeforeAccountStateOpen, control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::BeforeAccountStateOpen) ||
        !canonical_absolute_directory(account_state) || account_state == "/") {
        return durability_error(DurabilityReason::PathInvalid);
    }
    Descriptor current(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!current) {
        return durability_error(DurabilityReason::OpenFailed);
    }
    const auto components = split_components(account_state, true);
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto& component = components[index];
        if (component.empty()) {
            continue;
        }
        struct stat entry {};
        if (::fstatat(current.get(), component.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
            return errno == ELOOP || errno == ENOENT || errno == ENOTDIR
                       ? durability_error(DurabilityReason::PathInvalid)
                       : durability_error(DurabilityReason::OpenFailed);
        }
        if (S_ISLNK(entry.st_mode)) {
            return durability_error(DurabilityReason::PathInvalid);
        }
        if (!S_ISDIR(entry.st_mode)) {
            return durability_error(DurabilityReason::WrongType);
        }
        Descriptor child(::openat(current.get(), component.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        if (!child) {
            return errno == ELOOP || errno == ENOENT || errno == ENOTDIR
                       ? durability_error(DurabilityReason::PathInvalid)
                       : durability_error(DurabilityReason::OpenFailed);
        }
        struct stat opened {};
        if (::fstat(child.get(), &opened) != 0) {
            return durability_error(DurabilityReason::OpenFailed);
        }
        if (!same_directory(entry, opened)) {
            return durability_error(DurabilityReason::PathInvalid);
        }
        current = std::move(child);
        if (index + 1 == components.size()) {
            if (opened.st_uid != expected_uid) {
                return durability_error(DurabilityReason::WrongOwner);
            }
            if ((opened.st_mode & 07777) != 0700) {
                return durability_error(DurabilityReason::WrongMode);
            }
        }
    }
    if (const auto error =
            notify_controlled(hooks, FileSpoolStage::AfterAccountStateOpen, control)) {
        return *error;
    }
    return current;
}
// NOLINTEND(readability-function-cognitive-complexity)

// Root absence, creation races and unsafe pre-existing objects share one classification loop.
// NOLINTBEGIN(readability-function-cognitive-complexity)
std::variant<RootHandle, FileSpoolError>
open_spool_root(std::string account_state, uid_t expected_uid, bool create,
                const FileSpoolControl& control,
                const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (const auto error = control_error(control)) {
        return *error;
    }
    auto account = open_account_state(account_state, expected_uid, control, hooks);
    if (const auto* error = std::get_if<FileSpoolError>(&account)) {
        return *error;
    }
    RootHandle handle;
    handle.account_state = std::move(std::get<Descriptor>(account));
    handle.account_state_path = std::move(account_state);

    for (;;) {
        if (const auto error = control_error(control)) {
            return *error;
        }
        if (const auto error =
                notify_controlled(hooks, FileSpoolStage::BeforeRootInspect, control)) {
            return *error;
        }
        if (injected(hooks, FileSpoolStage::BeforeRootInspect)) {
            return durability_error(DurabilityReason::OpenFailed);
        }
        struct stat entry {};
        if (::fstatat(handle.account_state.get(), kSpoolName.data(), &entry, AT_SYMLINK_NOFOLLOW) !=
            0) {
            if (errno != ENOENT) {
                return durability_error(DurabilityReason::OpenFailed);
            }
            if (!create) {
                handle.absent = true;
                return handle;
            }
            if (const auto error =
                    notify_controlled(hooks, FileSpoolStage::BeforeRootCreate, control)) {
                return *error;
            }
            if (injected(hooks, FileSpoolStage::BeforeRootCreate)) {
                return durability_error(DurabilityReason::WriteFailed);
            }
            if (::mkdirat(handle.account_state.get(), kSpoolName.data(), 0700) != 0) {
                if (errno == EEXIST) {
                    continue;
                }
                return durability_error(errno == ENOSPC || errno == EDQUOT
                                            ? DurabilityReason::CapacityExhausted
                                            : DurabilityReason::WriteFailed);
            }
            if (::fchmodat(handle.account_state.get(), kSpoolName.data(), 0700,
                           AT_SYMLINK_NOFOLLOW) != 0) {
                return durability_error(DurabilityReason::WriteFailed);
            }
            Descriptor created(::openat(handle.account_state.get(), kSpoolName.data(),
                                        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            struct stat created_status {};
            struct stat created_entry {};
            if (!created || ::fchmod(created.get(), 0700) != 0 ||
                ::fstat(created.get(), &created_status) != 0 ||
                ::fstatat(handle.account_state.get(), kSpoolName.data(), &created_entry,
                          AT_SYMLINK_NOFOLLOW) != 0 ||
                !same_directory(created_entry, created_status) ||
                !valid_private_directory(created_status, expected_uid)) {
                return durability_error(DurabilityReason::PathInvalid);
            }
            notify(hooks, FileSpoolStage::AfterRootCreate);
            if (sync_descriptor(FileSpoolStage::BeforeAccountStateSync, handle.account_state.get(),
                                hooks) != 0) {
                return durability_error(DurabilityReason::DirectorySyncFailed);
            }
            if (const auto error = control_error(control)) {
                return *error;
            }
            handle.root = std::move(created);
            handle.root_status = created_status;
            return handle;
        }
        mutate_metadata(hooks, FileSpoolMetadata::RootEntry, entry);
        if (const auto error =
                notify_controlled(hooks, FileSpoolStage::AfterRootEntryStat, control)) {
            return *error;
        }
        if (S_ISLNK(entry.st_mode)) {
            return durability_error(DurabilityReason::PathInvalid);
        }
        if (!valid_private_directory(entry, expected_uid)) {
            return durability_error(root_reason(entry, expected_uid));
        }
        Descriptor opened(::openat(handle.account_state.get(), kSpoolName.data(),
                                   O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        if (!opened) {
            return durability_error(errno == ELOOP || errno == ENOENT || errno == ENOTDIR
                                        ? DurabilityReason::PathInvalid
                                        : DurabilityReason::OpenFailed);
        }
        struct stat opened_status {};
        if (::fstat(opened.get(), &opened_status) != 0) {
            return durability_error(DurabilityReason::OpenFailed);
        }
        mutate_metadata(hooks, FileSpoolMetadata::RootDescriptor, opened_status);
        if (const auto error = notify_controlled(hooks, FileSpoolStage::AfterRootOpen, control)) {
            return *error;
        }
        if (!same_directory(entry, opened_status)) {
            return durability_error(DurabilityReason::PathInvalid);
        }
        if (!valid_private_directory(opened_status, expected_uid)) {
            return durability_error(root_reason(opened_status, expected_uid));
        }
        if (create && sync_descriptor(FileSpoolStage::BeforeAccountStateSync,
                                      handle.account_state.get(), hooks) != 0) {
            return durability_error(DurabilityReason::DirectorySyncFailed);
        }
        if (create) {
            if (const auto error = control_error(control)) {
                return *error;
            }
        }
        handle.root = std::move(opened);
        handle.root_status = opened_status;
        return handle;
    }
}
// NOLINTEND(readability-function-cognitive-complexity)

std::string relative_spool_path(std::string_view invocation_id, std::string_view name) {
    return std::string(kSpoolName) + "/" + std::string(invocation_id) + "/" + std::string(name);
}

std::optional<std::pair<std::string, std::string>> parse_spool_path(const SpoolRef& reference) {
    const auto first = reference.relative_path.find('/');
    const auto second = first == std::string::npos ? std::string::npos
                                                   : reference.relative_path.find('/', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        reference.relative_path.find('/', second + 1) != std::string::npos ||
        reference.relative_path.substr(0, first) != kSpoolName) {
        return std::nullopt;
    }
    auto invocation = reference.relative_path.substr(first + 1, second - first - 1);
    auto name = reference.relative_path.substr(second + 1);
    if (!valid_invocation_id(invocation) || !valid_basename(name) || name != reference.file.name) {
        return std::nullopt;
    }
    return std::pair{std::move(invocation), std::move(name)};
}

FileSpoolError with_cleanup(FileSpoolError error, const SpoolRef& reference) {
    if (error.kind != FileSpoolErrorKind::Contradiction) {
        error.cleanup_reference = reference;
    }
    return error;
}

std::variant<InvocationHandle, FileSpoolError>
create_invocation_directory(RootHandle& root, std::string_view invocation_id, uid_t expected_uid,
                            const FileSpoolControl& control,
                            const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (const auto error =
            notify_controlled(hooks, FileSpoolStage::BeforeInvocationCreate, control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::BeforeInvocationCreate)) {
        return durability_error(DurabilityReason::WriteFailed);
    }
    if (::mkdirat(root.root.get(), std::string(invocation_id).c_str(), 0700) != 0) {
        if (errno == EEXIST) {
            return contradiction_error();
        }
        return durability_error(errno == ENOSPC || errno == EDQUOT
                                    ? DurabilityReason::CapacityExhausted
                                    : DurabilityReason::WriteFailed);
    }
    if (::fchmodat(root.root.get(), std::string(invocation_id).c_str(), 0700,
                   AT_SYMLINK_NOFOLLOW) != 0) {
        return durability_error(DurabilityReason::WriteFailed);
    }
    Descriptor invocation(::openat(root.root.get(), std::string(invocation_id).c_str(),
                                   O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat entry {};
    struct stat opened {};
    if (!invocation || ::fchmod(invocation.get(), 0700) != 0 ||
        ::fstatat(root.root.get(), std::string(invocation_id).c_str(), &entry,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(invocation.get(), &opened) != 0) {
        return durability_error(DurabilityReason::PathInvalid);
    }
    mutate_metadata(hooks, FileSpoolMetadata::InvocationEntry, entry);
    mutate_metadata(hooks, FileSpoolMetadata::InvocationDescriptor, opened);
    if (!same_directory(entry, opened) || !valid_private_directory(opened, expected_uid)) {
        return contradiction_error();
    }
    notify(hooks, FileSpoolStage::AfterInvocationCreate);
    if (sync_descriptor(FileSpoolStage::BeforeRootSync, root.root.get(), hooks) != 0) {
        return durability_error(DurabilityReason::DirectorySyncFailed);
    }
    if (const auto error = control_error(control)) {
        return *error;
    }
    return InvocationHandle{.descriptor = std::move(invocation), .status = opened};
}

std::variant<DestinationHandle, FileSpoolError>
create_destination(int invocation, std::string_view name, uid_t expected_uid,
                   const FileSpoolControl& control,
                   const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (const auto error = control_error(control)) {
        return *error;
    }
    const long name_max = hooks && hooks->name_max ? hooks->name_max(invocation)
                                                   : ::fpathconf(invocation, _PC_NAME_MAX);
    if (name_max < 0 || static_cast<std::uint64_t>(name_max) < name.size()) {
        return durability_error(DurabilityReason::PathInvalid);
    }
    if (const auto error =
            notify_controlled(hooks, FileSpoolStage::BeforeDestinationCreate, control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::BeforeDestinationCreate)) {
        return durability_error(DurabilityReason::WriteFailed);
    }
    Descriptor destination(::openat(invocation, std::string(name).c_str(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!destination) {
        if (errno == EEXIST || errno == ELOOP) {
            return contradiction_error();
        }
        return durability_error(errno == ENOSPC || errno == EDQUOT
                                    ? DurabilityReason::CapacityExhausted
                                    : DurabilityReason::WriteFailed);
    }
    if (::fchmod(destination.get(), 0600) != 0) {
        return durability_error(DurabilityReason::WriteFailed);
    }
    struct stat opened {};
    if (::fstat(destination.get(), &opened) != 0 || !valid_private_file(opened, expected_uid)) {
        return durability_error(DurabilityReason::PathInvalid);
    }
    if (const auto error =
            notify_controlled(hooks, FileSpoolStage::AfterDestinationCreate, control)) {
        return *error;
    }
    return DestinationHandle{.descriptor = std::move(destination), .status = opened};
}

bool stable_file_identity(const struct stat& before, const struct stat& after) {
    const auto before_times = file_times(before);
    const auto after_times = file_times(after);
    return before_times && after_times && same_inode(before, after) &&
           same_file_type(before, after) && before.st_size == after.st_size &&
           before.st_mode == after.st_mode && before.st_uid == after.st_uid &&
           before.st_nlink == after.st_nlink && *before_times == *after_times;
}

std::optional<FileSpoolError> revalidate_publication(
    RootHandle& root, std::string_view invocation_id, const InvocationHandle& invocation,
    std::string_view name, const DestinationHandle& destination, const FileSnapshot& expected,
    uid_t expected_uid, const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    notify(hooks, FileSpoolStage::BeforeDestinationRevalidate);

    struct stat root_entry {};
    struct stat root_opened {};
    struct stat invocation_entry {};
    struct stat invocation_opened {};
    struct stat destination_entry {};
    struct stat destination_opened {};
    if (::fstatat(root.account_state.get(), kSpoolName.data(), &root_entry, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        ::fstat(root.root.get(), &root_opened) != 0 ||
        ::fstatat(root.root.get(), std::string(invocation_id).c_str(), &invocation_entry,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(invocation.descriptor.get(), &invocation_opened) != 0 ||
        ::fstatat(invocation.descriptor.get(), std::string(name).c_str(), &destination_entry,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(destination.descriptor.get(), &destination_opened) != 0) {
        return contradiction_error();
    }
    mutate_metadata(hooks, FileSpoolMetadata::RootEntry, root_entry);
    mutate_metadata(hooks, FileSpoolMetadata::RootDescriptor, root_opened);
    mutate_metadata(hooks, FileSpoolMetadata::InvocationEntry, invocation_entry);
    mutate_metadata(hooks, FileSpoolMetadata::InvocationDescriptor, invocation_opened);
    mutate_metadata(hooks, FileSpoolMetadata::DestinationEntry, destination_entry);
    mutate_metadata(hooks, FileSpoolMetadata::DestinationDescriptor, destination_opened);
    if (!same_directory(root.root_status, root_entry) ||
        !same_directory(root.root_status, root_opened) ||
        !valid_private_directory(root_entry, expected_uid) ||
        !valid_private_directory(root_opened, expected_uid) ||
        !same_directory(invocation.status, invocation_entry) ||
        !same_directory(invocation.status, invocation_opened) ||
        !valid_private_directory(invocation_entry, expected_uid) ||
        !valid_private_directory(invocation_opened, expected_uid) ||
        !same_inode(destination.status, destination_entry) ||
        !same_inode(destination.status, destination_opened) ||
        !valid_private_file(destination_entry, expected_uid) ||
        !valid_private_file(destination_opened, expected_uid) || destination_opened.st_size < 0 ||
        static_cast<std::uint64_t>(destination_opened.st_size) != expected.size) {
        return contradiction_error();
    }

    const Descriptor reader(::openat(invocation.descriptor.get(), std::string(name).c_str(),
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat reader_before {};
    if (!reader || ::fstat(reader.get(), &reader_before) != 0 ||
        !same_inode(destination.status, reader_before) ||
        !valid_private_file(reader_before, expected_uid) || reader_before.st_size < 0 ||
        static_cast<std::uint64_t>(reader_before.st_size) != expected.size) {
        return contradiction_error();
    }

    Sha256 digest;
    std::array<unsigned char, kBufferSize> buffer{};
    std::uint64_t total = 0;
    while (total < expected.size) {
        const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            expected.size - total, static_cast<std::uint64_t>(buffer.size())));
        const auto count = ::read(reader.get(), buffer.data(), requested);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0 || static_cast<std::size_t>(count) > requested) {
            return contradiction_error();
        }
        digest.update(buffer.data(), static_cast<std::size_t>(count));
        total += static_cast<std::uint64_t>(count);
    }
    unsigned char extra = 0;
    ssize_t extra_count = -1;
    for (;;) {
        extra_count = ::read(reader.get(), &extra, 1);
        if (extra_count >= 0 || errno != EINTR) {
            break;
        }
    }
    if (extra_count != 0 || "sha256:" + digest.finish() != expected.sha256) {
        return contradiction_error();
    }

    struct stat final_root_entry {};
    struct stat final_root_opened {};
    struct stat final_invocation_entry {};
    struct stat final_invocation_opened {};
    struct stat final_destination_entry {};
    struct stat final_destination_opened {};
    struct stat reader_after {};
    if (::fstatat(root.account_state.get(), kSpoolName.data(), &final_root_entry,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(root.root.get(), &final_root_opened) != 0 ||
        ::fstatat(root.root.get(), std::string(invocation_id).c_str(), &final_invocation_entry,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(invocation.descriptor.get(), &final_invocation_opened) != 0 ||
        ::fstatat(invocation.descriptor.get(), std::string(name).c_str(), &final_destination_entry,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstat(destination.descriptor.get(), &final_destination_opened) != 0 ||
        ::fstat(reader.get(), &reader_after) != 0 ||
        !same_directory(root_entry, final_root_entry) ||
        !same_directory(root_opened, final_root_opened) ||
        !valid_private_directory(final_root_entry, expected_uid) ||
        !valid_private_directory(final_root_opened, expected_uid) ||
        !same_directory(invocation_entry, final_invocation_entry) ||
        !same_directory(invocation_opened, final_invocation_opened) ||
        !valid_private_directory(final_invocation_entry, expected_uid) ||
        !valid_private_directory(final_invocation_opened, expected_uid) ||
        !stable_file_identity(destination_entry, final_destination_entry) ||
        !stable_file_identity(destination_opened, final_destination_opened) ||
        !stable_file_identity(reader_before, reader_after) ||
        !same_inode(final_destination_entry, final_destination_opened) ||
        !same_inode(final_destination_opened, reader_after) ||
        !valid_private_file(final_destination_entry, expected_uid) ||
        !valid_private_file(final_destination_opened, expected_uid)) {
        return contradiction_error();
    }
    return std::nullopt;
}

std::variant<std::vector<std::string>, FileSpoolError>
directory_names(int directory, const FileSpoolControl& control,
                const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    notify(hooks, FileSpoolStage::BeforeRootEnumeration);
    if (const auto error = control_error(control)) {
        return *error;
    }
    if (injected(hooks, FileSpoolStage::BeforeRootEnumeration)) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    const int duplicate = ::dup(directory);
    if (duplicate < 0) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    DIR* stream = ::fdopendir(duplicate);
    if (stream == nullptr) {
        ::close(duplicate);
        return durability_error(DurabilityReason::ReadFailed);
    }
    std::vector<std::string> result;
    errno = 0;
    while (const auto* entry = ::readdir(stream)) {
        notify(hooks, FileSpoolStage::DuringRootEnumeration);
        if (const auto error = control_error(control)) {
            ::closedir(stream);
            return *error;
        }
        if (injected(hooks, FileSpoolStage::DuringRootEnumeration)) {
            ::closedir(stream);
            return durability_error(DurabilityReason::ReadFailed);
        }
        const std::string_view name(entry->d_name);
        if (name != "." && name != "..") {
            result.emplace_back(name);
        }
    }
    const int read_error = errno;
    ::closedir(stream);
    if (read_error != 0) {
        return durability_error(DurabilityReason::ReadFailed);
    }
    std::ranges::sort(result, unsigned_bytes_less);
    return result;
}

std::optional<FilesystemDiagnosticPath> diagnostic_for(std::string_view account_state,
                                                       std::string_view suffix) {
    std::string bytes(account_state);
    bytes += "/spool";
    bytes += suffix;
    return encode_filesystem_diagnostic_path(bytes);
}

std::string diagnostic_suffix(std::string_view invocation, std::string_view file = {}) {
    std::string result = "/";
    result += invocation;
    if (!file.empty()) {
        result += "/";
        result += file;
    }
    return result;
}

void select_contradiction(std::optional<FilesystemDiagnosticPath>& selected,
                          const FilesystemDiagnosticPath& candidate) {
    if (!selected || candidate.bytes_hex < selected->bytes_hex) {
        selected = candidate;
    }
}

// Enumeration retains unsafe entries and selects one raw-byte-ordered contradiction.
// NOLINTBEGIN(readability-function-cognitive-complexity)
std::variant<SpoolInventory, FileSpoolError>
enumerate_open_root(RootHandle& root, uid_t expected_uid, const FileSpoolControl& control,
                    const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    SpoolInventory inventory;
    auto root_names = directory_names(root.root.get(), control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&root_names)) {
        return *error;
    }
    for (const auto& invocation_name : std::get<std::vector<std::string>>(root_names)) {
        const auto directory_diagnostic =
            diagnostic_for(root.account_state_path, diagnostic_suffix(invocation_name));
        if (!directory_diagnostic) {
            return durability_error(DurabilityReason::SchemaError);
        }
        const auto& directory_path = *directory_diagnostic;
        if (!valid_invocation_id(invocation_name)) {
            select_contradiction(inventory.contradiction, directory_path);
            continue;
        }
        struct stat entry {};
        if (::fstatat(root.root.get(), invocation_name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
            return durability_error(DurabilityReason::ReadFailed);
        }
        mutate_metadata(hooks, FileSpoolMetadata::InvocationEntry, entry);
        if (S_ISLNK(entry.st_mode) || !valid_private_directory(entry, expected_uid)) {
            select_contradiction(inventory.contradiction, directory_path);
            continue;
        }
        const Descriptor invocation(::openat(root.root.get(), invocation_name.c_str(),
                                             O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        struct stat opened {};
        if (!invocation || ::fstat(invocation.get(), &opened) != 0) {
            select_contradiction(inventory.contradiction, directory_path);
            continue;
        }
        mutate_metadata(hooks, FileSpoolMetadata::InvocationDescriptor, opened);
        if (!same_directory(entry, opened) || !valid_private_directory(opened, expected_uid)) {
            select_contradiction(inventory.contradiction, directory_path);
            continue;
        }
        auto child_names = directory_names(invocation.get(), control, hooks);
        if (auto* error = std::get_if<FileSpoolError>(&child_names)) {
            return *error;
        }
        auto& children = std::get<std::vector<std::string>>(child_names);
        SpoolInvocationObservation observation;
        observation.invocation_id = invocation_name;
        observation.directory_path = directory_path;
        for (const auto& child_name : children) {
            const auto child_diagnostic = diagnostic_for(
                root.account_state_path, diagnostic_suffix(invocation_name, child_name));
            if (!child_diagnostic) {
                return durability_error(DurabilityReason::SchemaError);
            }
            struct stat child_entry {};
            if (::fstatat(invocation.get(), child_name.c_str(), &child_entry,
                          AT_SYMLINK_NOFOLLOW) != 0) {
                return durability_error(DurabilityReason::ReadFailed);
            }
            const Descriptor child(::openat(invocation.get(), child_name.c_str(),
                                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            struct stat child_opened {};
            if (!valid_basename(child_name) || !child || ::fstat(child.get(), &child_opened) != 0) {
                select_contradiction(inventory.contradiction, *child_diagnostic);
            } else {
                mutate_metadata(hooks, FileSpoolMetadata::DestinationEntry, child_entry);
                mutate_metadata(hooks, FileSpoolMetadata::DestinationDescriptor, child_opened);
                if (!same_inode(child_entry, child_opened) ||
                    !valid_private_file(child_entry, expected_uid) ||
                    !valid_private_file(child_opened, expected_uid)) {
                    select_contradiction(inventory.contradiction, *child_diagnostic);
                } else {
                    observation.files.push_back(
                        SpoolInvocationObservation::File{child_name, *child_diagnostic});
                }
            }
        }
        if (observation.files.size() == 1) {
            observation.file_name = observation.files.front().name;
            observation.file_path = observation.files.front().path;
        }
        inventory.invocations.push_back(std::move(observation));
        struct stat final_entry {};
        struct stat final_opened {};
        if (::fstatat(root.root.get(), invocation_name.c_str(), &final_entry,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            ::fstat(invocation.get(), &final_opened) != 0 || !same_directory(entry, final_entry) ||
            !same_directory(opened, final_opened)) {
            select_contradiction(inventory.contradiction, directory_path);
        }
    }
    struct stat root_entry {};
    struct stat root_opened {};
    if (::fstatat(root.account_state.get(), kSpoolName.data(), &root_entry, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        ::fstat(root.root.get(), &root_opened) != 0 || !same_directory(root_entry, root_opened) ||
        !valid_private_directory(root_entry, expected_uid) ||
        !valid_private_directory(root_opened, expected_uid)) {
        return durability_error(DurabilityReason::PathInvalid);
    }
    return inventory;
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace

struct PreparedSource::Impl {
    SourceLocator locator;
    FileSnapshot snapshot;
};

PreparedSource::PreparedSource(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

PreparedSource::PreparedSource(PreparedSource&&) noexcept = default;
PreparedSource& PreparedSource::operator=(PreparedSource&&) noexcept = default;
PreparedSource::~PreparedSource() = default;

const FileSnapshot& PreparedSource::snapshot() const {
    return implementation_->snapshot;
}

PrepareSpoolSourceResult
prepare_spool_source(std::string_view caller_path, std::string_view frozen_cwd,
                     const FileSpoolControl& control,
                     const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (const auto error = control_error(control)) {
        return *error;
    }
    auto locator = make_locator(caller_path, frozen_cwd);
    if (!locator) {
        return simple_error(FileSpoolErrorKind::InvalidInput);
    }
    if (!locator->absolute) {
        auto cwd = capture_cwd(frozen_cwd, control, hooks);
        if (auto* error = std::get_if<FileSpoolError>(&cwd)) {
            if (error->kind == FileSpoolErrorKind::TimedOut ||
                error->kind == FileSpoolErrorKind::Cancelled) {
                return *error;
            }
            return simple_error(FileSpoolErrorKind::InvalidInput);
        }
        locator->cwd = std::move(std::get<Descriptor>(cwd));
    }
    auto pass = run_source_pass(*locator, SourcePass::First, nullptr, -1, control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&pass)) {
        return *error;
    }
    auto implementation = std::make_unique<PreparedSource::Impl>();
    implementation->locator = std::move(*locator);
    implementation->snapshot = std::move(std::get<FileSnapshot>(pass));
    return PreparedSource(std::move(implementation));
}

CreateSpoolFileResult
create_spool_file(PreparedSource& source, std::string account_state, std::string_view invocation_id,
                  uid_t expected_uid, const FileSpoolControl& control,
                  const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (!source.implementation_ || !valid_invocation_id(invocation_id)) {
        return simple_error(FileSpoolErrorKind::InvalidInput);
    }
    const SpoolRef reference{.relative_path =
                                 relative_spool_path(invocation_id, source.snapshot().name),
                             .file = source.snapshot()};
    auto root_result =
        open_spool_root(std::move(account_state), expected_uid, true, control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&root_result)) {
        return *error;
    }
    auto root = std::move(std::get<RootHandle>(root_result));
    auto invocation_result =
        create_invocation_directory(root, invocation_id, expected_uid, control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&invocation_result)) {
        return with_cleanup(*error, reference);
    }
    auto invocation = std::move(std::get<InvocationHandle>(invocation_result));
    auto destination_result = create_destination(
        invocation.descriptor.get(), source.snapshot().name, expected_uid, control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&destination_result)) {
        return with_cleanup(*error, reference);
    }
    auto destination = std::move(std::get<DestinationHandle>(destination_result));
    auto pass = run_source_pass(source.implementation_->locator, SourcePass::Second,
                                &source.implementation_->snapshot, destination.descriptor.get(),
                                control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&pass)) {
        return with_cleanup(*error, reference);
    }
    if (sync_descriptor(FileSpoolStage::BeforeFileSync, destination.descriptor.get(), hooks) != 0) {
        return with_cleanup(durability_error(DurabilityReason::SyncFailed), reference);
    }
    if (sync_descriptor(FileSpoolStage::BeforeInvocationSync, invocation.descriptor.get(), hooks) !=
        0) {
        return with_cleanup(durability_error(DurabilityReason::DirectorySyncFailed), reference);
    }
    if (const auto error =
            revalidate_publication(root, invocation_id, invocation, source.snapshot().name,
                                   destination, source.snapshot(), expected_uid, hooks)) {
        return with_cleanup(*error, reference);
    }
    if (const auto error = control_error(control)) {
        return with_cleanup(*error, reference);
    }
    return CreatedSpool{.reference = reference,
                        .local_path = root.account_state_path + "/" + reference.relative_path};
}

SpoolRootInspectionResult
inspect_spool_root(std::string account_state, uid_t expected_uid, const FileSpoolControl& control,
                   const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (const auto error = control_error(control)) {
        return *error;
    }
    auto opened = open_spool_root(std::move(account_state), expected_uid, false, control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&opened)) {
        if (error->kind != FileSpoolErrorKind::DurabilityFailure || !error->durability_reason) {
            return *error;
        }
        const bool io = *error->durability_reason == DurabilityReason::OpenFailed ||
                        *error->durability_reason == DurabilityReason::ReadFailed;
        return SpoolRootInspection{.state = io ? SpoolRootState::IoFailure : SpoolRootState::Unsafe,
                                   .reason = error->durability_reason};
    }
    const auto& root = std::get<RootHandle>(opened);
    SpoolRootInspection inspection;
    inspection.state = root.absent ? SpoolRootState::Absent : SpoolRootState::Safe;
    return inspection;
}

SpoolInventoryResult enumerate_spool(std::string account_state, uid_t expected_uid,
                                     const FileSpoolControl& control,
                                     const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    if (const auto error = control_error(control)) {
        return *error;
    }
    auto opened = open_spool_root(std::move(account_state), expected_uid, false, control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&opened)) {
        return *error;
    }
    auto root = std::move(std::get<RootHandle>(opened));
    if (root.absent) {
        SpoolInventory inventory;
        inventory.root_absent = true;
        return inventory;
    }
    return enumerate_open_root(root, expected_uid, control, hooks);
}

SpoolReconciliationResult reconcile_spool_inventory(const SpoolInventory& inventory,
                                                    std::vector<ExpectedSpoolObject> expected) {
    std::ranges::sort(expected,
                      [](const ExpectedSpoolObject& left, const ExpectedSpoolObject& right) {
                          return left.invocation_id < right.invocation_id;
                      });
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (!valid_invocation_id(expected[index].invocation_id) ||
            !valid_basename(expected[index].file_name) ||
            (index != 0 && expected[index - 1].invocation_id == expected[index].invocation_id)) {
            return durability_error(DurabilityReason::SchemaError);
        }
    }
    SpoolReconciliation result;
    result.contradiction = inventory.contradiction;
    std::map<std::string, ExpectedSpoolObject, std::less<>> expected_by_id;
    for (auto& item : expected) {
        expected_by_id.emplace(item.invocation_id, std::move(item));
    }
    for (const auto& observation : inventory.invocations) {
        const auto match = expected_by_id.find(observation.invocation_id);
        if (match == expected_by_id.end()) {
            select_contradiction(result.contradiction, observation.directory_path);
            continue;
        }
        if (observation.files.empty()) {
            result.incomplete_invocations.push_back(observation.invocation_id);
            expected_by_id.erase(match);
            continue;
        }
        const auto unequal = std::ranges::find_if(observation.files, [&](const auto& file) {
            return file.name != match->second.file_name;
        });
        if (unequal != observation.files.end()) {
            select_contradiction(result.contradiction, unequal->path);
            expected_by_id.erase(match);
            continue;
        }
        result.ready_invocations.push_back(observation.invocation_id);
        expected_by_id.erase(match);
    }
    for (auto& [invocation_id, item] : expected_by_id) {
        static_cast<void>(invocation_id);
        result.missing.push_back(std::move(item));
    }
    return result;
}

// Cleanup validates the complete one-file object before each destructive filesystem step.
// NOLINTBEGIN(readability-function-cognitive-complexity)
SpoolCleanupCallResult
cleanup_spool_file(std::string_view account_state, const SpoolRef& reference, uid_t expected_uid,
                   const FileSpoolControl& control,
                   const std::shared_ptr<const testing::FileSpoolHooks>& hooks) {
    const auto parsed = parse_spool_path(reference);
    if (!parsed || !canonical_absolute_directory(account_state)) {
        return durability_error(DurabilityReason::SchemaError);
    }
    const std::string account_state_string(account_state);
    auto root_result = open_spool_root(account_state_string, expected_uid, false, control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&root_result)) {
        return *error;
    }
    auto root = std::move(std::get<RootHandle>(root_result));
    if (root.absent) {
        return SpoolCleanupResult{.removed = false};
    }
    const auto& [invocation_id, name] = *parsed;
    const auto invocation_diagnostic = diagnostic_for(account_state, "/" + invocation_id);
    const auto file_diagnostic = diagnostic_for(account_state, "/" + invocation_id + "/" + name);
    if (!invocation_diagnostic || !file_diagnostic) {
        return durability_error(DurabilityReason::SchemaError);
    }
    const auto sync_root = [&]() -> std::optional<FileSpoolError> {
        if (sync_descriptor(FileSpoolStage::BeforeCleanupRootSync, root.root.get(), hooks) != 0) {
            return durability_error(DurabilityReason::DirectorySyncFailed);
        }
        return control_error(control);
    };
    notify(hooks, FileSpoolStage::BeforeCleanupOpen);
    if (const auto error = control_error(control)) {
        return *error;
    }
    struct stat invocation_entry {};
    if (::fstatat(root.root.get(), invocation_id.c_str(), &invocation_entry, AT_SYMLINK_NOFOLLOW) !=
        0) {
        if (errno == ENOENT) {
            if (const auto error = sync_root()) {
                return *error;
            }
            return SpoolCleanupResult{.removed = false};
        }
        return durability_error(DurabilityReason::ReadFailed);
    }
    const Descriptor invocation(::openat(root.root.get(), invocation_id.c_str(),
                                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat invocation_opened {};
    if (!invocation || ::fstat(invocation.get(), &invocation_opened) != 0 ||
        !same_directory(invocation_entry, invocation_opened) ||
        !valid_private_directory(invocation_opened, expected_uid)) {
        return contradiction_at(*invocation_diagnostic);
    }
    auto children_result = directory_names(invocation.get(), control, hooks);
    if (auto* error = std::get_if<FileSpoolError>(&children_result)) {
        return *error;
    }
    const auto& children = std::get<std::vector<std::string>>(children_result);
    if (children.size() > 1 || (children.size() == 1 && children.front() != name)) {
        FilesystemDiagnosticPath unexpected = *invocation_diagnostic;
        if (!children.empty()) {
            const auto candidate =
                diagnostic_for(account_state, "/" + invocation_id + "/" + children.front());
            if (!candidate) {
                return durability_error(DurabilityReason::SchemaError);
            }
            unexpected = *candidate;
        }
        return contradiction_at(std::move(unexpected));
    }
    Descriptor file;
    struct stat file_entry {};
    struct stat file_opened {};
    if (!children.empty()) {
        if (::fstatat(invocation.get(), name.c_str(), &file_entry, AT_SYMLINK_NOFOLLOW) != 0) {
            return errno == ENOENT ? contradiction_at(*file_diagnostic)
                                   : durability_error(DurabilityReason::ReadFailed);
        }
        file = Descriptor(::openat(invocation.get(), name.c_str(),
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (!file || ::fstat(file.get(), &file_opened) != 0 ||
            !same_inode(file_entry, file_opened) || !valid_private_file(file_entry, expected_uid) ||
            !valid_private_file(file_opened, expected_uid)) {
            return contradiction_at(*file_diagnostic);
        }
        notify(hooks, FileSpoolStage::BeforeCleanupUnlink);
        if (const auto error = control_error(control)) {
            return *error;
        }
        struct stat final_entry {};
        if (::fstatat(invocation.get(), name.c_str(), &final_entry, AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_inode(file_entry, final_entry) ||
            ::unlinkat(invocation.get(), name.c_str(), 0) != 0) {
            return contradiction_at(*file_diagnostic);
        }
    }
    notify(hooks, FileSpoolStage::BeforeCleanupInvocationRemove);
    if (children.empty()) {
        if (const auto error = control_error(control)) {
            return *error;
        }
    }
    std::optional<FileSpoolError> removal_error;
    if (::unlinkat(root.root.get(), invocation_id.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
        removal_error = errno == ENOTEMPTY ? contradiction_at(*invocation_diagnostic)
                                           : durability_error(DurabilityReason::WriteFailed);
    }
    if (const auto error = sync_root()) {
        return *error;
    }
    if (removal_error) {
        return *removal_error;
    }
    return SpoolCleanupResult{.removed = true};
}
// NOLINTEND(readability-function-cognitive-complexity)

std::optional<FilesystemDiagnosticPath>
encode_filesystem_diagnostic_path(std::string_view absolute_path_bytes) {
    if (absolute_path_bytes.size() < 2 || absolute_path_bytes.front() != '/' ||
        absolute_path_bytes.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    constexpr std::string_view hex = "0123456789abcdef";
    FilesystemDiagnosticPath result;
    result.bytes_hex.reserve(absolute_path_bytes.size() * 2);
    for (const char byte : absolute_path_bytes) {
        const auto value = static_cast<unsigned char>(byte);
        result.bytes_hex.push_back(hex.at(value >> 4U));
        result.bytes_hex.push_back(hex.at(value & 0x0fU));
    }
    return result;
}

bool valid_filesystem_diagnostic_path(const FilesystemDiagnosticPath& value) {
    if (value.bytes_hex.size() < 4 || value.bytes_hex.size() % 2 != 0 ||
        !value.bytes_hex.starts_with("2f")) {
        return false;
    }
    for (std::size_t offset = 0; offset < value.bytes_hex.size(); offset += 2) {
        const auto first = value.bytes_hex[offset];
        const auto second = value.bytes_hex[offset + 1];
        const auto lower_hex = [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        };
        if (!lower_hex(first) || !lower_hex(second) || (first == '0' && second == '0')) {
            return false;
        }
    }
    return true;
}

} // namespace tgcli::daemon
