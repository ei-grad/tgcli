#include "daemon/file_spool.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace tgcli::daemon;

namespace {

constexpr std::string_view kInvocation = "00112233445566778899aabbccddeeff";
constexpr std::string_view kOtherInvocation = "ffeeddccbbaa99887766554433221100";

class TempTree final {
  public:
    TempTree() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-file-spool-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        state_ = root_ / "state";
        std::filesystem::create_directory(state_);
        REQUIRE(::chmod(state_.c_str(), 0700) == 0);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const {
        return root_;
    }

    [[nodiscard]] const std::filesystem::path& state() const {
        return state_;
    }

    [[nodiscard]] std::filesystem::path source(std::string_view name = "source.bin") const {
        return root_ / std::string(name);
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void write(const std::filesystem::path& filename, std::string_view bytes,
               mode_t mode = 0600) const {
        const int descriptor =
            ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        REQUIRE(descriptor >= 0);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            REQUIRE(count > 0);
            offset += static_cast<std::size_t>(count);
        }
        REQUIRE(::fchmod(descriptor, mode) == 0);
        REQUIRE(::close(descriptor) == 0);
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path state_;
};

class ScopedUmask final {
  public:
    explicit ScopedUmask(mode_t value) : previous_(::umask(value)) {}
    ~ScopedUmask() {
        ::umask(previous_);
    }
    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;
    ScopedUmask(ScopedUmask&&) = delete;
    ScopedUmask& operator=(ScopedUmask&&) = delete;

