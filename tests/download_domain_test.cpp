#include "daemon/download_domain.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::core;
using namespace tgcli::daemon;

namespace {

TdFile file(std::int32_t id, std::int64_t size = 10) {
    return {.id = id, .size = size, .expected_size = size, .local = std::nullopt};
}

TdFile observed(std::int32_t id, std::int64_t downloaded, std::int64_t size, bool completed,
                std::string path = "/cache/file") {
    return {.id = id,
            .size = size,
            .expected_size = size,
            .local = TdLocalFile{.path = std::move(path),
                                 .can_be_downloaded = !completed,
                                 .is_downloading_active = !completed,
                                 .is_downloading_completed = completed,
                                 .download_offset = 0,
                                 .downloaded_prefix_size = downloaded,
                                 .downloaded_size = downloaded}};
}

} // namespace

TEST_CASE("download media selection rejects wrappers before nested media", "[download][domain]") {
    TdDownloadMessage message{.id = 1,
                              .chat_id = -1,
                              .media_album_id = 7,
                              .media_kind = TdDownloadMediaKind::Animation,
                              .primary_file = file(1),
                              .photo_sizes = {}};
    CHECK(std::get<DownloadMediaError>(select_download_media(message)) ==
          DownloadMediaError::AlbumUnsupported);
    message.media_album_id = 0;
    for (const auto& [kind, error] :
         {std::pair{TdDownloadMediaKind::PaidMedia, DownloadMediaError::PaidMediaUnsupported},
          std::pair{TdDownloadMediaKind::WebPage, DownloadMediaError::WebPageUnsupported},
          std::pair{TdDownloadMediaKind::Expired, DownloadMediaError::ExpiredMedia},
          std::pair{TdDownloadMediaKind::Unsupported, DownloadMediaError::UnsupportedMedia}}) {
        message.media_kind = kind;
        CHECK(std::get<DownloadMediaError>(select_download_media(message)) == error);
    }
}

TEST_CASE("download photo selection validates every row and keeps the first area tie",
          "[download][domain]") {
    TdDownloadMessage message{.id = 1,
                              .chat_id = -1,
                              .media_album_id = 0,
                              .media_kind = TdDownloadMediaKind::Photo,
                              .primary_file = std::nullopt,
                              .photo_sizes = {{.width = 100, .height = 100, .file = file(1)},
                                              {.width = 200, .height = 50, .file = file(2)},
                                              {.width = 101, .height = 100, .file = file(3)}}};
    const auto selected = std::get<DownloadMediaSelection>(select_download_media(message));
    CHECK(selected.media_type == DownloadMediaType::Photo);
    CHECK(selected.file.id == 3);
    message.photo_sizes.pop_back();
    CHECK(std::get<DownloadMediaSelection>(select_download_media(message)).file.id == 1);
    message.photo_sizes[1].width = -1;
    CHECK(std::get<DownloadMediaError>(select_download_media(message)) ==
          DownloadMediaError::Malformed);
}

TEST_CASE("download primary selection rejects malformed TD file fields", "[download][domain]") {
    TdDownloadMessage message{.id = 1,
                              .chat_id = -1,
                              .media_album_id = 0,
                              .media_kind = TdDownloadMediaKind::Document,
                              .primary_file = file(1),
                              .photo_sizes = {}};
    CHECK(std::holds_alternative<DownloadMediaSelection>(select_download_media(message)));
    message.primary_file->id = 0;
    CHECK(std::get<DownloadMediaError>(select_download_media(message)) ==
          DownloadMediaError::Malformed);
    message.primary_file = file(1, kTdInt53Max + 1);
    CHECK(std::get<DownloadMediaError>(select_download_media(message)) ==
          DownloadMediaError::Malformed);
}

