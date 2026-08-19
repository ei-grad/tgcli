#include "common/daemon_lock.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <stdlib.h>
#endif

namespace tgcli::daemon_lock {

namespace {

constexpr std::size_t kMaxRecordBytes = 256;
constexpr std::size_t kControlTokenBytes = 16;

int open_flags(int base) {
#ifdef O_NOFOLLOW
    return base | O_CLOEXEC | O_NOFOLLOW;
#else
    return base | O_CLOEXEC;
#endif
}

bool validate_lock_file(int fd, uid_t expected_uid, std::string& error) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        error = std::string("cannot stat daemon lock: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error = "daemon lock is not a regular file";
        return false;
    }
    if (st.st_uid != expected_uid) {
        error = "daemon lock is owned by uid " + std::to_string(st.st_uid) + ", not " +
                std::to_string(expected_uid);
        return false;
    }
    if ((st.st_mode & 07777) != 0600) {
        error = "daemon lock has unsafe permissions; expected mode 0600";
        return false;
    }
    if (st.st_nlink != 1) {
        error = "daemon lock has an unexpected hard-link count";
        return false;
    }
    return true;
}

bool process_start_token(pid_t pid, std::string& token, std::string& error) {
#if defined(__linux__)
    const std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream input(stat_path);
    std::string data;
    if (!input || !std::getline(input, data)) {
        error = "cannot read process identity for pid " + std::to_string(pid);
        return false;
    }
    const auto command_end = data.rfind(')');
    if (command_end == std::string::npos || command_end + 2 >= data.size()) {
        error = "malformed process identity for pid " + std::to_string(pid);
        return false;
    }
    std::istringstream fields(data.substr(command_end + 2));
    std::string value;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value)) {
            error = "incomplete process identity for pid " + std::to_string(pid);
            return false;
        }
        if (field == 22) {
            if (value.empty() || !std::ranges::all_of(value, [](unsigned char ch) {
                    return ch >= static_cast<unsigned char>('0') &&
                           ch <= static_cast<unsigned char>('9');
                })) {
                error = "invalid process start time for pid " + std::to_string(pid);
                return false;
            }
            token = "linux:" + value;
            return true;
        }
    }
#elif defined(__APPLE__)
    proc_bsdinfo info{};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) !=
        static_cast<int>(sizeof(info))) {
        error = "cannot read process identity for pid " + std::to_string(pid);
        return false;
    }
    token = "macos:" + std::to_string(info.pbi_start_tvsec) + ":" +
            std::to_string(info.pbi_start_tvusec);
    return true;
#else
    (void)pid;
    error = "safe daemon process identity is unsupported on this platform";
    return false;
#endif
    error = "cannot determine process identity for pid " + std::to_string(pid);
    return false;
}

bool random_control_token(std::string& token, std::string& error) {
    std::array<unsigned char, kControlTokenBytes> bytes{};
#if defined(__linux__)
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = std::string("cannot generate daemon control token: ") +
                    (count < 0 ? std::strerror(errno) : "short read");
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
#elif defined(__APPLE__)
    ::arc4random_buf(bytes.data(), bytes.size());
#else
    error = "secure daemon control tokens are unsupported on this platform";
    return false;
#endif

    constexpr std::string_view hex = "0123456789abcdef";
    token.clear();
    token.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        token.push_back(hex.at(byte >> 4));
        token.push_back(hex.at(byte & 0x0f));
    }
    return true;
}

bool parse_canonical_unsigned(std::string_view value, std::uint64_t minimum, std::uint64_t maximum,
                              std::uint64_t& parsed) {
    if (value.empty() || (value.size() > 1 && value.front() == '0') ||
        !std::ranges::all_of(value, [](unsigned char ch) {
            return ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('9');
        })) {
        return false;
    }
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
           parsed >= minimum && parsed <= maximum;
}