  private:
    mode_t previous_;
};

FileSpoolError require_error(const auto& result) {
    REQUIRE(std::holds_alternative<FileSpoolError>(result));
    return std::get<FileSpoolError>(result);
}

PreparedSource require_source(PrepareSpoolSourceResult result) {
    if (const auto* error = std::get_if<FileSpoolError>(&result)) {
        INFO(static_cast<int>(error->kind));
        if (error->source_reason) {
            INFO(static_cast<int>(*error->source_reason));
        }
        if (error->durability_reason) {
            INFO(static_cast<int>(*error->durability_reason));
        }
    }
    REQUIRE(std::holds_alternative<PreparedSource>(result));
    return std::move(std::get<PreparedSource>(result));
}

CreatedSpool require_created(CreateSpoolFileResult result) {
    if (const auto* error = std::get_if<FileSpoolError>(&result)) {
        INFO(static_cast<int>(error->kind));
        if (error->durability_reason) {
            INFO(static_cast<int>(*error->durability_reason));
        }
    }
    REQUIRE(std::holds_alternative<CreatedSpool>(result));
    return std::get<CreatedSpool>(std::move(result));
}

std::string read_all(const std::filesystem::path& filename) {
    std::ifstream stream(filename, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

mode_t mode_of(const std::filesystem::path& filename) {
    struct stat status {};
    REQUIRE(::lstat(filename.c_str(), &status) == 0);
    return status.st_mode & 07777;
}

std::shared_ptr<testing::FileSpoolHooks> mutation_hook(FileSpoolStage selected,
                                                       std::function<void()> mutation) {
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->at_stage = [selected, mutation = std::move(mutation),
                       fired = false](FileSpoolStage stage) mutable {
        if (!fired && stage == selected) {
            fired = true;
            mutation();
        }
    };
    return hooks;
}

} // namespace

TEST_CASE("file spool pass one freezes lexical source identity", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "abc", 0644);

    auto absolute = require_source(prepare_spool_source(temp.source().string(), "invalid cwd"));
    CHECK(absolute.snapshot().path == temp.source().string());
    CHECK(absolute.snapshot().name == "source.bin");
    CHECK(absolute.snapshot().size == 3);
    CHECK(absolute.snapshot().sha256 ==
          "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    std::filesystem::create_directory(temp.root() / "nested");
    temp.write(temp.root() / "nested" / ".env", "payload");
    auto relative =
        require_source(prepare_spool_source("nested//./../nested/.env", temp.root().string()));
    CHECK(relative.snapshot().path == (temp.root() / "nested" / ".env").string());
    CHECK(relative.snapshot().name == ".env");

    REQUIRE(::link(temp.source().c_str(), (temp.root() / "hardlink").c_str()) == 0);
    auto hardlink = require_source(prepare_spool_source("hardlink", temp.root().string()));
    CHECK(hardlink.snapshot().inode == absolute.snapshot().inode);
}

TEST_CASE("file spool retains one cwd descriptor across both passes", "[file-spool]") {
    const TempTree temp;
    const auto cwd = temp.root() / "cwd";
    const auto moved = temp.root() / "moved";
    std::filesystem::create_directory(cwd);
    temp.write(cwd / "value", "original");
    auto source = require_source(prepare_spool_source("value", cwd.string()));

    REQUIRE(::rename(cwd.c_str(), moved.c_str()) == 0);
    std::filesystem::create_directory(cwd);
    temp.write(cwd / "value", "replacement");
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    CHECK(read_all(created.local_path) == "original");
    CHECK(created.reference.file.path == (cwd / "value").string());
}

TEST_CASE("file spool replays lexically cancelled components without following symlinks",
          "[file-spool]") {
    const TempTree temp;
    std::filesystem::create_directory(temp.root() / "real");
    temp.write(temp.source(), "payload");
    REQUIRE(::symlink((temp.root() / "real").c_str(), (temp.root() / "link").c_str()) == 0);
    const auto result =
        prepare_spool_source((temp.root() / "link" / ".." / "source.bin").string(), "/");
    const auto error = require_error(result);
    CHECK(error.kind == FileSpoolErrorKind::SourceUnavailable);
    CHECK(error.source_reason == SourceFileReason::Symlink);
}

TEST_CASE("file spool rejects invalid source grammar and safe basenames", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");

    for (const auto& value :
         std::vector<std::string>{"", "source.bin/", ".", "..", std::string("bad\0name", 8),
                                  std::string("bad\x01name", 8), std::string("bad\xc2\x80name", 9),
                                  std::string(256, 'a'), std::string("\xc0\x80", 2)}) {
        const auto error = require_error(prepare_spool_source(value, temp.root().string()));
        CHECK(error.kind == FileSpoolErrorKind::InvalidInput);
    }
    CHECK(require_error(prepare_spool_source("source.bin", "relative")).kind ==
          FileSpoolErrorKind::InvalidInput);
    CHECK(require_error(prepare_spool_source("source.bin", temp.root().string() + "/.")).kind ==
          FileSpoolErrorKind::InvalidInput);
    CHECK(
        require_error(prepare_spool_source("source.bin", temp.root().string() + "/missing")).kind ==
        FileSpoolErrorKind::InvalidInput);

    const std::string too_long(4097, 'a');
    CHECK(require_error(prepare_spool_source(too_long, temp.root().string())).kind ==
          FileSpoolErrorKind::InvalidInput);
}

TEST_CASE("file spool classifies initial source failures exactly", "[file-spool]") {
    const TempTree temp;
    SECTION("missing") {
        const auto error = require_error(prepare_spool_source("missing", temp.root().string()));
        CHECK(error.source_reason == SourceFileReason::Missing);
    }
    SECTION("target symlink") {
        REQUIRE(::symlink("missing", temp.source().c_str()) == 0);
        const auto error =
            require_error(prepare_spool_source(temp.source().string(), temp.root().string()));
        CHECK(error.source_reason == SourceFileReason::Symlink);
    }
    SECTION("parent symlink") {
        std::filesystem::create_directory(temp.root() / "outside");
        temp.write(temp.root() / "outside" / "value", "payload");
        REQUIRE(::symlink((temp.root() / "outside").c_str(), (temp.root() / "alias").c_str()) == 0);
        const auto error =
            require_error(prepare_spool_source((temp.root() / "alias" / "value").string(), "/"));
        CHECK(error.source_reason == SourceFileReason::Symlink);
    }
    SECTION("directory") {
        std::filesystem::create_directory(temp.source());
        const auto error = require_error(prepare_spool_source(temp.source().string(), "/"));
        CHECK(error.source_reason == SourceFileReason::WrongType);
    }
    SECTION("fifo") {
        REQUIRE(::mkfifo(temp.source().c_str(), 0600) == 0);
        const auto error = require_error(prepare_spool_source(temp.source().string(), "/"));
        CHECK(error.source_reason == SourceFileReason::WrongType);
    }
    SECTION("empty") {
        temp.write(temp.source(), "");
        const auto error = require_error(prepare_spool_source(temp.source().string(), "/"));
        CHECK(error.source_reason == SourceFileReason::Empty);
    }
    SECTION("unreadable") {
        temp.write(temp.source(), "payload", 0000);
        const auto result = prepare_spool_source(temp.source().string(), "/");
        if (::geteuid() == 0) {
            CHECK(std::holds_alternative<PreparedSource>(result));
        } else {
            CHECK(require_error(result).source_reason == SourceFileReason::Unreadable);
        }
    }
}

TEST_CASE("file spool detects pass-one entry and content races", "[file-spool]") {
    const TempTree temp;
    SECTION("replace before open") {
        temp.write(temp.source(), "original");
        const auto moved = temp.root() / "moved";
        auto hooks = mutation_hook(FileSpoolStage::AfterSourceEntryStat, [&] {
            REQUIRE(::rename(temp.source().c_str(), moved.c_str()) == 0);
            temp.write(temp.source(), "replacement");
        });
        CHECK(require_error(prepare_spool_source("source.bin", temp.root().string(), {}, hooks))
                  .kind == FileSpoolErrorKind::InputChanged);
    }
    SECTION("truncate during read") {
        temp.write(temp.source(), std::string(static_cast<std::size_t>(128) * 1024U, 'a'));
        auto hooks = mutation_hook(FileSpoolStage::DuringPass1Read,
                                   [&] { REQUIRE(::truncate(temp.source().c_str(), 1) == 0); });
        CHECK(require_error(prepare_spool_source("source.bin", temp.root().string(), {}, hooks))
                  .kind == FileSpoolErrorKind::InputChanged);
    }
    SECTION("grow during read") {
        temp.write(temp.source(), "a");
        auto hooks = mutation_hook(FileSpoolStage::DuringPass1Read, [&] {
            std::ofstream stream(temp.source(), std::ios::binary | std::ios::app);
            stream << "b";
        });
        CHECK(require_error(prepare_spool_source("source.bin", temp.root().string(), {}, hooks))
                  .kind == FileSpoolErrorKind::InputChanged);
    }
    SECTION("same-size rewrite during read") {
        temp.write(temp.source(), "original");
        auto hooks = mutation_hook(FileSpoolStage::DuringPass1Read,
                                   [&] { temp.write(temp.source(), "modified"); });
        CHECK(require_error(prepare_spool_source("source.bin", temp.root().string(), {}, hooks))
                  .kind == FileSpoolErrorKind::InputChanged);
    }
    SECTION("replace after read") {
        temp.write(temp.source(), "original");
        const auto moved = temp.root() / "moved";
        auto hooks = mutation_hook(FileSpoolStage::BeforeSourceRevalidate, [&] {
            REQUIRE(::rename(temp.source().c_str(), moved.c_str()) == 0);
            temp.write(temp.source(), "replacement");
        });
        CHECK(require_error(prepare_spool_source("source.bin", temp.root().string(), {}, hooks))
                  .kind == FileSpoolErrorKind::InputChanged);
    }
}

TEST_CASE("file spool detects retained parent edge replacement", "[file-spool]") {
    const TempTree temp;
    const auto parent = temp.root() / "parent";
    const auto moved = temp.root() / "moved";
    std::filesystem::create_directory(parent);
    temp.write(parent / "source.bin", "payload");
    auto hooks = mutation_hook(FileSpoolStage::BeforeSourceRevalidate, [&] {
        REQUIRE(::rename(parent.c_str(), moved.c_str()) == 0);
        REQUIRE(::symlink(moved.c_str(), parent.c_str()) == 0);
    });
    const auto error =
        require_error(prepare_spool_source((parent / "source.bin").string(), "/", {}, hooks));
    CHECK(error.kind == FileSpoolErrorKind::InputChanged);
}

TEST_CASE("file spool maps every pass-two source discrepancy to input changed", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "original");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));

