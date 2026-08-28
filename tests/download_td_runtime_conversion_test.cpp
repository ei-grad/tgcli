#include "core/td_runtime_test_adapter.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <td/telegram/td_api.h>

using namespace tgcli::core;
namespace td_api = td::td_api;

namespace {

td_api::object_ptr<td_api::file> native_file(std::int32_t id, std::string path, std::int64_t size,
                                             std::int64_t downloaded, bool completed) {
    auto local = td_api::make_object<td_api::localFile>();
    local->path_ = std::move(path);
    local->can_be_downloaded_ = true;
    local->is_downloading_completed_ = completed;
    local->downloaded_size_ = downloaded;
    auto file = td_api::make_object<td_api::file>();
    file->id_ = id;
    file->size_ = size;
    file->expected_size_ = size + 1;
    file->local_ = std::move(local);
    return file;
}

TdValue convert_download_message(td_api::object_ptr<td_api::MessageContent> content,
                                 std::int64_t album_id = 0) {
    auto message = td_api::make_object<td_api::message>();
    message->id_ = 91;
    message->chat_id_ = -1001;
    message->media_album_id_ = album_id;
    message->content_ = std::move(content);
    td_api::object_ptr<td_api::Object> object = std::move(message);
    return detail::convert_production_direct_response_for_test(TdFunctionKind::GetDownloadMessage,
                                                               TdValue::from(std::move(object)));
}

} // namespace

TEST_CASE("download TD factories retain the exact pinned arguments",
          "[download][core][tdlib][td-runtime-factory]") {
    auto message = detail::make_production_get_download_message_for_test(-1001, 91);
    CHECK(
        detail::production_function_matches_for_test(message, TdFunctionKind::GetDownloadMessage));
    CHECK(detail::production_get_message_matches_for_test(message, -1001, 91));

    auto download = detail::make_production_download_file_for_test(7);
    CHECK(detail::production_function_matches_for_test(download, TdFunctionKind::DownloadFile));
    CHECK(detail::production_download_file_matches_for_test(download, 7));

    auto suggested = detail::make_production_get_suggested_file_name_for_test(7, "/safe/output");
    CHECK(detail::production_function_matches_for_test(suggested,
                                                       TdFunctionKind::GetSuggestedFileName));
    CHECK(
        detail::production_get_suggested_file_name_matches_for_test(suggested, 7, "/safe/output"));
    CHECK_THROWS_AS(detail::make_production_download_file_for_test(0), std::invalid_argument);
    CHECK_THROWS_AS(detail::make_production_get_suggested_file_name_for_test(7, ""),
                    std::invalid_argument);
}

TEST_CASE("download message conversion retains primary file and album identity",
          "[download][core][tdlib][td-runtime-converter]") {
    auto animation = td_api::make_object<td_api::animation>();
    animation->animation_ = native_file(7, "/cache/a", 10, 4, false);
    auto content = td_api::make_object<td_api::messageAnimation>();
    content->animation_ = std::move(animation);

    auto converted = convert_download_message(std::move(content), 55);
    const auto* message = converted.get_if<TdDownloadMessage>();
    REQUIRE(message != nullptr);
    CHECK(message->id == 91);
    CHECK(message->chat_id == -1001);
    CHECK(message->media_album_id == 55);
    CHECK(message->media_kind == TdDownloadMediaKind::Animation);
    REQUIRE(message->primary_file);
    CHECK(message->primary_file->id == 7);
    CHECK(message->primary_file->size == 10);
    CHECK(message->primary_file->expected_size == 11);
    REQUIRE(message->primary_file->local);
    CHECK(message->primary_file->local->path == "/cache/a");
    CHECK(message->primary_file->local->downloaded_size == 4);
    CHECK_FALSE(message->primary_file->local->is_downloading_completed);
}

