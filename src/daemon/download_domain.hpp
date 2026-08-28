#pragma once

#include "core/td_runtime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

enum class DownloadMediaType {
    Animation,
    Audio,
    Document,
    Photo,
    Sticker,
    Video,
    VideoNote,
    VoiceNote,
};

enum class DownloadMediaError {
    AlbumUnsupported,
    PaidMediaUnsupported,
    WebPageUnsupported,
    ExpiredMedia,
    UnsupportedMedia,
    Malformed,
};

struct DownloadMediaSelection {
    DownloadMediaType media_type = DownloadMediaType::Document;
    core::TdFile file;

    bool operator==(const DownloadMediaSelection&) const = default;
};

using DownloadMediaOutcome = std::variant<DownloadMediaSelection, DownloadMediaError>;

[[nodiscard]] DownloadMediaOutcome select_download_media(const core::TdDownloadMessage& message);
[[nodiscard]] std::string_view download_media_type_name(DownloadMediaType type);
[[nodiscard]] std::string_view download_media_error_reason(DownloadMediaError error);

enum class DownloadFileEventStatus {
    Accepted,
    IgnoredWrongId,
    Malformed,
    ConflictingCompletion,
};

struct DownloadFileEvent {
    DownloadFileEventStatus status = DownloadFileEventStatus::Accepted;
    std::optional<nlohmann::json> advisory_progress;
    bool completed = false;
};

class DownloadFileTracker {
  public:
    explicit DownloadFileTracker(std::int32_t file_id) : file_id_(file_id) {}

    [[nodiscard]] DownloadFileEvent observe(const core::TdFile& file, bool response);
    [[nodiscard]] const std::optional<core::TdFile>& completed_file() const noexcept {
        return completed_;
    }

  private:
    std::int32_t file_id_ = 0;
    std::int64_t last_advisory_ = 0;
    bool total_invalidated_ = false;
    std::optional<core::TdFile> completed_;
};

[[nodiscard]] bool safe_download_leaf(std::string_view value);

} // namespace tgcli::daemon