bool parse_process_start(std::string_view value) {
#if defined(__linux__)
    constexpr std::string_view prefix = "linux:";
    if (!value.starts_with(prefix)) {
        return false;
    }
    std::uint64_t ticks = 0;
    return parse_canonical_unsigned(value.substr(prefix.size()), 1,
                                    std::numeric_limits<std::uint64_t>::max(), ticks);
#elif defined(__APPLE__)
    constexpr std::string_view prefix = "macos:";
    if (!value.starts_with(prefix)) {
        return false;
    }
    value.remove_prefix(prefix.size());
    const auto separator = value.find(':');
    if (separator == std::string_view::npos ||
        value.find(':', separator + 1) != std::string_view::npos) {
        return false;
    }
    std::uint64_t seconds = 0;
    std::uint64_t microseconds = 0;
    return parse_canonical_unsigned(value.substr(0, separator), 1,
                                    std::numeric_limits<std::uint64_t>::max(), seconds) &&
           parse_canonical_unsigned(value.substr(separator + 1), 0, 999999, microseconds);
#else
    (void)value;
    return false;
#endif
}

bool parse_record(std::string_view record, Identity& identity, std::string& error) {
    const std::string prefix = std::string(kIdentityRecordTag) + " ";
    if (!record.starts_with(prefix)) {
        error = "daemon lock identity is malformed";
        return false;
    }
    const std::size_t pid_start = prefix.size();
    const std::size_t pid_end = record.find(' ', pid_start);
    const std::size_t process_start = pid_end == std::string_view::npos ? pid_end : pid_end + 1;
    const std::size_t process_end = record.find(' ', process_start);
    if (pid_end == std::string_view::npos || process_end == std::string_view::npos) {
        error = "daemon lock identity is malformed";
        return false;
    }
    const std::size_t token_start = process_end + 1;
    const std::size_t expected_size = token_start + kControlTokenHexLength + 1;
    const std::string_view pid_text = record.substr(pid_start, pid_end - pid_start);
    const std::string_view process_text = record.substr(process_start, process_end - process_start);
    const std::string_view control_token = record.substr(token_start, kControlTokenHexLength);
    std::uint64_t parsed_pid = 0;
    if (record.size() != expected_size || record.back() != '\n' ||
        !parse_canonical_unsigned(pid_text, 1,
                                  static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()),
                                  parsed_pid) ||
        !parse_process_start(process_text) ||
        !std::ranges::all_of(control_token, [](unsigned char ch) {
            return (ch >= static_cast<unsigned char>('0') &&
                    ch <= static_cast<unsigned char>('9')) ||
                   (ch >= static_cast<unsigned char>('a') && ch <= static_cast<unsigned char>('f'));
        })) {
        error = "daemon lock identity is malformed";
        return false;
    }
    identity = Identity{static_cast<pid_t>(parsed_pid), std::string(process_text),
                        std::string(control_token)};
    return true;
}