TEST_CASE("download photo conversion preserves TD order and ignores message video",
          "[download][core][tdlib][td-runtime-converter]") {
    auto first = td_api::make_object<td_api::photoSize>();
    first->width_ = 100;
    first->height_ = 100;
    first->photo_ = native_file(10, "/cache/first", 100, 100, true);
    auto second = td_api::make_object<td_api::photoSize>();
    second->width_ = 200;
    second->height_ = 50;
    second->photo_ = native_file(11, "/cache/second", 101, 101, true);
    auto photo = td_api::make_object<td_api::photo>();
    photo->sizes_.push_back(std::move(first));
    photo->sizes_.push_back(std::move(second));
    auto content = td_api::make_object<td_api::messagePhoto>();
    content->photo_ = std::move(photo);
    content->video_ = td_api::make_object<td_api::video>();
    content->video_->video_ = native_file(99, "/cache/video", 999, 999, true);

    auto converted = convert_download_message(std::move(content));
    const auto* message = converted.get_if<TdDownloadMessage>();
    REQUIRE(message != nullptr);
    CHECK(message->media_kind == TdDownloadMediaKind::Photo);
    REQUIRE(message->photo_sizes.size() == 2);
    REQUIRE(message->photo_sizes[0].file);
    REQUIRE(message->photo_sizes[1].file);
    CHECK(message->photo_sizes[0].file->id == 10);
    CHECK(message->photo_sizes[1].file->id == 11);
    CHECK_FALSE(message->primary_file);
}

TEST_CASE("download converter classifies paid webpage expired and unsupported contents",
          "[download][core][tdlib][td-runtime-converter]") {
    const auto kind_of = [](const TdValue& value) {
        const auto* message = value.get_if<TdDownloadMessage>();
        REQUIRE(message != nullptr);
        return message->media_kind;
    };
    CHECK(kind_of(convert_download_message(td_api::make_object<td_api::messagePaidMedia>())) ==
          TdDownloadMediaKind::PaidMedia);
    auto webpage = td_api::make_object<td_api::messageText>();
    webpage->link_preview_ = td_api::make_object<td_api::linkPreview>();
    auto webpage_value = convert_download_message(std::move(webpage));
    CHECK(kind_of(webpage_value) == TdDownloadMediaKind::WebPage);
    CHECK(kind_of(convert_download_message(td_api::make_object<td_api::messageExpiredPhoto>())) ==
          TdDownloadMediaKind::Expired);
    CHECK(kind_of(convert_download_message(td_api::make_object<td_api::messageLocation>())) ==
          TdDownloadMediaKind::Unsupported);
}

TEST_CASE("download converter rejects a message without content",
          "[download][core][tdlib][td-runtime-converter]") {
    auto converted = convert_download_message(nullptr);
    CHECK(converted.get_if<TdDownloadMessage>() == nullptr);
}

TEST_CASE("download file response and update preserve the same file snapshot",
          "[download][core][tdlib][td-runtime-converter]") {
    td_api::object_ptr<td_api::Object> response = native_file(7, "/cache/a", 10, 10, true);
    auto converted = detail::convert_production_direct_response_for_test(
        TdFunctionKind::DownloadFile, TdValue::from(std::move(response)));
    const auto* file = converted.get_if<TdFile>();
    REQUIRE(file != nullptr);
    REQUIRE(file->local);
    CHECK(file->local->is_downloading_completed);

    auto update = td_api::make_object<td_api::updateFile>(native_file(7, "/cache/a", 10, 10, true));
    td_api::object_ptr<td_api::Object> update_object = std::move(update);
    auto update_value =
        detail::convert_production_update_for_test(TdValue::from(std::move(update_object)), 3);
    const auto* file_update = update_value.get_if<TdUpdateFile>();
    REQUIRE(file_update != nullptr);
    CHECK(file_update->file == *file);
}

TEST_CASE("suggested file name conversion preserves exact TD text",
          "[download][core][tdlib][td-runtime-converter]") {
    td_api::object_ptr<td_api::Object> text = td_api::make_object<td_api::text>("report.pdf");
    auto converted = detail::convert_production_direct_response_for_test(
        TdFunctionKind::GetSuggestedFileName, TdValue::from(std::move(text)));
    const auto* suggested = converted.get_if<TdSuggestedFileName>();
    REQUIRE(suggested != nullptr);
    CHECK(suggested->value == "report.pdf");
}
