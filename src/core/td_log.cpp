#include "core/td_log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tgcli::core {

namespace {

bool private_directory(const struct stat& status, uid_t uid) {
    return S_ISDIR(status.st_mode) && status.st_uid == uid && (status.st_mode & 07777) == 0700;
}

bool private_regular_file(const struct stat& status, uid_t uid) {
    return S_ISREG(status.st_mode) && status.st_uid == uid && (status.st_mode & 07777) == 0600 &&
           status.st_nlink == 1;
}

std::string system_error(std::string_view operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

bool same_file(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool write_all_at(int fd, std::string_view bytes, off_t offset,
                  const detail::TdLogTestHooks* test_hooks, std::string& error) {
    while (!bytes.empty()) {
        const auto written = test_hooks != nullptr && test_hooks->write_at
                                 ? test_hooks->write_at(fd, bytes.data(), bytes.size(), offset)
                                 : ::pwrite(fd, bytes.data(), bytes.size(), offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written == 0) {
                errno = EIO;
            }
            error = system_error("cannot write TDLib log record");
            return false;
        }
        if (static_cast<std::size_t>(written) > bytes.size()) {
            errno = EIO;
            error = system_error("cannot write TDLib log record");
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
        offset += written;
    }
    return true;
}

bool write_all_at_direct(int fd, std::string_view bytes, off_t offset, std::string& error) {
    return write_all_at(fd, bytes, offset, nullptr, error);
}

bool read_contents(int fd, uid_t uid, std::uint64_t max_size, std::string& contents,
                   std::string& error) {
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        error = system_error("cannot inspect TDLib log contents");
        return false;
    }
    if (!private_regular_file(status, uid) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > max_size) {
        error = "TDLib log contents exceed the configured safe file limit";
        return false;
    }
    contents.assign(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t consumed = 0;
    while (consumed < contents.size()) {
        const auto count = ::pread(fd, contents.data() + consumed, contents.size() - consumed,
                                   static_cast<off_t>(consumed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EIO;
            }
            error = system_error("cannot read TDLib log generation");
            return false;
        }
        consumed += static_cast<std::size_t>(count);
    }
    return true;
}

bool restore_contents(int fd, std::string_view contents, std::string& error) {
    if (::ftruncate(fd, 0) != 0 || !write_all_at_direct(fd, contents, 0, error) ||
        ::ftruncate(fd, static_cast<off_t>(contents.size())) != 0) {
        if (error.empty()) {
            error = system_error("cannot restore TDLib log generation");
        }
        return false;
    }
    return true;
}

std::string rotated_name(std::string_view active, std::size_t generation) {
    return std::string(active) + "." + std::to_string(generation);
}

int open_private_entry(int directory_fd, const std::string& name, uid_t uid, std::string& error) {
    int fd = ::openat(directory_fd, name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    bool created = false;
    if (fd < 0 && errno == ENOENT) {
        fd = ::openat(directory_fd, name.c_str(),
                      O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        created = fd >= 0;
    }
    if (fd < 0) {
        error = system_error("cannot open TDLib log entry");
        return -1;
    }
    if (created && ::fchmod(fd, 0600) != 0) {
        error = system_error("cannot set TDLib log permissions");
        ::close(fd);
        return -1;
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0 || !private_regular_file(status, uid)) {
        error = "TDLib log entry is not a current-uid 0600 single-link regular file";
        ::close(fd);
        return -1;
    }
    return fd;
}

void close_all(std::vector<int>& fds) {
    for (const int fd : fds) {
        ::close(fd);
    }
    fds.clear();
}

} // namespace

std::unique_ptr<TdLogSink> TdLogSink::create(const TdLogConfiguration& configuration, uid_t uid,
                                             std::string& error) {
    return create_for_test(configuration, uid, {}, error);
}

std::unique_ptr<TdLogSink> TdLogSink::create_for_test(const TdLogConfiguration& configuration,
                                                      uid_t uid, detail::TdLogTestHooks hooks,
                                                      std::string& error) {
    if (configuration.file_path.empty() || configuration.max_file_size == 0 ||
        configuration.rotated_file_count != kTdLogRotatedFileCount) {
        error = "invalid TDLib log configuration";
        return nullptr;
    }
    const auto separator = configuration.file_path.rfind('/');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == configuration.file_path.size()) {
        error = "TDLib log path must contain a directory and file name";
        return nullptr;
    }
    std::string directory_path = configuration.file_path.substr(0, separator);
    std::string file_name = configuration.file_path.substr(separator + 1);
    if (file_name.find('/') != std::string::npos) {
        error = "invalid TDLib log file name";
        return nullptr;
    }

    const int directory_fd =
        ::open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        error = system_error("cannot open TDLib log directory");
        return nullptr;
    }
    struct stat directory_status {};
    if (::fstat(directory_fd, &directory_status) != 0 ||
        !private_directory(directory_status, uid)) {
        error = "TDLib log directory is not a current-uid private directory";
        ::close(directory_fd);
        return nullptr;
    }
    const int file_fd = open_private_entry(directory_fd, file_name, uid, error);
    if (file_fd < 0) {
        ::close(directory_fd);
        return nullptr;
    }

    struct stat file_status {};
    if (::fstat(file_fd, &file_status) != 0 || !private_regular_file(file_status, uid)) {
        error = "TDLib log file is not a current-uid 0600 single-link regular file";
        ::close(file_fd);
        ::close(directory_fd);
        return nullptr;
    }
    if (file_status.st_size < 0 || static_cast<std::uintmax_t>(file_status.st_size) >
                                       std::numeric_limits<std::uint64_t>::max()) {
        error = "TDLib log file size is invalid";
        ::close(file_fd);
        ::close(directory_fd);
        return nullptr;
    }
    if (static_cast<std::uintmax_t>(file_status.st_size) > configuration.max_file_size) {
        error = "TDLib log file exceeds the configured safe file limit";
        ::close(file_fd);
        ::close(directory_fd);
        return nullptr;
    }

    std::vector<int> rotated_file_fds;
    rotated_file_fds.reserve(configuration.rotated_file_count);
    for (std::size_t generation = 1; generation <= configuration.rotated_file_count; ++generation) {
        const int rotated_fd =
            open_private_entry(directory_fd, rotated_name(file_name, generation), uid, error);
        if (rotated_fd < 0) {
            close_all(rotated_file_fds);
            ::close(file_fd);
            ::close(directory_fd);
            return nullptr;
        }
        struct stat rotated_status {};
        if (::fstat(rotated_fd, &rotated_status) != 0 || rotated_status.st_size < 0 ||
            static_cast<std::uintmax_t>(rotated_status.st_size) > configuration.max_file_size) {
            error = "TDLib log generation exceeds the configured safe file limit";
            ::close(rotated_fd);
            close_all(rotated_file_fds);
            ::close(file_fd);
            ::close(directory_fd);
            return nullptr;
        }
        rotated_file_fds.push_back(rotated_fd);
    }

    auto result = std::unique_ptr<TdLogSink>(new TdLogSink(
        directory_fd, file_fd, std::move(rotated_file_fds), std::move(directory_path),
        std::move(file_name), configuration.max_file_size, uid, directory_status.st_dev,
        directory_status.st_ino, static_cast<std::uint64_t>(file_status.st_size),
        std::make_unique<detail::TdLogTestHooks>(std::move(hooks))));
    if (!result->current_entry_matches(error) || !result->validate_rotated_entries(error)) {
        return nullptr;
    }
    error.clear();
    return result;
}

TdLogSink::TdLogSink(int directory_fd, int file_fd, std::vector<int> rotated_file_fds,
                     std::string directory_path, std::string file_name, std::uint64_t max_file_size,
                     uid_t uid, dev_t directory_device, ino_t directory_inode,
                     std::uint64_t current_size, std::unique_ptr<detail::TdLogTestHooks> test_hooks)
    : directory_fd_(directory_fd), file_fd_(file_fd), directory_path_(std::move(directory_path)),
      file_name_(std::move(file_name)), max_file_size_(max_file_size),
      rotated_file_fds_(std::move(rotated_file_fds)), uid_(uid),
      directory_device_(directory_device), directory_inode_(directory_inode),
      current_size_(current_size), test_hooks_(std::move(test_hooks)) {}

TdLogSink::~TdLogSink() {
    close_all(rotated_file_fds_);
    if (file_fd_ >= 0) {
        ::close(file_fd_);
    }
    if (directory_fd_ >= 0) {
        ::close(directory_fd_);
    }
}

bool TdLogSink::append(int verbosity, std::string_view record, std::string& error) {
    if (verbosity > kTdLogVerbosity || record.empty()) {
        error.clear();
        return true;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (poisoned_) {
        error = "TDLib logging is disabled after a previous sink failure";
        return false;
    }
    if (record.size() > max_file_size_) {
        error = "TDLib log record exceeds the rotation limit";
        return poison(error);
    }
    if (current_size_ != 0 &&
        (current_size_ > max_file_size_ || record.size() > max_file_size_ - current_size_) &&
        !rotate(error)) {
        return poison(error);
    }
    if (!current_entry_matches(error) || !validate_rotated_entries(error) ||
        !write_record(record, error)) {
        return poison(error);
    }
    error.clear();
    return true;
}

std::vector<std::string> TdLogSink::log_paths() const {
    std::vector<std::string> result;
    result.reserve(rotated_file_fds_.size() + 1);
    result.push_back(directory_path_ + "/" + file_name_);
    for (std::size_t generation = 1; generation <= rotated_file_fds_.size(); ++generation) {
        result.push_back(directory_path_ + "/" + rotated_name(file_name_, generation));
    }
    return result;
}

bool TdLogSink::current_entry_matches(std::string& error) const {
    if (!directory_path_matches(error)) {
        return false;
    }
    struct stat descriptor_status {};
    struct stat entry_status {};
    if (::fstat(file_fd_, &descriptor_status) != 0 ||
        ::fstatat(directory_fd_, file_name_.c_str(), &entry_status, AT_SYMLINK_NOFOLLOW) != 0) {
        error = system_error("cannot verify TDLib log identity");
        return false;
    }
    if (!private_regular_file(descriptor_status, uid_) ||
        !private_regular_file(entry_status, uid_) || !same_file(descriptor_status, entry_status)) {
        error = "TDLib log path was replaced or became unsafe";
        return false;
    }
    return true;
}

bool TdLogSink::directory_path_matches(std::string& error) const {
    struct stat descriptor_status {};
    struct stat path_status {};
    if (::fstat(directory_fd_, &descriptor_status) != 0 ||
        ::lstat(directory_path_.c_str(), &path_status) != 0) {
        error = system_error("cannot verify TDLib log directory identity");
        return false;
    }
    if (!private_directory(descriptor_status, uid_) || !private_directory(path_status, uid_) ||
        descriptor_status.st_dev != directory_device_ ||
        descriptor_status.st_ino != directory_inode_ ||
        !same_file(descriptor_status, path_status)) {
        error = "TDLib log directory path was replaced or became unsafe";
        return false;
    }
    return true;
}

bool TdLogSink::validate_rotated_entries(std::string& error) const {
    for (std::size_t index = 0; index < rotated_file_fds_.size(); ++index) {
        struct stat descriptor_status {};
        struct stat entry_status {};
        const auto name = rotated_name(file_name_, index + 1);
        if (::fstat(rotated_file_fds_[index], &descriptor_status) != 0 ||
            ::fstatat(directory_fd_, name.c_str(), &entry_status, AT_SYMLINK_NOFOLLOW) != 0) {
            error = system_error("cannot verify TDLib log generation identity");
            return false;
        }
        if (!private_regular_file(descriptor_status, uid_) ||
            !private_regular_file(entry_status, uid_) ||
            !same_file(descriptor_status, entry_status)) {
            error = "TDLib log generation path was replaced or became unsafe";
            return false;
        }
    }
    return true;
}

bool TdLogSink::rotate(std::string& error) {
    if (!current_entry_matches(error) || !validate_rotated_entries(error)) {
        return false;
    }
    if (test_hooks_ != nullptr && test_hooks_->after_rotation_validation) {
        test_hooks_->after_rotation_validation();
    }
    if (!current_entry_matches(error) || !validate_rotated_entries(error)) {
        return false;
    }
    std::vector<std::string> snapshots;
    if (!snapshot_rotation(snapshots, error)) {
        return false;
    }
    const auto rollback = [&](std::string operation_error) {
        std::string rollback_error;
        if (!restore_rotation(snapshots, rollback_error)) {
            error = std::move(operation_error) + "; rotation rollback failed: " + rollback_error;
        } else {
            error = std::move(operation_error);
        }
        return false;
    };
    for (std::size_t generation = rotated_file_fds_.size(); generation > 1; --generation) {
        if (!replace_contents(rotated_file_fds_[generation - 1], snapshots[generation - 1],
                              error)) {
            return rollback(error);
        }
    }
    if (!replace_contents(rotated_file_fds_.front(), snapshots.front(), error)) {
        return rollback(error);
    }
    if (::ftruncate(file_fd_, 0) != 0) {
        error = system_error("cannot reset active TDLib log after rotation");
        return rollback(error);
    }
    if (!current_entry_matches(error) || !validate_rotated_entries(error)) {
        return rollback(error);
    }
    current_size_ = 0;
    return true;
}

bool TdLogSink::replace_contents(int target_fd, std::string_view contents, std::string& error) {
    if (::ftruncate(target_fd, 0) != 0) {
        error = system_error("cannot reset TDLib log generation");
        return false;
    }
    if (!write_all_at(target_fd, contents, 0, test_hooks_.get(), error)) {
        return false;
    }
    if (::ftruncate(target_fd, static_cast<off_t>(contents.size())) != 0) {
        error = system_error("cannot finalize TDLib log generation");
        return false;
    }
    return true;
}

bool TdLogSink::snapshot_rotation(std::vector<std::string>& snapshots, std::string& error) const {
    snapshots.resize(rotated_file_fds_.size() + 1);
    if (!read_contents(file_fd_, uid_, max_file_size_, snapshots.front(), error)) {
        return false;
    }
    for (std::size_t index = 0; index < rotated_file_fds_.size(); ++index) {
        if (!read_contents(rotated_file_fds_[index], uid_, max_file_size_, snapshots[index + 1],
                           error)) {
            return false;
        }
    }
    return true;
}

bool TdLogSink::restore_rotation(const std::vector<std::string>& snapshots,
                                 std::string& error) const {
    if (snapshots.size() != rotated_file_fds_.size() + 1) {
        error = "invalid TDLib log rotation snapshot";
        return false;
    }
    bool restored = true;
    std::string first_error;
    const auto restore_one = [&](int fd, std::string_view contents) {
        std::string restore_error;
        if (!restore_contents(fd, contents, restore_error)) {
            restored = false;
            if (first_error.empty()) {
                first_error = std::move(restore_error);
            }
        }
    };
    restore_one(file_fd_, snapshots.front());
    for (std::size_t index = 0; index < rotated_file_fds_.size(); ++index) {
        restore_one(rotated_file_fds_[index], snapshots[index + 1]);
    }
    if (!restored) {
        error = std::move(first_error);
    }
    return restored;
}

bool TdLogSink::write_record(std::string_view record, std::string& error) {
    struct stat before {};
    if (::fstat(file_fd_, &before) != 0 || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) != current_size_) {
        error = "TDLib log size changed outside the sink";
        return false;
    }
    if (!write_all_at(file_fd_, record, static_cast<off_t>(current_size_), test_hooks_.get(),
                      error)) {
        const std::string write_error = error;
        if (::ftruncate(file_fd_, static_cast<off_t>(current_size_)) != 0) {
            error = write_error + "; partial-record rollback failed: " +
                    system_error("cannot restore TDLib log length");
        }
        return false;
    }
    struct stat after {};
    const auto expected_size = current_size_ + record.size();
    if (::fstat(file_fd_, &after) != 0 || after.st_size < 0 ||
        static_cast<std::uint64_t>(after.st_size) != expected_size) {
        error = "TDLib log record length was not committed atomically";
        if (::ftruncate(file_fd_, static_cast<off_t>(current_size_)) != 0) {
            error += "; partial-record rollback failed";
        }
        return false;
    }
    current_size_ = expected_size;
    return true;
}

bool TdLogSink::poison(std::string& error) {
    poisoned_ = true;
    if (error.empty()) {
        error = "TDLib log sink entered a terminal failure state";
    }
    return false;
}

} // namespace tgcli::core