    SECTION("missing") {
        REQUIRE(::unlink(temp.source().c_str()) == 0);
    }
    SECTION("symlink") {
        REQUIRE(::unlink(temp.source().c_str()) == 0);
        REQUIRE(::symlink("other", temp.source().c_str()) == 0);
    }
    SECTION("wrong type") {
        REQUIRE(::unlink(temp.source().c_str()) == 0);
        std::filesystem::create_directory(temp.source());
    }
    SECTION("replacement") {
        REQUIRE(::unlink(temp.source().c_str()) == 0);
        temp.write(temp.source(), "original");
    }
    SECTION("same inode rewrite") {
        temp.write(temp.source(), "modified");
    }

    const auto error = require_error(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    CHECK(error.kind == FileSpoolErrorKind::InputChanged);
    REQUIRE(error.cleanup_reference);
    CHECK(error.cleanup_reference->relative_path ==
          "spool/00112233445566778899aabbccddeeff/source.bin");
}

TEST_CASE("file spool verifies pass-two digest and post-copy parent edges", "[file-spool]") {
    const TempTree temp;
    SECTION("digest mismatch with unchanged metadata") {
        temp.write(temp.source(), "original");
        auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->read = [changed = false](FileSpoolIo operation, int descriptor, void* data,
                                        std::size_t size) mutable -> ssize_t {
            const auto count = ::read(descriptor, data, size);
            if (!changed && operation == FileSpoolIo::Pass2Read && count > 0) {
                changed = true;
                static_cast<unsigned char*>(data)[0] ^= 0xffU;
            }
            return count;
        };
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
        CHECK(error.kind == FileSpoolErrorKind::InputChanged);
    }
    SECTION("parent replacement after copy") {
        const auto parent = temp.root() / "parent";
        const auto moved = temp.root() / "moved";
        std::filesystem::create_directory(parent);
        temp.write(parent / "source.bin", "payload");
        auto source = require_source(prepare_spool_source((parent / "source.bin").string(), "/"));
        auto hooks = mutation_hook(FileSpoolStage::BeforeSourceRevalidate, [&] {
            REQUIRE(::rename(parent.c_str(), moved.c_str()) == 0);
            REQUIRE(::symlink(moved.c_str(), parent.c_str()) == 0);
        });
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
        CHECK(error.kind == FileSpoolErrorKind::InputChanged);
    }
}

TEST_CASE("file spool creates durable private object with exact identity", "[file-spool]") {
    const TempTree temp;
    const std::string bytes = std::string(1024 * 1024 + 17, '\0') + "tail";
    temp.write(temp.source(), bytes, 0666);
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    std::vector<FileSpoolStage> syncs;
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->sync = [&](FileSpoolStage stage, int descriptor) {
        syncs.push_back(stage);
        return ::fsync(descriptor);
    };
    const ScopedUmask umask(0000);
    const auto created = require_created(create_spool_file(
        source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));

    CHECK(created.reference.file == source.snapshot());
    CHECK(created.reference.relative_path == "spool/00112233445566778899aabbccddeeff/source.bin");
    CHECK(created.local_path ==
          (temp.state() / "spool" / std::string(kInvocation) / "source.bin").string());
    CHECK(read_all(created.local_path) == bytes);
    CHECK(mode_of(temp.state() / "spool") == 0700);
    CHECK(mode_of(temp.state() / "spool" / std::string(kInvocation)) == 0700);
    CHECK(mode_of(created.local_path) == 0600);
    CHECK(syncs == std::vector{FileSpoolStage::BeforeAccountStateSync,
                               FileSpoolStage::BeforeRootSync, FileSpoolStage::BeforeFileSync,
                               FileSpoolStage::BeforeInvocationSync});
}

TEST_CASE("file spool exact modes survive restrictive umask", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const ScopedUmask umask(0777);
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    CHECK(mode_of(temp.state() / "spool") == 0700);
    CHECK(mode_of(temp.state() / "spool" / std::string(kInvocation)) == 0700);
    CHECK(mode_of(created.local_path) == 0600);
}

TEST_CASE("file spool root inspection is total and non-mutating", "[file-spool]") {
    const TempTree temp;
    auto absent = inspect_spool_root(temp.state().string(), ::getuid());
    REQUIRE(std::holds_alternative<SpoolRootInspection>(absent));
    CHECK(std::get<SpoolRootInspection>(absent).state == SpoolRootState::Absent);
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool"));

    SECTION("safe") {
        std::filesystem::create_directory(temp.state() / "spool");
        REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
        const auto result = inspect_spool_root(temp.state().string(), ::getuid());
        CHECK(std::get<SpoolRootInspection>(result).state == SpoolRootState::Safe);
    }
    SECTION("symlink") {
        REQUIRE(::symlink(temp.root().c_str(), (temp.state() / "spool").c_str()) == 0);
        const auto inspection =
            std::get<SpoolRootInspection>(inspect_spool_root(temp.state().string(), ::getuid()));
        CHECK(inspection.state == SpoolRootState::Unsafe);
        CHECK(inspection.reason == DurabilityReason::PathInvalid);
    }
    SECTION("file") {
        temp.write(temp.state() / "spool", "unsafe");
        const auto inspection =
            std::get<SpoolRootInspection>(inspect_spool_root(temp.state().string(), ::getuid()));
        CHECK(inspection.reason == DurabilityReason::WrongType);
    }
    SECTION("mode") {
        std::filesystem::create_directory(temp.state() / "spool");
        REQUIRE(::chmod((temp.state() / "spool").c_str(), 0755) == 0);
        const auto inspection =
            std::get<SpoolRootInspection>(inspect_spool_root(temp.state().string(), ::getuid()));
        CHECK(inspection.reason == DurabilityReason::WrongMode);
    }
    SECTION("owner") {
        std::filesystem::create_directory(temp.state() / "spool");
        REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->mutate_metadata = [](FileSpoolMetadata point, struct stat& status) {
            if (point == FileSpoolMetadata::RootEntry ||
                point == FileSpoolMetadata::RootDescriptor) {
                ++status.st_uid;
            }
        };
        const auto inspection = std::get<SpoolRootInspection>(
            inspect_spool_root(temp.state().string(), ::getuid(), {}, hooks));
        CHECK(inspection.reason == DurabilityReason::WrongOwner);
    }
    SECTION("open io") {
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->should_fail = [](FileSpoolStage stage) {
            return stage == FileSpoolStage::BeforeRootInspect;
        };
        const auto inspection = std::get<SpoolRootInspection>(
            inspect_spool_root(temp.state().string(), ::getuid(), {}, hooks));
        CHECK(inspection.state == SpoolRootState::IoFailure);
        CHECK(inspection.reason == DurabilityReason::OpenFailed);
    }
}

TEST_CASE("file spool detects root entry to descriptor replacement", "[file-spool]") {
    const TempTree temp;
    std::filesystem::create_directory(temp.state() / "spool");
    REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
    const auto moved = temp.state() / "moved";
    auto hooks = mutation_hook(FileSpoolStage::AfterRootEntryStat, [&] {
        REQUIRE(::rename((temp.state() / "spool").c_str(), moved.c_str()) == 0);
        std::filesystem::create_directory(temp.state() / "spool");
        REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
    });
    const auto inspection = std::get<SpoolRootInspection>(
        inspect_spool_root(temp.state().string(), ::getuid(), {}, hooks));
    CHECK(inspection.state == SpoolRootState::Unsafe);
    CHECK(inspection.reason == DurabilityReason::PathInvalid);
}

TEST_CASE("file spool preserves typed destination failures and cleanup capability",
          "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    SECTION("runtime name max") {
        auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->name_max = [](int) { return 3; };
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
        CHECK(error.durability_reason == DurabilityReason::PathInvalid);
        REQUIRE(error.cleanup_reference);
        const auto cleanup =
            cleanup_spool_file(temp.state().string(), *error.cleanup_reference, ::getuid());
        REQUIRE(std::holds_alternative<SpoolCleanupResult>(cleanup));
        CHECK(std::get<SpoolCleanupResult>(cleanup).removed);
    }
    SECTION("capacity") {
        auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->write = [](int, const void*, std::size_t) -> ssize_t {
            errno = ENOSPC;
            return -1;
        };
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
        CHECK(error.durability_reason == DurabilityReason::CapacityExhausted);
        REQUIRE(error.cleanup_reference);
    }
    SECTION("short writes and EINTR are retried") {
        auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        std::size_t calls = 0;
        hooks->write = [&](int descriptor, const void* bytes, std::size_t size) -> ssize_t {
            ++calls;
            if (calls == 1) {
                errno = EINTR;
                return -1;
            }
            return ::write(descriptor, bytes, std::min<std::size_t>(size, 2));
        };
        const auto created = require_created(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
        CHECK(calls > 2);
        CHECK(read_all(created.local_path) == "payload");
    }
    SECTION("invocation collision is retained contradiction") {
        std::filesystem::create_directory(temp.state() / "spool");
        REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
        std::filesystem::create_directory(temp.state() / "spool" / std::string(kInvocation));
        REQUIRE(::chmod((temp.state() / "spool" / std::string(kInvocation)).c_str(), 0700) == 0);
        auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
        const auto error = require_error(
            create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
        CHECK(error.kind == FileSpoolErrorKind::Contradiction);
        CHECK_FALSE(error.cleanup_reference);
        CHECK(std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation)));
    }
}

