#include "daemon/download_filesystem.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::core;
using namespace tgcli::daemon;

namespace {

class TempDirectory {
  public:
    TempDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "tgcli-download-fs-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

  private:
    std::filesystem::path root_;
};

void write_file(const std::filesystem::path& file, std::string_view bytes) {
    const int descriptor = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw std::runtime_error("open test file failed");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            ::close(descriptor);
            throw std::runtime_error("write test file failed");
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) {
        throw std::runtime_error("close test file failed");
    }
}

void append_file(const std::filesystem::path& file, std::string_view bytes) {
    const int descriptor = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (descriptor < 0 ||
        ::write(descriptor, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()) ||
        ::close(descriptor) != 0) {
        throw std::runtime_error("append test file failed");
    }
}

std::string read_file(const std::filesystem::path& file) {
    const int descriptor = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        throw std::runtime_error("open result failed");
    }
    std::string result;
    std::array<char, 32> bytes{};
    while (true) {
        const auto count = ::read(descriptor, bytes.data(), bytes.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            ::close(descriptor);
            throw std::runtime_error("read result failed");
        }
        result.append(bytes.data(), static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    return result;
}

TdFile completed_file(const std::filesystem::path& source, std::int64_t size) {
    return {.id = 7,
            .size = size,
            .expected_size = size,
            .local = TdLocalFile{.path = source.string(),
                                 .can_be_downloaded = false,
                                 .is_downloading_active = false,
                                 .is_downloading_completed = true,
                                 .download_offset = 0,
                                 .downloaded_prefix_size = size,
                                 .downloaded_size = size}};
}

std::shared_ptr<testing::DownloadFilesystemHooks> hooks_failing(DownloadFilesystemStage stage) {
    auto hooks = std::make_shared<testing::DownloadFilesystemHooks>();
    hooks->random_hex = [] { return std::string(32, 'a'); };
    hooks->fail = [stage](DownloadFilesystemStage candidate) { return candidate == stage; };
    return hooks;
}

} // namespace

TEST_CASE("download destination freezes relative precedence and distinguishes directory mode",
          "[download][filesystem]") {
    const TempDirectory temporary;
    const auto output_directory = temporary.root() / "out";
    const auto media_directory = temporary.root() / "media";
    std::filesystem::create_directory(output_directory);
    std::filesystem::create_directory(media_directory);

    auto media = prepare_download_destination(std::nullopt, "media", temporary.root().string());
    const auto* media_plan = std::get_if<DownloadDestination>(&media);
    REQUIRE(media_plan != nullptr);
    CHECK(media_plan->directory == media_directory.string());
    CHECK(media_plan->directory_mode);

    auto directory = prepare_download_destination("out", "media", temporary.root().string());
    const auto* directory_plan = std::get_if<DownloadDestination>(&directory);
    REQUIRE(directory_plan != nullptr);
    CHECK(directory_plan->directory == output_directory.string());
    CHECK(directory_plan->directory_mode);

    auto exact = prepare_download_destination("result.bin", "media", temporary.root().string());
    const auto* exact_plan = std::get_if<DownloadDestination>(&exact);
    REQUIRE(exact_plan != nullptr);
    CHECK(exact_plan->directory == temporary.root().string());
    CHECK(exact_plan->leaf == "result.bin");
    CHECK_FALSE(exact_plan->directory_mode);

    auto missing_directory =
        prepare_download_destination("missing/", "media", temporary.root().string());
    CHECK(std::get<DownloadFilesystemError>(missing_directory).reason ==
          DownloadFilesystemReason::InvalidPath);

    const auto dot = prepare_download_destination(std::nullopt, ".", temporary.root().string());
    CHECK(std::get<DownloadDestination>(dot).directory == temporary.root().string());
    const auto trailing = prepare_download_destination(std::nullopt, media_directory.string() + "/",
                                                       temporary.root().string());
    CHECK(std::get<DownloadDestination>(trailing).directory == media_directory.string());
    const auto root = prepare_download_destination(std::nullopt, std::nullopt, "/");
    const auto* root_plan = std::get_if<DownloadDestination>(&root);
    REQUIRE(root_plan != nullptr);
    CHECK(root_plan->directory == "/");
    CHECK(root_plan->directory_mode);
}

