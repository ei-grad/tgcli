#pragma once

#include "core/td_runtime.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace tgcli::daemon {

enum class DownloadFilesystemReason {
    InvalidPath,
    OpenFailed,
    WriteFailed,
    SyncFailed,
    SourceChanged,
    CleanupFailed,
};

enum class DownloadFilesystemErrorKind { OutputExists, OutputUnavailable, Stopped };

struct DownloadFilesystemError {
    DownloadFilesystemErrorKind kind = DownloadFilesystemErrorKind::OutputUnavailable;
    DownloadFilesystemReason reason = DownloadFilesystemReason::InvalidPath;
    std::string final_path;

    bool operator==(const DownloadFilesystemError&) const = default;
};

struct DownloadDestination {
    std::string directory;
    std::optional<std::string> leaf;
    bool directory_mode = false;

    bool operator==(const DownloadDestination&) const = default;
};

using DownloadDestinationOutcome = std::variant<DownloadDestination, DownloadFilesystemError>;

[[nodiscard]] DownloadDestinationOutcome
prepare_download_destination(const std::optional<std::string>& output,
                             const std::optional<std::string>& media_directory,
                             std::string_view frozen_cwd);
[[nodiscard]] DownloadDestinationOutcome apply_suggested_file_name(DownloadDestination destination,
                                                                   std::string suggested_name);

struct PublishedDownload {
    std::string path;
    std::uint64_t bytes = 0;

    bool operator==(const PublishedDownload&) const = default;
};

enum class DownloadFilesystemStage {
    TempCreated,
    CopyComplete,
    SourceRevalidated,
    TempSynced,
    BeforePublish,
    Published,
    DirectorySynced,
    Cleanup,
};

namespace testing {

struct DownloadFilesystemHooks {
    std::function<std::string()> random_hex;
    std::function<void(DownloadFilesystemStage)> observe;
    std::function<bool(DownloadFilesystemStage)> fail;
};

} // namespace testing

using DownloadPublishOutcome = std::variant<PublishedDownload, DownloadFilesystemError>;
using DownloadPublishControl = std::function<bool()>;

[[nodiscard]] DownloadPublishOutcome
publish_download_file(const core::TdFile& completed_file, const DownloadDestination& destination,
                      const DownloadPublishControl& may_publish = {},
                      const std::shared_ptr<const testing::DownloadFilesystemHooks>& hooks = {});

[[nodiscard]] std::string_view download_filesystem_reason_name(DownloadFilesystemReason reason);

} // namespace tgcli::daemon