TEST_CASE("file spool exposes every durability sync cutpoint", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    const auto failed_stage =
        GENERATE(FileSpoolStage::BeforeAccountStateSync, FileSpoolStage::BeforeRootSync,
                 FileSpoolStage::BeforeFileSync, FileSpoolStage::BeforeInvocationSync);
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->should_fail = [failed_stage](FileSpoolStage stage) { return stage == failed_stage; };
    const auto error = require_error(create_spool_file(
        source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
    CHECK(error.durability_reason == (failed_stage == FileSpoolStage::BeforeFileSync
                                          ? DurabilityReason::SyncFailed
                                          : DurabilityReason::DirectorySyncFailed));
}

TEST_CASE("file spool cleanup is exact, durable and idempotent", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    std::vector<FileSpoolStage> syncs;
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->sync = [&](FileSpoolStage stage, int descriptor) {
        syncs.push_back(stage);
        return ::fsync(descriptor);
    };
    auto cleanup =
        cleanup_spool_file(temp.state().string(), created.reference, ::getuid(), {}, hooks);
    REQUIRE(std::holds_alternative<SpoolCleanupResult>(cleanup));
    CHECK(std::get<SpoolCleanupResult>(cleanup).removed);
    CHECK(std::get<SpoolCleanupResult>(cleanup).root_synced);
    CHECK(syncs == std::vector{FileSpoolStage::BeforeCleanupRootSync});
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation)));
    CHECK(std::filesystem::exists(temp.state() / "spool"));

    cleanup = cleanup_spool_file(temp.state().string(), created.reference, ::getuid());
    REQUIRE(std::holds_alternative<SpoolCleanupResult>(cleanup));
    CHECK_FALSE(std::get<SpoolCleanupResult>(cleanup).removed);
    CHECK(std::get<SpoolCleanupResult>(cleanup).root_synced);
}

TEST_CASE("file spool cleanup retains unexpected objects", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source("a"), "payload");
    auto source = require_source(prepare_spool_source(temp.source("a").string(), "/"));
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    const auto unexpected = temp.state() / "spool" / std::string(kInvocation) / "b";
    temp.write(unexpected, "keep");
    const auto error =
        require_error(cleanup_spool_file(temp.state().string(), created.reference, ::getuid()));
    CHECK(error.kind == FileSpoolErrorKind::Contradiction);
    REQUIRE(error.diagnostic_path);
    const auto expected_diagnostic = encode_filesystem_diagnostic_path(unexpected.string());
    REQUIRE(expected_diagnostic);
    CHECK(error.diagnostic_path->bytes_hex == expected_diagnostic->bytes_hex);
    CHECK(std::filesystem::exists(created.local_path));
    CHECK(std::filesystem::exists(unexpected));
}

