#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace tgcli::core {

inline constexpr std::uint64_t kTdLogMaxFileSize = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kTdLogRotatedFileCount = 4;
inline constexpr int kTdLogVerbosity = 1;
inline constexpr int kTdLogInfoVerbosity = 3;

struct TdLogConfiguration {
    std::string file_path;
    std::uint64_t max_file_size = kTdLogMaxFileSize;
    std::size_t rotated_file_count = kTdLogRotatedFileCount;
    bool json_diagnostics = false;

    bool operator==(const TdLogConfiguration&) const = default;
};

namespace detail {

struct TdLogTestHooks {
    std::function<void()> after_rotation_validation;
    std::function<ssize_t(int, const void*, std::size_t, off_t)> write_at;
};

} // namespace detail

// Owns the active and generation descriptors used by TDLib's process-global
// log callback. Descriptors, rather than paths reopened during rotation, are
// authoritative after construction; rotation refuses replaced entries.
class TdLogSink {
  public:
    static std::unique_ptr<TdLogSink> create(const TdLogConfiguration& configuration, uid_t uid,
                                             std::string& error);
    static std::unique_ptr<TdLogSink> create_for_test(const TdLogConfiguration& configuration,
                                                      uid_t uid, detail::TdLogTestHooks hooks,
                                                      std::string& error);

    ~TdLogSink();
    TdLogSink(const TdLogSink&) = delete;
    TdLogSink& operator=(const TdLogSink&) = delete;
    TdLogSink(TdLogSink&&) = delete;
    TdLogSink& operator=(TdLogSink&&) = delete;

    // TDLib gives its callback one complete record at a time. Records above
    // ERROR are discarded even if a future caller raises TDLib's global level.
    bool append(int verbosity, std::string_view record, std::string& error);

    [[nodiscard]] std::vector<std::string> log_paths() const;

  private:
    TdLogSink(int directory_fd, int file_fd, std::vector<int> rotated_file_fds,
              std::string directory_path, std::string file_name, std::uint64_t max_file_size,
              uid_t uid, dev_t directory_device, ino_t directory_inode, std::uint64_t current_size,
              std::unique_ptr<detail::TdLogTestHooks> test_hooks);

    bool rotate(std::string& error);
    bool replace_contents(int target_fd, std::string_view contents, std::string& error);
    bool snapshot_rotation(std::vector<std::string>& snapshots, std::string& error) const;
    bool restore_rotation(const std::vector<std::string>& snapshots, std::string& error) const;
    bool write_record(std::string_view record, std::string& error);
    bool poison(std::string& error);
    bool directory_path_matches(std::string& error) const;
    bool current_entry_matches(std::string& error) const;
    bool validate_rotated_entries(std::string& error) const;

    int directory_fd_ = -1;
    int file_fd_ = -1;
    std::string directory_path_;
    std::string file_name_;
    std::uint64_t max_file_size_ = 0;
    std::vector<int> rotated_file_fds_;
    uid_t uid_ = 0;
    dev_t directory_device_ = 0;
    ino_t directory_inode_ = 0;
    std::uint64_t current_size_ = 0;
    std::unique_ptr<detail::TdLogTestHooks> test_hooks_;
    bool poisoned_ = false;
    mutable std::mutex mutex_;
};

} // namespace tgcli::core