bool read_record(int fd, std::string& record, std::string& error) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        error = std::string("cannot stat daemon lock identity: ") + std::strerror(errno);
        return false;
    }
    if (st.st_size <= 0 || static_cast<std::size_t>(st.st_size) > kMaxRecordBytes) {
        error = "daemon lock identity has an invalid size";
        return false;
    }
    record.assign(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t offset = 0;
    while (offset < record.size()) {
        const ssize_t count =
            ::pread(fd, record.data() + offset, record.size() - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = std::string("cannot read daemon lock identity: ") +
                    (count < 0 ? std::strerror(errno) : "unexpected end of file");
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool query_kernel_owner(int fd, bool& held, pid_t& owner_pid, std::string& error) {
    struct flock query {};
    query.l_type = F_WRLCK;
    query.l_whence = SEEK_SET;
    query.l_start = 0;
    query.l_len = 0;
    if (::fcntl(fd, F_GETLK, &query) != 0) {
        error = std::string("cannot inspect daemon lock owner: ") + std::strerror(errno);
        return false;
    }
    if (query.l_type == F_UNLCK) {
        held = false;
        owner_pid = -1;
        return true;
    }
    if (query.l_type != F_WRLCK || query.l_pid < 1) {
        error = "daemon lock has an invalid kernel owner";
        return false;
    }
    held = true;
    owner_pid = query.l_pid;
    return true;
}

OwnerStatus inspect_released_record(int fd, Identity& identity, pid_t& observed_pid,
                                    std::string& error) {
    std::string initial_record;
    std::string initial_error;
    const bool initial_read = read_record(fd, initial_record, initial_error);
    Identity parsed_identity;
    std::string parse_error;
    const bool parsed = initial_read && parse_record(initial_record, parsed_identity, parse_error);

    bool final_held = false;
    pid_t final_owner = -1;
    if (!query_kernel_owner(fd, final_held, final_owner, error)) {
        return OwnerStatus::Invalid;
    }
    if (final_held) {
        observed_pid = final_owner;
        error.clear();
        return OwnerStatus::Transition;
    }

    std::string final_record;
    std::string final_error;
    const bool final_read = read_record(fd, final_record, final_error);
    if (initial_read != final_read || (initial_read && initial_record != final_record)) {
        observed_pid = -1;
        error.clear();
        return OwnerStatus::Transition;
    }
    if (!initial_read) {
        error = initial_error;
        return OwnerStatus::Invalid;
    }
    if (!parsed) {
        error = parse_error;
        return OwnerStatus::Invalid;
    }
    if (!final_read) {
        error = final_error;
        return OwnerStatus::Invalid;
    }
    identity = std::move(parsed_identity);
    observed_pid = -1;
    error.clear();
    return OwnerStatus::Released;
}

OwnerStatus query_owner(int fd, Identity& identity, pid_t& observed_pid, std::string& error) {
    bool initial_held = false;
    pid_t initial_owner = -1;
    if (!query_kernel_owner(fd, initial_held, initial_owner, error)) {
        return OwnerStatus::Invalid;
    }
    if (!initial_held) {
        return inspect_released_record(fd, identity, observed_pid, error);
    }
    observed_pid = initial_owner;

    std::optional<std::string> initial_record;
    std::string initial_record_error;
    std::string record_bytes;
    if (read_record(fd, record_bytes, initial_record_error)) {
        initial_record = std::move(record_bytes);
    }

    Identity parsed_identity;
    const Identity* parsed = nullptr;
    std::string parse_error;
    if (initial_record && parse_record(*initial_record, parsed_identity, parse_error)) {
        parsed = &parsed_identity;
    }

    std::optional<std::string> live_start;
    std::string live_start_error;
    std::string live_start_value;
    if (parsed != nullptr && parsed->pid == initial_owner &&
        process_start_token(parsed->pid, live_start_value, live_start_error)) {
        live_start = std::move(live_start_value);
    }

    bool final_held = false;
    pid_t final_owner = -1;
    if (!query_kernel_owner(fd, final_held, final_owner, error)) {
        return OwnerStatus::Invalid;
    }

    std::optional<std::string> final_record;
    std::string final_record_error;
    record_bytes.clear();
    if (read_record(fd, record_bytes, final_record_error)) {
        final_record = std::move(record_bytes);
    }

    const auto as_view =
        [](const std::optional<std::string>& value) -> std::optional<std::string_view> {
        if (!value) {
            return std::nullopt;
        }
        return *value;
    };
    std::string classification_error;
    const auto status = detail::classify_owner_observation(
        initial_owner, final_held, final_owner, as_view(initial_record), as_view(final_record),
        parsed, live_start ? std::optional<std::string_view>(*live_start) : std::nullopt,
        classification_error);
    if (status == detail::ObservationStatus::Transition) {
        if (parsed != nullptr) {
            identity = std::move(parsed_identity);
        }
        error.clear();
        return OwnerStatus::Transition;
    }
    if (status == detail::ObservationStatus::Invalid) {
        if (!initial_record) {
            error = initial_record_error;
        } else if (parsed == nullptr) {
            error = parse_error;
        } else if (!live_start && parsed->pid == initial_owner) {
            error = live_start_error;
        } else if (!final_record) {
            error = final_record_error;
        } else {
            error = classification_error;
        }
        return OwnerStatus::Invalid;
    }
    identity = std::move(parsed_identity);
    error.clear();
    return OwnerStatus::Held;
}

void observe_acquire_stage(const detail::AcquireHooks* hooks, detail::AcquireStage stage) {
    if (hooks != nullptr && hooks->observer != nullptr) {
        hooks->observer(stage, hooks->context);
    }
}

bool write_all_at_start(int fd, std::string_view data, const detail::AcquireHooks* hooks,
                        std::string& error) {
    if (::ftruncate(fd, 0) != 0) {
        error = std::string("cannot truncate daemon lock identity: ") + std::strerror(errno);
        return false;
    }
    observe_acquire_stage(hooks, detail::AcquireStage::RecordTruncated);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t count =
            ::pwrite(fd, data.data() + offset, data.size() - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = std::string("cannot write daemon lock identity: ") +
                    (count < 0 ? std::strerror(errno) : "short write");
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    observe_acquire_stage(hooks, detail::AcquireStage::RecordPublished);
    return true;
}

} // namespace

namespace detail {

ObservationStatus classify_owner_observation(pid_t initial_owner, bool final_held,
                                             pid_t final_owner,
                                             std::optional<std::string_view> initial_record,
                                             std::optional<std::string_view> final_record,
                                             const Identity* parsed_identity,
                                             std::optional<std::string_view> live_process_start,
                                             std::string& error) {
    if (!final_held || final_owner != initial_owner ||
        initial_record.has_value() != final_record.has_value() ||
        (initial_record && *initial_record != *final_record)) {
        error.clear();
        return ObservationStatus::Transition;
    }
    if (initial_owner < 1 || !initial_record || parsed_identity == nullptr || !live_process_start) {
        error = "daemon lock identity could not be validated against a stable owner";
        return ObservationStatus::Invalid;
    }
    if (parsed_identity->pid != initial_owner) {
        error = "daemon lock identity does not match its stable kernel owner";
        return ObservationStatus::Invalid;
    }
    if (parsed_identity->process_start != *live_process_start) {
        error = "daemon lock identity refers to a different stable process instance";
        return ObservationStatus::Invalid;
    }
    error.clear();
    return ObservationStatus::Stable;
}

} // namespace detail

OwnerWatch::OwnerWatch(OwnerWatch&& other) noexcept
    : fd_(other.fd_), identity_(std::move(other.identity_)), observed_pid_(other.observed_pid_) {
    other.fd_ = -1;
    other.observed_pid_ = -1;
}

OwnerWatch& OwnerWatch::operator=(OwnerWatch&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        identity_ = std::move(other.identity_);
        observed_pid_ = other.observed_pid_;
        other.fd_ = -1;
        other.observed_pid_ = -1;
    }
    return *this;
}

OwnerWatch::~OwnerWatch() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool OwnerWatch::owner_released(bool& released, std::string& error) const {
    if (observed_pid_ < 1) {
        released = true;
        return true;
    }
    bool held = false;
    pid_t owner_pid = -1;
    if (!query_kernel_owner(fd_, held, owner_pid, error)) {
        return false;
    }
    if (!held || owner_pid != observed_pid_) {
        released = true;
        return true;
    }
    if (identity_.process_start.empty()) {
        released = false;
        return true;
    }
    std::string live_start;
    if (!process_start_token(owner_pid, live_start, error)) {
        bool rechecked_held = false;
        pid_t rechecked_pid = -1;
        std::string recheck_error;
        if (query_kernel_owner(fd_, rechecked_held, rechecked_pid, recheck_error) &&
            (!rechecked_held || rechecked_pid != observed_pid_)) {
            error.clear();
            released = true;
            return true;
        }
        return false;
    }
    released = live_start != identity_.process_start;
    return true;
}

int acquire(const std::string& lock_path, Identity& identity, std::string& error,
            const detail::AcquireHooks* hooks) {
    const int fd = ::open(lock_path.c_str(), open_flags(O_RDWR | O_CREAT), 0600);
    if (fd < 0) {
        error = "cannot open " + lock_path + ": " + std::strerror(errno);
        return -1;
    }
    if (!validate_lock_file(fd, getuid(), error)) {
        ::close(fd);
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        error = "another tgcli process holds the account lock (" + lock_path + ")";
        ::close(fd);
        return -1;
    }
    observe_acquire_stage(hooks, detail::AcquireStage::BootstrapLocked);

    struct flock owner_lock {};
    owner_lock.l_type = F_WRLCK;
    owner_lock.l_whence = SEEK_SET;
    owner_lock.l_start = 0;
    owner_lock.l_len = 0;
    if (::fcntl(fd, F_SETLK, &owner_lock) != 0) {
        error = std::string("cannot establish daemon identity lock: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    observe_acquire_stage(hooks, detail::AcquireStage::OwnerLocked);

    identity.pid = getpid();
    if (!process_start_token(identity.pid, identity.process_start, error) ||
        !random_control_token(identity.control_token, error)) {
        ::close(fd);
        return -1;
    }
    const std::string record = std::string(kIdentityRecordTag) + " " +
                               std::to_string(identity.pid) + " " + identity.process_start + " " +
                               identity.control_token + "\n";
    if (!write_all_at_start(fd, record, hooks, error)) {
        ::close(fd);
        return -1;
    }
    if (::flock(fd, LOCK_UN) != 0) {
        error = std::string("cannot publish daemon identity: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

LifetimeLease::~LifetimeLease() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    if (parent_fd_ >= 0) {
        ::close(parent_fd_);
    }
}

bool LifetimeLease::validate(uid_t expected_uid, std::string& error) const {
    if (fd_ < 0 || parent_fd_ < 0 || identity_.pid != ::getpid() || path_.empty() ||
        parent_path_.empty() || basename_.empty()) {
        error = "daemon lock lifetime lease is not held";
        return false;
    }
    struct stat descriptor_metadata {};
    if (::fstat(fd_, &descriptor_metadata) != 0 || !validate_lock_file(fd_, expected_uid, error) ||
        static_cast<std::uint64_t>(descriptor_metadata.st_dev) != device_ ||
        static_cast<std::uint64_t>(descriptor_metadata.st_ino) != inode_) {
        if (error.empty()) {
            error = "daemon lock lifetime lease changed";
        }
        return false;
    }
    struct stat parent_metadata {};
    struct stat current_parent_metadata {};
    if (::fstat(parent_fd_, &parent_metadata) != 0 ||
        static_cast<std::uint64_t>(parent_metadata.st_dev) != parent_device_ ||
        static_cast<std::uint64_t>(parent_metadata.st_ino) != parent_inode_ ||
        !S_ISDIR(parent_metadata.st_mode) ||
        ::lstat(parent_path_.c_str(), &current_parent_metadata) != 0 ||
        current_parent_metadata.st_dev != parent_metadata.st_dev ||
        current_parent_metadata.st_ino != parent_metadata.st_ino ||
        !S_ISDIR(current_parent_metadata.st_mode)) {
        error = "daemon lock lifetime parent path changed";
        return false;
    }
    struct stat named_metadata {};
    if (::fstatat(parent_fd_, basename_.c_str(), &named_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        named_metadata.st_dev != descriptor_metadata.st_dev ||
        named_metadata.st_ino != descriptor_metadata.st_ino) {
        error = "daemon lock lifetime path changed";
        return false;
    }
    std::string record;
    Identity parsed;
    if (!read_record(fd_, record, error) || !parse_record(record, parsed, error) ||
        parsed != identity_) {
        if (error.empty()) {
            error = "daemon lock lifetime identity changed";
        }
        return false;
    }
    std::string process_start;
    if (!process_start_token(identity_.pid, process_start, error) ||
        process_start != identity_.process_start) {
        if (error.empty()) {
            error = "daemon lock lifetime owner changed";
        }
        return false;
    }
    error.clear();
    return true;
}

std::shared_ptr<LifetimeLease> acquire_lifetime(const std::string& lock_path, Identity& identity,
                                                std::string& error) {
    const auto separator = lock_path.rfind('/');
    std::string parent_path;
    if (separator == std::string::npos) {
        parent_path = ".";
    } else if (separator == 0) {
        parent_path = "/";
    } else {
        parent_path = lock_path.substr(0, separator);
    }
    const std::string basename =
        separator == std::string::npos ? lock_path : lock_path.substr(separator + 1);
    if (basename.empty() || basename == "." || basename == ".." ||
        basename.find('/') != std::string::npos) {
        error = "invalid daemon lock lifetime path";
        return {};
    }
    const int parent_fd = ::open(parent_path.c_str(), open_flags(O_RDONLY | O_DIRECTORY));
    struct stat parent_metadata {};
    if (parent_fd < 0 || ::fstat(parent_fd, &parent_metadata) != 0 ||
        !S_ISDIR(parent_metadata.st_mode)) {
        error = "cannot retain daemon lock parent directory";
        if (parent_fd >= 0) {
            ::close(parent_fd);
        }
        return {};
    }
    Identity acquired_identity;
    const int fd = acquire(lock_path, acquired_identity, error);
    if (fd < 0) {
        ::close(parent_fd);
        return {};
    }
    struct stat metadata {};
    struct stat named_metadata {};
    if (::fstat(fd, &metadata) != 0 ||
        ::fstatat(parent_fd, basename.c_str(), &named_metadata, AT_SYMLINK_NOFOLLOW) != 0 ||
        metadata.st_dev != named_metadata.st_dev || metadata.st_ino != named_metadata.st_ino) {
        error = "acquired daemon lock path changed";
        ::close(fd);
        ::close(parent_fd);
        return {};
    }
    identity = acquired_identity;
    error.clear();
    return std::shared_ptr<LifetimeLease>(new LifetimeLease(
        fd, parent_fd, lock_path, parent_path, basename, std::move(acquired_identity),
        static_cast<std::uint64_t>(metadata.st_dev), static_cast<std::uint64_t>(metadata.st_ino),
        static_cast<std::uint64_t>(parent_metadata.st_dev),
        static_cast<std::uint64_t>(parent_metadata.st_ino)));
}

bool parse_identity_record(std::string_view record, Identity& identity, std::string& error) {
    return parse_record(record, identity, error);
}

bool owner_pid_matches(pid_t record_pid, pid_t kernel_pid, std::string& error) {
    if (record_pid < 1 || kernel_pid < 1 || record_pid != kernel_pid) {
        error = "daemon lock identity does not match its kernel owner";
        return false;
    }
    return true;
}

std::optional<OwnerWatch> verify_owner(const std::string& lock_path, uid_t expected_uid,
                                       std::string& error) {
    std::optional<OwnerWatch> owner;
    const auto status = inspect_owner(lock_path, expected_uid, owner, error);
    if (status == OwnerStatus::Released) {
        error = "daemon lock is not held; refusing its recorded identity";
        owner.reset();
    } else if (status == OwnerStatus::Transition) {
        error = "daemon lock ownership changed during verification";
        owner.reset();
    }
    return owner;
}

OwnerStatus inspect_owner(const std::string& lock_path, uid_t expected_uid,
                          std::optional<OwnerWatch>& owner, std::string& error) {
    owner.reset();
    const int fd = ::open(lock_path.c_str(), open_flags(O_RDWR));
    if (fd < 0) {
        if (errno == ENOENT) {
            error.clear();
            return OwnerStatus::Released;
        }
        error = "cannot open " + lock_path + ": " + std::strerror(errno);
        return OwnerStatus::Invalid;
    }
    if (!validate_lock_file(fd, expected_uid, error)) {
        ::close(fd);
        return OwnerStatus::Invalid;
    }

    if (::flock(fd, LOCK_SH | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            error =
                std::string("cannot synchronize daemon lock inspection: ") + std::strerror(errno);
            ::close(fd);
            return OwnerStatus::Invalid;
        }
        bool held = false;
        pid_t owner_pid = -1;
        if (!query_kernel_owner(fd, held, owner_pid, error)) {
            ::close(fd);
            return OwnerStatus::Invalid;
        }
        owner = OwnerWatch(fd, Identity{}, held ? owner_pid : -1);
        error.clear();
        return OwnerStatus::Transition;
    }

    Identity identity;
    pid_t observed_pid = -1;
    const auto status = query_owner(fd, identity, observed_pid, error);
    const int unlock_result = ::flock(fd, LOCK_UN);
    if (unlock_result != 0 && status != OwnerStatus::Invalid) {
        error = std::string("cannot finish daemon lock inspection: ") + std::strerror(errno);
        ::close(fd);
        return OwnerStatus::Invalid;
    }
    if (status == OwnerStatus::Invalid) {
        ::close(fd);
        return OwnerStatus::Invalid;
    }
    owner = OwnerWatch(fd, std::move(identity), observed_pid);
    return status;
}

} // namespace tgcli::daemon_lock