TEST_CASE("file spool cleanup returns original failure when root sync fails", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->should_fail = [](FileSpoolStage stage) {
        return stage == FileSpoolStage::BeforeCleanupRootSync;
    };
    const auto error = require_error(
        cleanup_spool_file(temp.state().string(), created.reference, ::getuid(), {}, hooks));
    CHECK(error.durability_reason == DurabilityReason::DirectorySyncFailed);
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation)));
}

TEST_CASE("filesystem diagnostic paths are reversible canonical byte hex", "[file-spool]") {
    std::string bytes = "/state/spool/";
    bytes.push_back(static_cast<char>(0xff));
    const auto encoded = encode_filesystem_diagnostic_path(bytes);
    REQUIRE(encoded);
    CHECK(encoded->bytes_hex == "2f73746174652f73706f6f6c2fff");
    CHECK(valid_filesystem_diagnostic_path(*encoded));

    for (const auto& invalid : std::vector<FilesystemDiagnosticPath>{
             {"2Fff"}, {"2ff"}, {"6162"}, {"2f00ff"}, {"2fzz"}, {"2f"}}) {
        CHECK_FALSE(valid_filesystem_diagnostic_path(invalid));
    }
    CHECK_FALSE(encode_filesystem_diagnostic_path("relative"));
    CHECK_FALSE(encode_filesystem_diagnostic_path(std::string("/a\0b", 4)));
}

TEST_CASE("file spool inventory sorts raw bytes and never requires UTF-8", "[file-spool]") {
    const TempTree temp;
    std::filesystem::create_directory(temp.state() / "spool");
    REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
    std::string high(32, 'a');
    high[0] = static_cast<char>(0xff);
    const std::string ascii(32, 'z');
    std::filesystem::create_directory(temp.state() / "spool" / high);
    std::filesystem::create_directory(temp.state() / "spool" / ascii);

    const auto inventory_result = enumerate_spool(temp.state().string(), ::getuid());
    REQUIRE(std::holds_alternative<SpoolInventory>(inventory_result));
    const auto& inventory = std::get<SpoolInventory>(inventory_result);
    REQUIRE(inventory.contradiction);
    const auto expected =
        encode_filesystem_diagnostic_path((temp.state() / "spool" / ascii).string());
    REQUIRE(expected);
    CHECK(inventory.contradiction == expected);
}

TEST_CASE("file spool inventory rejects malformed retained children byte-safely", "[file-spool]") {
    const TempTree temp;
    const auto invocation = temp.state() / "spool" / std::string(kInvocation);
    std::filesystem::create_directories(invocation);
    REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
    REQUIRE(::chmod(invocation.c_str(), 0700) == 0);
    const std::string invalid_name(1, static_cast<char>(0xff));
    temp.write(invocation / invalid_name, "payload");
    const auto inventory =
        std::get<SpoolInventory>(enumerate_spool(temp.state().string(), ::getuid()));
    REQUIRE(inventory.contradiction);
    CHECK(inventory.contradiction ==
          encode_filesystem_diagnostic_path((invocation / invalid_name).string()));
}

TEST_CASE("file spool inventory rejects destination hard links", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    REQUIRE(::link(created.local_path.c_str(), (temp.root() / "outside-link").c_str()) == 0);
    const auto inventory =
        std::get<SpoolInventory>(enumerate_spool(temp.state().string(), ::getuid()));
    REQUIRE(inventory.contradiction);
    CHECK(inventory.contradiction == encode_filesystem_diagnostic_path(created.local_path));
}

TEST_CASE("file spool reconciliation distinguishes ready incomplete missing and orphan",
          "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    const auto incomplete = temp.state() / "spool" / std::string(kOtherInvocation);
    std::filesystem::create_directory(incomplete);
    REQUIRE(::chmod(incomplete.c_str(), 0700) == 0);

    const auto inventory =
        std::get<SpoolInventory>(enumerate_spool(temp.state().string(), ::getuid()));
    auto reconciliation_result =
        reconcile_spool_inventory(inventory, {{std::string(kInvocation), "source.bin"},
                                              {std::string(kOtherInvocation), "pending.bin"},
                                              {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "missing.bin"}});
    REQUIRE(std::holds_alternative<SpoolReconciliation>(reconciliation_result));
    const auto& reconciliation = std::get<SpoolReconciliation>(reconciliation_result);
    CHECK(reconciliation.ready_invocations == std::vector{std::string(kInvocation)});
    CHECK(reconciliation.incomplete_invocations == std::vector{std::string(kOtherInvocation)});
    REQUIRE(reconciliation.missing.size() == 1);
    CHECK(reconciliation.missing.front().file_name == "missing.bin");
    CHECK_FALSE(reconciliation.contradiction);

    reconciliation_result = reconcile_spool_inventory(inventory, {});
    REQUIRE(std::holds_alternative<SpoolReconciliation>(reconciliation_result));
    REQUIRE(std::get<SpoolReconciliation>(reconciliation_result).contradiction);
    CHECK(std::filesystem::exists(created.local_path));
}

TEST_CASE("file spool enumeration IO failure is typed and retains root", "[file-spool]") {
    const TempTree temp;
    std::filesystem::create_directory(temp.state() / "spool");
    REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->should_fail = [](FileSpoolStage stage) {
        return stage == FileSpoolStage::DuringRootEnumeration;
    };
    const auto error = require_error(enumerate_spool(temp.state().string(), ::getuid(), {}, hooks));
    CHECK(error.durability_reason == DurabilityReason::ReadFailed);
    CHECK(std::filesystem::exists(temp.state() / "spool"));
}

TEST_CASE("file spool checks deadline and cancellation without filesystem mutation",
          "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    FileSpoolControl expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    CHECK(require_error(prepare_spool_source(temp.source().string(), "/", expired)).kind ==
          FileSpoolErrorKind::TimedOut);

    const std::stop_source stop;
    stop.request_stop();
    FileSpoolControl cancelled;
    cancelled.stop_token = stop.get_token();
    CHECK(require_error(prepare_spool_source(temp.source().string(), "/", cancelled)).kind ==
          FileSpoolErrorKind::Cancelled);
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool"));
}

