#include "daemon/removal_filesystem.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::daemon;

namespace {

class TempTree final {
  public:
    TempTree() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-removal-fs-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        parent_ = root_ / "accounts";
        std::filesystem::create_directory(parent_);
        REQUIRE(::chmod(parent_.c_str(), 0700) == 0);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    [[nodiscard]] std::filesystem::path account(std::string_view name = "work") const {
        return parent_ / name;
    }

    void create_account(std::string_view name = "work") const {
        std::filesystem::create_directory(account(name));
        REQUIRE(::chmod(account(name).c_str(), 0700) == 0);
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path parent_;
};

constexpr std::string_view kInvocation = "00112233445566778899aabbccddeeff";

CapturedRemovalRoot capture(const std::filesystem::path& path) {
    RemovalFilesystemFailure failure;
    auto captured = capture_removal_root(path.string(), ::getuid(), failure);
    INFO(failure.reason);
    REQUIRE(captured);
    return *captured;
}

} // namespace

TEST_CASE("removal root capture records exact identity or planned absence", "[removal][fs]") {
    const TempTree temp;
    auto absent = capture(temp.account());
    CHECK_FALSE(absent.identity);

    temp.create_account();
    auto present = capture(temp.account());
    REQUIRE(present.identity);
    CHECK(present.identity->path == temp.account());
    CHECK(present.identity->owner == static_cast<std::uint64_t>(::getuid()));

    RemovalFilesystemFailure failure;
    REQUIRE(revalidate_removal_root(present, ::getuid(), failure));
    REQUIRE(::rename(temp.account().c_str(), temp.account("moved").c_str()) == 0);
    CHECK_FALSE(revalidate_removal_root(present, ::getuid(), failure));
    CHECK(failure.reason == "path_changed");
}

TEST_CASE("removal capture rejects unsafe roots and fake mount boundaries", "[removal][fs]") {
    const TempTree temp;
    const auto outside = temp.account("outside");
    std::filesystem::create_directory(outside);
    REQUIRE(::symlink(outside.c_str(), temp.account().c_str()) == 0);
    RemovalFilesystemFailure failure;
    CHECK_FALSE(capture_removal_root(temp.account().string(), ::getuid(), failure));
    CHECK(failure.reason == "path_invalid");

    REQUIRE(::unlink(temp.account().c_str()) == 0);
    temp.create_account();
    auto hooks = std::make_shared<testing::RemovalFilesystemHooks>();
    hooks->force_device_boundary = [](std::string_view) { return true; };
    CHECK_FALSE(capture_removal_root(temp.account().string(), ::getuid(), failure, hooks));
    CHECK(failure.reason == "mount_boundary");
}

TEST_CASE("removal requires mount identity and refuses same-device nested mount traversal",
          "[removal][fs][mount]") {
    const TempTree temp;
    temp.create_account();
    std::filesystem::create_directory(temp.account() / "mounted");
    { std::ofstream(temp.account() / "mounted" / "keep") << "outside"; }

    auto unavailable = std::make_shared<testing::RemovalFilesystemHooks>();
    unavailable->mount_identity = [](int) { return std::optional<std::uint64_t>{}; };
    RemovalFilesystemFailure failure;
    CHECK_FALSE(capture_removal_root(temp.account().string(), ::getuid(), failure, unavailable));
    CHECK(failure.reason == "mount_boundary");

    const auto captured = capture(temp.account());
    auto different_nested_mount = std::make_shared<testing::RemovalFilesystemHooks>();
    std::size_t mount_observations = 0;
    different_nested_mount->mount_identity = [&](int) {
        ++mount_observations;
        return std::optional<std::uint64_t>{mount_observations < 5 ? 17U : 29U};
    };
    CHECK_FALSE(delete_removal_root(captured, kInvocation, "data", ::getuid(), failure,
                                    different_nested_mount));
    CHECK(failure.reason == "mount_boundary");
    CHECK(mount_observations == 5);
    const auto staged =
        temp.account().parent_path() / (".tgcli-removal-" + std::string(kInvocation) + "-data");
    CHECK(std::filesystem::exists(staged / "mounted" / "keep"));
}

TEST_CASE("descriptor-relative removal deletes nested contents without following symlinks",
          "[removal][fs]") {
    const TempTree temp;
    temp.create_account();
    std::filesystem::create_directories(temp.account() / "a" / "b");
    { std::ofstream(temp.account() / "a" / "b" / "value") << "payload"; }
    const auto outside = temp.account("outside");
    std::filesystem::create_directory(outside);
    { std::ofstream(outside / "keep") << "untouched"; }
    REQUIRE(::symlink(outside.c_str(), (temp.account() / "link").c_str()) == 0);

    const auto captured = capture(temp.account());
    RemovalFilesystemFailure failure;
    REQUIRE(delete_removal_root(captured, kInvocation, "data", ::getuid(), failure));
    CHECK_FALSE(std::filesystem::exists(temp.account()));
    CHECK(std::filesystem::exists(outside / "keep"));
}

TEST_CASE("removal resumes from deterministic staging after a crash", "[removal][fs]") {
    const TempTree temp;
    temp.create_account();
    { std::ofstream(temp.account() / "value") << "payload"; }
    const auto captured = capture(temp.account());
    auto hooks = std::make_shared<testing::RemovalFilesystemHooks>();
    hooks->should_fail = [](RemovalFilesystemStage stage, std::string_view) {
        return stage == RemovalFilesystemStage::AfterStageRename;
    };
    RemovalFilesystemFailure failure;
    CHECK_FALSE(delete_removal_root(captured, kInvocation, "data", ::getuid(), failure, hooks));
    CHECK(failure.reason == "io_error");
    CHECK_FALSE(std::filesystem::exists(temp.account()));

    REQUIRE(delete_removal_root(captured, kInvocation, "data", ::getuid(), failure));
    CHECK_FALSE(std::filesystem::exists(temp.account()));
}

TEST_CASE("removal refuses original-path replacement after staging", "[removal][fs]") {
    const TempTree temp;
    temp.create_account();
    { std::ofstream(temp.account() / "captured") << "payload"; }
    const auto captured = capture(temp.account());
    auto hooks = std::make_shared<testing::RemovalFilesystemHooks>();
    hooks->at_stage = [&](RemovalFilesystemStage stage, std::string_view) {
        if (stage == RemovalFilesystemStage::AfterStageRename) {
            temp.create_account();
            std::ofstream(temp.account() / "replacement") << "keep";
        }
    };
    RemovalFilesystemFailure failure;
    CHECK_FALSE(delete_removal_root(captured, kInvocation, "state", ::getuid(), failure, hooks));
    CHECK(failure.reason == "path_changed");
    CHECK(std::filesystem::exists(temp.account() / "replacement"));
}

TEST_CASE("planned absent roots cannot appear before deletion", "[removal][fs]") {
    const TempTree temp;
    const auto captured = capture(temp.account());
    temp.create_account();
    RemovalFilesystemFailure failure;
    CHECK_FALSE(revalidate_removal_root(captured, ::getuid(), failure));
    CHECK(failure.reason == "path_changed");
    CHECK_FALSE(delete_removal_root(captured, kInvocation, "data", ::getuid(), failure));
    CHECK(failure.reason == "path_changed");
    CHECK(std::filesystem::exists(temp.account()));
}