TEST_CASE("download suggested name is a strict leaf and exact mode never needs one",
          "[download][filesystem]") {
    const TempDirectory temporary;
    auto prepared =
        prepare_download_destination(std::nullopt, std::nullopt, temporary.root().string());
    auto plan = std::get<DownloadDestination>(std::move(prepared));
    auto applied = apply_suggested_file_name(plan, "report.pdf");
    CHECK(std::get<DownloadDestination>(applied).leaf == "report.pdf");
    for (const auto* const value : {"", ".", "..", "nested/file", "line\nfeed"}) {
        CHECK(std::holds_alternative<DownloadFilesystemError>(
            apply_suggested_file_name(plan, value)));
    }

    auto exact = std::get<DownloadDestination>(
        prepare_download_destination("exact.bin", std::nullopt, temporary.root().string()));
    const auto exact_name = apply_suggested_file_name(std::move(exact), "ignored");
    CHECK(std::holds_alternative<DownloadFilesystemError>(exact_name));
}

TEST_CASE("download publication copies stable source and publishes exclusively",
          "[download][filesystem]") {
    const TempDirectory temporary;
    const auto source = temporary.root() / "source.bin";
    write_file(source, "payload");
    auto destination = std::get<DownloadDestination>(
        prepare_download_destination("result.bin", std::nullopt, temporary.root().string()));
    auto hooks = std::make_shared<testing::DownloadFilesystemHooks>();
    hooks->random_hex = [] { return std::string(32, 'a'); };

    auto outcome = publish_download_file(completed_file(source, 7), destination, {}, hooks);
    const auto* published = std::get_if<PublishedDownload>(&outcome);
    REQUIRE(published != nullptr);
    CHECK(published->path == (temporary.root() / "result.bin").string());
    CHECK(published->bytes == 7);
    CHECK(read_file(published->path) == "payload");
    struct stat metadata {};
    REQUIRE(::stat(published->path.c_str(), &metadata) == 0);
    CHECK((metadata.st_mode & 0777) == 0600);

    auto collision = publish_download_file(completed_file(source, 7), destination, {}, hooks);
    CHECK(std::get<DownloadFilesystemError>(collision).kind ==
          DownloadFilesystemErrorKind::OutputExists);
}

TEST_CASE("download publication rejects source and destination symlink traversal",
          "[download][filesystem]") {
    const TempDirectory temporary;
    const auto source = temporary.root() / "source.bin";
    const auto source_link = temporary.root() / "source-link";
    const auto directory_link = temporary.root() / "directory-link";
    write_file(source, "payload");
    REQUIRE(::symlink(source.c_str(), source_link.c_str()) == 0);
    REQUIRE(::symlink(temporary.root().c_str(), directory_link.c_str()) == 0);
    auto direct = std::get<DownloadDestination>(
        prepare_download_destination("result.bin", std::nullopt, temporary.root().string()));
    auto source_error = publish_download_file(completed_file(source_link, 7), direct);
    CHECK(std::get<DownloadFilesystemError>(source_error).reason ==
          DownloadFilesystemReason::OpenFailed);

    auto unsafe = prepare_download_destination((directory_link / "result.bin").string(),
                                               std::nullopt, temporary.root().string());
    const auto plan = std::get<DownloadDestination>(std::move(unsafe));
    auto destination_error = publish_download_file(completed_file(source, 7), plan);
    CHECK(std::get<DownloadFilesystemError>(destination_error).reason ==
          DownloadFilesystemReason::InvalidPath);
}