TEST_CASE("file spool reports source read IO and timestamp representation failures",
          "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    SECTION("read IO") {
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->read = [](FileSpoolIo operation, int, void*, std::size_t) -> ssize_t {
            REQUIRE(operation == FileSpoolIo::Pass1Read);
            errno = EIO;
            return -1;
        };
        const auto error =
            require_error(prepare_spool_source(temp.source().string(), "/", {}, hooks));
        CHECK(error.durability_reason == DurabilityReason::ReadFailed);
    }
    SECTION("timestamp overflow") {
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->mutate_metadata = [](FileSpoolMetadata point, struct stat& status) {
            if (point == FileSpoolMetadata::SourceBefore) {
#if defined(__APPLE__)
                status.st_mtimespec.tv_sec = std::numeric_limits<time_t>::max();
                status.st_mtimespec.tv_nsec = 999'999'999;
#else
                status.st_mtim.tv_sec = std::numeric_limits<time_t>::max();
                status.st_mtim.tv_nsec = 999'999'999;
#endif
            }
        };
        const auto error =
            require_error(prepare_spool_source(temp.source().string(), "/", {}, hooks));
        CHECK(error.durability_reason == DurabilityReason::SchemaError);
    }
    SECTION("source owner mode and link count are not policy") {
        REQUIRE(::chmod(temp.source().c_str(), 0640) == 0);
        REQUIRE(::link(temp.source().c_str(), (temp.root() / "other-link").c_str()) == 0);
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->mutate_metadata = [](FileSpoolMetadata point, struct stat& status) {
            if (point == FileSpoolMetadata::SourceEntry ||
                point == FileSpoolMetadata::SourceBefore ||
                point == FileSpoolMetadata::SourceAfter) {
                ++status.st_uid;
            }
        };
        auto source = require_source(prepare_spool_source(temp.source().string(), "/", {}, hooks));
        CHECK(source.snapshot().size == 7);
    }
}

TEST_CASE("file spool detects destination replacement and retains contradiction", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto destination = temp.state() / "spool" / std::string(kInvocation) / "source.bin";
    auto hooks = mutation_hook(FileSpoolStage::BeforeDestinationRevalidate, [&] {
        REQUIRE(::unlink(destination.c_str()) == 0);
        REQUIRE(::symlink(temp.source().c_str(), destination.c_str()) == 0);
    });
    const auto error = require_error(create_spool_file(
        source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
    CHECK(error.kind == FileSpoolErrorKind::Contradiction);
    CHECK_FALSE(error.cleanup_reference);
    CHECK(std::filesystem::is_symlink(destination));
}

TEST_CASE("file spool revalidates durable destination bytes before publication", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto destination = temp.state() / "spool" / std::string(kInvocation) / "source.bin";

    SECTION("truncate") {
        bool file_synced = false;
        bool invocation_synced = false;
        auto hooks = std::make_shared<testing::FileSpoolHooks>();
        hooks->sync = [&](FileSpoolStage stage, int descriptor) {
            file_synced = file_synced || stage == FileSpoolStage::BeforeFileSync;
            invocation_synced = invocation_synced || stage == FileSpoolStage::BeforeInvocationSync;
            return ::fsync(descriptor);
        };
        hooks->at_stage = [&](FileSpoolStage stage) {
            if (stage == FileSpoolStage::BeforeDestinationRevalidate) {
                REQUIRE(file_synced);
                REQUIRE(invocation_synced);
                REQUIRE(::truncate(destination.c_str(), 3) == 0);
            }
        };
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
        CHECK(error.kind == FileSpoolErrorKind::Contradiction);
    }
    SECTION("same-size rewrite") {
        auto hooks = mutation_hook(FileSpoolStage::BeforeDestinationRevalidate,
                                   [&] { temp.write(destination, "changed"); });
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
        CHECK(error.kind == FileSpoolErrorKind::Contradiction);
    }
}

TEST_CASE("file spool revalidates retained invocation edge before publication", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto invocation = temp.state() / "spool" / std::string(kInvocation);
    const auto moved = temp.state() / "spool" / "moved";
    auto hooks = mutation_hook(FileSpoolStage::BeforeDestinationCreate, [&] {
        REQUIRE(::rename(invocation.c_str(), moved.c_str()) == 0);
        std::filesystem::create_directory(invocation);
        REQUIRE(::chmod(invocation.c_str(), 0700) == 0);
    });
    const auto error = require_error(create_spool_file(
        source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
    CHECK(error.kind == FileSpoolErrorKind::Contradiction);
    CHECK_FALSE(std::filesystem::exists(invocation / "source.bin"));
    CHECK(std::filesystem::exists(moved / "source.bin"));
}

TEST_CASE("file spool revalidates the complete account-state path before publication",
          "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto moved = temp.root() / "moved-state";
    auto hooks = mutation_hook(FileSpoolStage::BeforeDestinationRevalidate, [&] {
        REQUIRE(::rename(temp.state().c_str(), moved.c_str()) == 0);
        std::filesystem::create_directory(temp.state());
        REQUIRE(::chmod(temp.state().c_str(), 0700) == 0);
    });
    const auto error = require_error(create_spool_file(
        source, temp.state().string(), std::string(kInvocation), ::getuid(), {}, hooks));
    CHECK(error.kind == FileSpoolErrorKind::Contradiction);
    CHECK_FALSE(error.cleanup_reference);
    CHECK(std::filesystem::exists(moved / "spool" / std::string(kInvocation) / "source.bin"));
    CHECK_FALSE(
        std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation) / "source.bin"));
}

TEST_CASE("file spool readback observes control after each bounded chunk", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), std::string(200000, 'x'));
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    std::size_t readback_calls = 0;
    auto hooks = std::make_shared<testing::FileSpoolHooks>();

    SECTION("cancelled") {
        std::stop_source stop;
        const FileSpoolControl control{std::nullopt, stop.get_token()};
        hooks->read = [&](FileSpoolIo operation, int descriptor, void* data,
                          std::size_t size) -> ssize_t {
            const auto count = ::read(descriptor, data, size);
            if (operation == FileSpoolIo::DestinationReadback) {
                ++readback_calls;
                CHECK(size <= static_cast<std::size_t>(64) * 1024U);
                stop.request_stop();
            }
            return count;
        };
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), control, hooks));
        CHECK(error.kind == FileSpoolErrorKind::Cancelled);
        CHECK(readback_calls == 1);
    }
    SECTION("timed out") {
        FileSpoolControl control;
        hooks->read = [&](FileSpoolIo operation, int descriptor, void* data,
                          std::size_t size) -> ssize_t {
            const auto count = ::read(descriptor, data, size);
            if (operation == FileSpoolIo::DestinationReadback) {
                ++readback_calls;
                CHECK(size <= static_cast<std::size_t>(64) * 1024U);
                control.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            }
            return count;
        };
        const auto error = require_error(create_spool_file(
            source, temp.state().string(), std::string(kInvocation), ::getuid(), control, hooks));
        CHECK(error.kind == FileSpoolErrorKind::TimedOut);
        CHECK(readback_calls == 1);
    }
}