TEST_CASE("download accepts every curated primary media kind with exact public names",
          "[download][domain]") {
    struct Case {
        TdDownloadMediaKind input;
        DownloadMediaType output;
        std::string_view name;
    };
    constexpr std::array cases{
        Case{TdDownloadMediaKind::Animation, DownloadMediaType::Animation, "animation"},
        Case{TdDownloadMediaKind::Audio, DownloadMediaType::Audio, "audio"},
        Case{TdDownloadMediaKind::Document, DownloadMediaType::Document, "document"},
        Case{TdDownloadMediaKind::Sticker, DownloadMediaType::Sticker, "sticker"},
        Case{TdDownloadMediaKind::Video, DownloadMediaType::Video, "video"},
        Case{TdDownloadMediaKind::VideoNote, DownloadMediaType::VideoNote, "video_note"},
        Case{TdDownloadMediaKind::VoiceNote, DownloadMediaType::VoiceNote, "voice_note"},
    };
    for (const auto& value : cases) {
        const TdDownloadMessage message{.id = 1,
                                        .chat_id = -1,
                                        .media_album_id = 0,
                                        .media_kind = value.input,
                                        .primary_file = file(7),
                                        .photo_sizes = {}};
        const auto selected = std::get<DownloadMediaSelection>(select_download_media(message));
        CHECK(selected.media_type == value.output);
        CHECK(download_media_type_name(selected.media_type) == value.name);
    }
    CHECK(download_media_type_name(DownloadMediaType::Photo) == "photo");
}

TEST_CASE("download progress is strict increase and invalidates an exceeded total",
          "[download][domain]") {
    DownloadFileTracker tracker(7);
    auto first = tracker.observe(observed(7, 4, 10, false), false);
    REQUIRE(first.advisory_progress);
    CHECK((*first.advisory_progress)["downloaded_bytes"] == 4);
    CHECK((*first.advisory_progress)["total_bytes"] == 10);
    CHECK_FALSE(tracker.observe(observed(7, 4, 10, false), false).advisory_progress);
    CHECK_FALSE(tracker.observe(observed(7, 3, 10, false), false).advisory_progress);
    auto exceeded = tracker.observe(observed(7, 11, 10, false), false);
    REQUIRE(exceeded.advisory_progress);
    CHECK((*exceeded.advisory_progress)["total_bytes"].is_null());
    auto later = tracker.observe(observed(7, 12, 20, false), false);
    REQUIRE(later.advisory_progress);
    CHECK((*later.advisory_progress)["total_bytes"].is_null());
}

TEST_CASE("download zero completed state emits no advisory before authoritative final progress",
          "[download][domain]") {
    DownloadFileTracker tracker(7);
    auto zero = observed(7, 0, 0, true, "/cache/empty");
    zero.expected_size = 0;
    const auto event = tracker.observe(zero, true);
    CHECK(event.completed);
    CHECK_FALSE(event.advisory_progress);
}

TEST_CASE("download completed snapshot accepts advisory can-be-downloaded state",
          "[download][domain]") {
    DownloadFileTracker tracker(7);
    auto completed = observed(7, 10, 10, true);
    completed.local->can_be_downloaded = true;
    const auto event = tracker.observe(completed, true);
    CHECK(event.completed);
    CHECK(event.status == DownloadFileEventStatus::Accepted);
}

TEST_CASE("download tracker ignores wrong updates but rejects wrong response and conflicts",
          "[download][domain]") {
    DownloadFileTracker tracker(7);
    CHECK(tracker.observe(observed(8, 1, 10, false), false).status ==
          DownloadFileEventStatus::IgnoredWrongId);
    CHECK(tracker.observe(observed(8, 1, 10, false), true).status ==
          DownloadFileEventStatus::Malformed);
    const auto completed = observed(7, 10, 10, true);
    CHECK(tracker.observe(completed, false).completed);
    CHECK(tracker.observe(completed, true).completed);
    CHECK(tracker.observe(observed(7, 9, 10, false), true).completed);
    auto advisory_variant = completed;
    advisory_variant.local->can_be_downloaded = true;
    advisory_variant.local->downloaded_prefix_size = 8;
    CHECK(tracker.observe(advisory_variant, true).completed);
    CHECK(tracker.observe(observed(7, 10, 10, true, "/cache/other"), false).status ==
          DownloadFileEventStatus::ConflictingCompletion);
}

TEST_CASE("download safe leaf is strict UTF-8 without path or control syntax",
          "[download][domain]") {
    CHECK(safe_download_leaf("photo 1.jpg"));
    CHECK(safe_download_leaf("δ.png"));
    const std::array<std::string, 6> invalid{"",    ".",          "..",
                                             "a/b", "line\nfeed", std::string("bad\xFF", 4)};
    for (const auto& value : invalid) {
        CHECK_FALSE(safe_download_leaf(value));
    }
}