TEST_CASE("download publication enforces size and pre-publish stop with cleanup",
          "[download][filesystem]") {
    const TempDirectory temporary;
    const auto source = temporary.root() / "source.bin";
    write_file(source, "payload");
    auto destination = std::get<DownloadDestination>(
        prepare_download_destination("result.bin", std::nullopt, temporary.root().string()));
    auto hooks = std::make_shared<testing::DownloadFilesystemHooks>();
    hooks->random_hex = [] { return std::string(32, 'a'); };
    auto mismatch = publish_download_file(completed_file(source, 8), destination, {}, hooks);
    CHECK(std::get<DownloadFilesystemError>(mismatch).reason ==
          DownloadFilesystemReason::SourceChanged);
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));
    auto short_mismatch = publish_download_file(completed_file(source, 6), destination, {}, hooks);
    CHECK(std::get<DownloadFilesystemError>(short_mismatch).reason ==
          DownloadFilesystemReason::SourceChanged);
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));

    auto stopped_outcome =
        publish_download_file(completed_file(source, 7), destination, [] { return false; }, hooks);
    CHECK(std::get<DownloadFilesystemError>(stopped_outcome).kind ==
          DownloadFilesystemErrorKind::Stopped);
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));
    CHECK(std::distance(std::filesystem::directory_iterator(temporary.root()),
                        std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("download copy never writes beyond the captured source size",
          "[download][filesystem][race]") {
    const TempDirectory temporary;
    const auto source = temporary.root() / "source.bin";
    write_file(source, "payload");
    auto destination = std::get<DownloadDestination>(
        prepare_download_destination("result.bin", std::nullopt, temporary.root().string()));
    auto hooks = std::make_shared<testing::DownloadFilesystemHooks>();
    hooks->random_hex = [] { return std::string(32, 'a'); };
    hooks->observe = [&](DownloadFilesystemStage stage) {
        if (stage == DownloadFilesystemStage::TempCreated) {
            append_file(source, "growth");
        }
    };
    hooks->fail = [](DownloadFilesystemStage stage) {
        return stage == DownloadFilesystemStage::Cleanup;
    };

    const auto outcome = publish_download_file(completed_file(source, 7), destination, {}, hooks);
    CHECK(std::get<DownloadFilesystemError>(outcome).reason ==
          DownloadFilesystemReason::CleanupFailed);
    const auto temporary_file =
        temporary.root() / ".result.bin.tgcli-download.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.tmp";
    REQUIRE(std::filesystem::exists(temporary_file));
    CHECK(std::filesystem::file_size(temporary_file) == 7);
}

TEST_CASE("download filesystem cutpoints preserve cleanup and crash contracts",
          "[download][filesystem]") {
    const TempDirectory temporary;
    const auto source = temporary.root() / "source.bin";
    write_file(source, "payload");
    auto destination = std::get<DownloadDestination>(
        prepare_download_destination("result.bin", std::nullopt, temporary.root().string()));

    auto before_publish = publish_download_file(completed_file(source, 7), destination, {},
                                                hooks_failing(DownloadFilesystemStage::TempSynced));
    CHECK(std::get<DownloadFilesystemError>(before_publish).reason ==
          DownloadFilesystemReason::SyncFailed);
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));

    auto after_publish = publish_download_file(completed_file(source, 7), destination, {},
                                               hooks_failing(DownloadFilesystemStage::Published));
    CHECK(std::get<DownloadFilesystemError>(after_publish).reason ==
          DownloadFilesystemReason::SyncFailed);
    CHECK(std::filesystem::exists(temporary.root() / "result.bin"));
}

TEST_CASE("download cleanup failure replaces the triggering error and leaves only private temp",
          "[download][filesystem]") {
    const TempDirectory temporary;
    const auto source = temporary.root() / "source.bin";
    write_file(source, "payload");
    auto destination = std::get<DownloadDestination>(
        prepare_download_destination("result.bin", std::nullopt, temporary.root().string()));
    auto hooks = std::make_shared<testing::DownloadFilesystemHooks>();
    hooks->random_hex = [] { return std::string(32, 'a'); };
    hooks->fail = [](DownloadFilesystemStage stage) {
        return stage == DownloadFilesystemStage::TempCreated ||
               stage == DownloadFilesystemStage::Cleanup;
    };
    const auto outcome = publish_download_file(completed_file(source, 7), destination, {}, hooks);
    CHECK(std::get<DownloadFilesystemError>(outcome).reason ==
          DownloadFilesystemReason::CleanupFailed);
    CHECK_FALSE(std::filesystem::exists(temporary.root() / "result.bin"));
    std::vector<std::filesystem::directory_entry> entries(
        std::filesystem::directory_iterator(temporary.root()),
        std::filesystem::directory_iterator{});
    REQUIRE(entries.size() == 2);
    const auto temporary_entry = std::ranges::find_if(
        entries, [](const auto& entry) { return entry.path().filename() != "source.bin"; });
    REQUIRE(temporary_entry != entries.end());
    struct stat metadata {};
    REQUIRE(::lstat(temporary_entry->path().c_str(), &metadata) == 0);
    CHECK(S_ISREG(metadata.st_mode));
    CHECK((metadata.st_mode & 0777) == 0600);
}