TEST_CASE("file spool cleanup retry proves an already absent invocation durable", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));

    auto fail_sync = std::make_shared<testing::FileSpoolHooks>();
    fail_sync->should_fail = [](FileSpoolStage stage) {
        return stage == FileSpoolStage::BeforeCleanupRootSync;
    };
    CHECK(require_error(cleanup_spool_file(temp.state().string(), created.reference, ::getuid(), {},
                                           fail_sync))
              .durability_reason == DurabilityReason::DirectorySyncFailed);
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation)));

    std::size_t root_syncs = 0;
    auto retry_hooks = std::make_shared<testing::FileSpoolHooks>();
    retry_hooks->sync = [&](FileSpoolStage stage, int descriptor) {
        if (stage == FileSpoolStage::BeforeCleanupRootSync) {
            ++root_syncs;
        }
        return ::fsync(descriptor);
    };
    const auto retry =
        cleanup_spool_file(temp.state().string(), created.reference, ::getuid(), {}, retry_hooks);
    REQUIRE(std::holds_alternative<SpoolCleanupResult>(retry));
    CHECK_FALSE(std::get<SpoolCleanupResult>(retry).removed);
    CHECK(std::get<SpoolCleanupResult>(retry).root_synced);
    CHECK(root_syncs == 1);
}

TEST_CASE("file spool defers cleanup cancellation through namespace durability", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto created = require_created(
        create_spool_file(source, temp.state().string(), std::string(kInvocation), ::getuid()));
    std::stop_source stop;
    const FileSpoolControl control{std::nullopt, stop.get_token()};
    std::size_t root_syncs = 0;
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->at_stage = [&](FileSpoolStage stage) {
        if (stage == FileSpoolStage::BeforeCleanupInvocationRemove) {
            stop.request_stop();
        }
    };
    hooks->sync = [&](FileSpoolStage stage, int descriptor) {
        if (stage == FileSpoolStage::BeforeCleanupRootSync) {
            ++root_syncs;
        }
        return ::fsync(descriptor);
    };
    const auto error = require_error(
        cleanup_spool_file(temp.state().string(), created.reference, ::getuid(), control, hooks));
    CHECK(error.kind == FileSpoolErrorKind::Cancelled);
    CHECK(root_syncs == 1);
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation)));
}

TEST_CASE("file spool retries uncertain root creation parent sync", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    auto fail_parent_sync = std::make_shared<testing::FileSpoolHooks>();
    fail_parent_sync->should_fail = [](FileSpoolStage stage) {
        return stage == FileSpoolStage::BeforeAccountStateSync;
    };
    CHECK(require_error(create_spool_file(source, temp.state().string(), std::string(kInvocation),
                                          ::getuid(), {}, fail_parent_sync))
              .durability_reason == DurabilityReason::DirectorySyncFailed);
    REQUIRE(std::filesystem::is_directory(temp.state() / "spool"));

    auto retry_source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto retry_error = require_error(create_spool_file(retry_source, temp.state().string(),
                                                             std::string(kInvocation), ::getuid(),
                                                             {}, fail_parent_sync));
    CHECK(retry_error.durability_reason == DurabilityReason::DirectorySyncFailed);
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation)));

    auto final_source = require_source(prepare_spool_source(temp.source().string(), "/"));
    const auto created = require_created(create_spool_file(final_source, temp.state().string(),
                                                           std::string(kInvocation), ::getuid()));
    CHECK(std::filesystem::exists(created.local_path));
}

TEST_CASE("file spool defers root creation cancellation through parent durability",
          "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    std::stop_source stop;
    const FileSpoolControl control{std::nullopt, stop.get_token()};
    std::size_t parent_syncs = 0;
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->at_stage = [&](FileSpoolStage stage) {
        if (stage == FileSpoolStage::AfterRootCreate) {
            stop.request_stop();
        }
    };
    hooks->sync = [&](FileSpoolStage stage, int descriptor) {
        if (stage == FileSpoolStage::BeforeAccountStateSync) {
            ++parent_syncs;
        }
        return ::fsync(descriptor);
    };
    const auto error = require_error(create_spool_file(
        source, temp.state().string(), std::string(kInvocation), ::getuid(), control, hooks));
    CHECK(error.kind == FileSpoolErrorKind::Cancelled);
    CHECK(parent_syncs == 1);
    CHECK(std::filesystem::is_directory(temp.state() / "spool"));
    CHECK_FALSE(std::filesystem::exists(temp.state() / "spool" / std::string(kInvocation)));
}

