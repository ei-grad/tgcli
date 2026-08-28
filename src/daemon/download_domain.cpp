#include "daemon/download_domain.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace tgcli::daemon {

namespace {

bool valid_file(const core::TdFile& file) {
    if (file.id <= 0 || file.size < 0 || file.size > core::kTdInt53Max || file.expected_size < 0 ||
        file.expected_size > core::kTdInt53Max) {
        return false;
    }
    if (!file.local) {
        return true;
    }
    const auto& local = *file.local;
    return common::valid_utf8(local.path) && local.download_offset >= 0 &&
           local.download_offset <= core::kTdInt53Max && local.downloaded_prefix_size >= 0 &&
           local.downloaded_prefix_size <= core::kTdInt53Max && local.downloaded_size >= 0 &&
           local.downloaded_size <= core::kTdInt53Max;
}

DownloadMediaOutcome primary(DownloadMediaType type, const std::optional<core::TdFile>& file) {
    if (!file || !valid_file(*file)) {
        return DownloadMediaError::Malformed;
    }
    return DownloadMediaSelection{.media_type = type, .file = *file};
}

std::optional<std::int64_t> checked_area(std::int32_t width, std::int32_t height) {
    if (width < 0 || height < 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
}

std::optional<std::int64_t> displayed_total(const core::TdFile& file) {
    if (file.size > 0) {
        return file.size;
    }
    if (file.expected_size > 0) {
        return file.expected_size;
    }
    return std::nullopt;
}

bool strict_absolute_source(std::string_view value) {
    if (value.size() < 2 || value.front() != '/') {
        return false;
    }
    std::size_t start = 1;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto component =
            value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

bool same_completed_source(const core::TdFile& left, const core::TdFile& right) {
    return left.id == right.id && left.size == right.size &&
           left.expected_size == right.expected_size && left.local && right.local &&
           left.local->path == right.local->path &&
           left.local->is_downloading_completed == right.local->is_downloading_completed &&
           left.local->is_downloading_active == right.local->is_downloading_active;
}

} // namespace

DownloadMediaOutcome select_download_media(const core::TdDownloadMessage& message) {
    if (message.media_album_id != 0) {
        return DownloadMediaError::AlbumUnsupported;
    }
    switch (message.media_kind) {
    case core::TdDownloadMediaKind::Animation:
        return primary(DownloadMediaType::Animation, message.primary_file);
    case core::TdDownloadMediaKind::Audio:
        return primary(DownloadMediaType::Audio, message.primary_file);
    case core::TdDownloadMediaKind::Document:
        return primary(DownloadMediaType::Document, message.primary_file);
    case core::TdDownloadMediaKind::Photo: {
        const core::TdFile* selected = nullptr;
        std::int64_t selected_area = -1;
        for (const auto& size : message.photo_sizes) {
            const auto area = checked_area(size.width, size.height);
            if (!area || !size.file || !valid_file(*size.file)) {
                return DownloadMediaError::Malformed;
            }
            if (*area > selected_area) {
                selected_area = *area;
                selected = &*size.file;
            }
        }
        if (selected == nullptr) {
            return DownloadMediaError::Malformed;
        }
        return DownloadMediaSelection{.media_type = DownloadMediaType::Photo, .file = *selected};
    }
    case core::TdDownloadMediaKind::Sticker:
        return primary(DownloadMediaType::Sticker, message.primary_file);
    case core::TdDownloadMediaKind::Video:
        return primary(DownloadMediaType::Video, message.primary_file);
    case core::TdDownloadMediaKind::VideoNote:
        return primary(DownloadMediaType::VideoNote, message.primary_file);
    case core::TdDownloadMediaKind::VoiceNote:
        return primary(DownloadMediaType::VoiceNote, message.primary_file);
    case core::TdDownloadMediaKind::PaidMedia:
        return DownloadMediaError::PaidMediaUnsupported;
    case core::TdDownloadMediaKind::WebPage:
        return DownloadMediaError::WebPageUnsupported;
    case core::TdDownloadMediaKind::Expired:
        return DownloadMediaError::ExpiredMedia;
    case core::TdDownloadMediaKind::Unsupported:
        return DownloadMediaError::UnsupportedMedia;
    }
    return DownloadMediaError::Malformed;
}

std::string_view download_media_type_name(DownloadMediaType type) {
    switch (type) {
    case DownloadMediaType::Animation:
        return "animation";
    case DownloadMediaType::Audio:
        return "audio";
    case DownloadMediaType::Document:
        return "document";
    case DownloadMediaType::Photo:
        return "photo";
    case DownloadMediaType::Sticker:
        return "sticker";
    case DownloadMediaType::Video:
        return "video";
    case DownloadMediaType::VideoNote:
        return "video_note";
    case DownloadMediaType::VoiceNote:
        return "voice_note";
    }
    return "document";
}

std::string_view download_media_error_reason(DownloadMediaError error) {
    switch (error) {
    case DownloadMediaError::AlbumUnsupported:
        return "album_unsupported";
    case DownloadMediaError::PaidMediaUnsupported:
        return "paid_media_unsupported";
    case DownloadMediaError::WebPageUnsupported:
        return "web_page_unsupported";
    case DownloadMediaError::ExpiredMedia:
        return "expired_media";
    case DownloadMediaError::UnsupportedMedia:
        return "unsupported_media";
    case DownloadMediaError::Malformed:
        return "malformed_tdlib_response";
    }
    return "malformed_tdlib_response";
}

DownloadFileEvent DownloadFileTracker::observe(const core::TdFile& file, bool response) {
    if (file.id != file_id_) {
        return {.status = response ? DownloadFileEventStatus::Malformed
                                   : DownloadFileEventStatus::IgnoredWrongId,
                .advisory_progress = std::nullopt,
                .completed = false};
    }
    if (!valid_file(file) || !file.local) {
        return {.status = DownloadFileEventStatus::Malformed,
                .advisory_progress = std::nullopt,
                .completed = false};
    }
    const auto& local = *file.local;
    DownloadFileEvent event;
    if (completed_) {
        event.completed = true;
        if (!local.is_downloading_completed) {
            return event;
        }
        if (!strict_absolute_source(local.path) || local.is_downloading_active) {
            event.status = DownloadFileEventStatus::Malformed;
            return event;
        }
        if (!same_completed_source(*completed_, file)) {
            event.status = DownloadFileEventStatus::ConflictingCompletion;
        }
        return event;
    }
    if (local.downloaded_size > last_advisory_) {
        auto total = total_invalidated_ ? std::optional<std::int64_t>{} : displayed_total(file);
        if (total && local.downloaded_size > *total) {
            total_invalidated_ = true;
            total.reset();
        }
        event.advisory_progress = {
            {"operation", "download"},
            {"file_id", file_id_},
            {"downloaded_bytes", local.downloaded_size},
            {"total_bytes", total ? nlohmann::json(*total) : nlohmann::json(nullptr)}};
        last_advisory_ = local.downloaded_size;
    }
    if (!local.is_downloading_completed) {
        return event;
    }
    if (!strict_absolute_source(local.path) || local.is_downloading_active) {
        event.status = DownloadFileEventStatus::Malformed;
        event.advisory_progress.reset();
        return event;
    }
    completed_ = file;
    event.completed = true;
    return event;
}

bool safe_download_leaf(std::string_view value) {
    if (value.empty() || value == "." || value == ".." || !common::valid_utf8(value)) {
        return false;
    }
    return std::ranges::none_of(value, [](unsigned char byte) {
        return byte == '/' || byte == '\0' || byte < 0x20U || byte == 0x7FU;
    });
}

} // namespace tgcli::daemon