TEST_CASE("file spool defers invocation cancellation through root durability", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
    std::stop_source stop;
    const FileSpoolControl control{std::nullopt, stop.get_token()};
    std::size_t root_syncs = 0;
    auto hooks = std::make_shared<testing::FileSpoolHooks>();
    hooks->at_stage = [&](FileSpoolStage stage) {
        if (stage == FileSpoolStage::AfterInvocationCreate) {
            stop.request_stop();
        }
    };
    hooks->sync = [&](FileSpoolStage stage, int descriptor) {
        if (stage == FileSpoolStage::BeforeRootSync) {
            ++root_syncs;
        }
        return ::fsync(descriptor);
    };
    const auto error = require_error(create_spool_file(
        source, temp.state().string(), std::string(kInvocation), ::getuid(), control, hooks));
    CHECK(error.kind == FileSpoolErrorKind::Cancelled);
    CHECK(root_syncs == 1);
    REQUIRE(error.cleanup_reference);
    CHECK(std::filesystem::is_directory(temp.state() / "spool" / std::string(kInvocation)));
}

TEST_CASE("file spool stage controls retain typed errors across operations", "[file-spool]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    std::filesystem::create_directory(temp.state() / "spool");
    REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);

    SECTION("create cancellation") {
        std::stop_source stop;
        const FileSpoolControl control{std::nullopt, stop.get_token()};
        auto hooks =
            mutation_hook(FileSpoolStage::BeforeAccountStateOpen, [&] { stop.request_stop(); });
        auto source = require_source(prepare_spool_source(temp.source().string(), "/"));
        CHECK(require_error(create_spool_file(source, temp.state().string(),
                                              std::string(kInvocation), ::getuid(), control, hooks))
                  .kind == FileSpoolErrorKind::Cancelled);
    }
    SECTION("inspect timeout") {
        FileSpoolControl control;
        auto hooks = mutation_hook(FileSpoolStage::AfterAccountStateOpen, [&] {
            control.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        });
        CHECK(require_error(inspect_spool_root(temp.state().string(), ::getuid(), control, hooks))
                  .kind == FileSpoolErrorKind::TimedOut);
    }
    SECTION("enumerate cancellation") {
        std::stop_source stop;
        const FileSpoolControl control{std::nullopt, stop.get_token()};
        auto hooks =
            mutation_hook(FileSpoolStage::BeforeRootEnumeration, [&] { stop.request_stop(); });
        CHECK(require_error(enumerate_spool(temp.state().string(), ::getuid(), control, hooks))
                  .kind == FileSpoolErrorKind::Cancelled);
    }
    SECTION("cleanup timeout") {
        const SpoolRef reference{
            "spool/00112233445566778899aabbccddeeff/source.bin",
            FileSnapshot{"/source.bin", "source.bin", 1, "sha256:00", 1, 1, 1, 1}};
        FileSpoolControl control;
        auto hooks = mutation_hook(FileSpoolStage::BeforeCleanupOpen, [&] {
            control.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        });
        CHECK(require_error(
                  cleanup_spool_file(temp.state().string(), reference, ::getuid(), control, hooks))
                  .kind == FileSpoolErrorKind::TimedOut);
    }
}

TEST_CASE("file spool reconciliation reports first sorted child unequal expected", "[file-spool]") {
    const TempTree temp;
    const auto invocation = temp.state() / "spool" / std::string(kInvocation);
    std::filesystem::create_directories(invocation);
    REQUIRE(::chmod((temp.state() / "spool").c_str(), 0700) == 0);
    REQUIRE(::chmod(invocation.c_str(), 0700) == 0);
    temp.write(invocation / "a", "expected");
    temp.write(invocation / "b", "extra-one");
    temp.write(invocation / "z", "extra-two");

    const auto inventory =
        std::get<SpoolInventory>(enumerate_spool(temp.state().string(), ::getuid()));
    const auto reconciled = reconcile_spool_inventory(inventory, {{std::string(kInvocation), "a"}});
    REQUIRE(std::holds_alternative<SpoolReconciliation>(reconciled));
    const auto& contradiction = std::get<SpoolReconciliation>(reconciled).contradiction;
    REQUIRE(contradiction);
    const auto expected = encode_filesystem_diagnostic_path((invocation / "b").string());
    REQUIRE(expected);
    CHECK(contradiction->bytes_hex == expected->bytes_hex);
}

TEST_CASE("file spool concurrent same invocation has one winner and no overwrite",
          "[file-spool][concurrency]") {
    const TempTree temp;
    temp.write(temp.source("one"), "first");
    temp.write(temp.source("two"), "second");
    auto first_source = require_source(prepare_spool_source(temp.source("one").string(), "/"));
    auto second_source = require_source(prepare_spool_source(temp.source("two").string(), "/"));
    std::optional<CreateSpoolFileResult> first;
    std::optional<CreateSpoolFileResult> second;
    std::thread left([&] {
        first = create_spool_file(first_source, temp.state().string(), std::string(kInvocation),
                                  ::getuid());
    });
    std::thread right([&] {
        second = create_spool_file(second_source, temp.state().string(), std::string(kInvocation),
                                   ::getuid());
    });
    left.join();
    right.join();
    REQUIRE(first);
    REQUIRE(second);
    const auto winners = static_cast<int>(std::holds_alternative<CreatedSpool>(*first)) +
                         static_cast<int>(std::holds_alternative<CreatedSpool>(*second));
    CHECK(winners == 1);
    const auto& loser = std::holds_alternative<FileSpoolError>(*first) ? *first : *second;
    CHECK(std::get<FileSpoolError>(loser).kind == FileSpoolErrorKind::Contradiction);
}

TEST_CASE("file spool concurrent distinct invocations remain independent",
          "[file-spool][concurrency]") {
    const TempTree temp;
    temp.write(temp.source(), "payload");
    auto first_source = require_source(prepare_spool_source(temp.source().string(), "/"));
    auto second_source = require_source(prepare_spool_source(temp.source().string(), "/"));
    std::optional<CreateSpoolFileResult> first;
    std::optional<CreateSpoolFileResult> second;
    std::thread left([&] {
        first = create_spool_file(first_source, temp.state().string(), std::string(kInvocation),
                                  ::getuid());
    });
    std::thread right([&] {
        second = create_spool_file(second_source, temp.state().string(),
                                   std::string(kOtherInvocation), ::getuid());
    });
    left.join();
    right.join();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(std::holds_alternative<CreatedSpool>(*first));
    CHECK(std::holds_alternative<CreatedSpool>(*second));
}
