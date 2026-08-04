#include "common/paths.hpp"
#include "core/td_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using tgcli::core::TdLogConfiguration;
using tgcli::core::TdLogSink;

namespace {

class PrivateTree {
  public:
    PrivateTree() {
        std::vector<char> pattern{'/', 't', 'm', 'p', '/', 't', 'g', 'c', 'l', 'i', '-',
                                  'l', 'o', 'g', '-', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
        char* created = ::mkdtemp(pattern.data());
        if (created == nullptr || ::chmod(created, 0700) != 0) {
            throw std::runtime_error("cannot create private logging test tree");
        }
        root_ = created;
    }

    ~PrivateTree() {
        std::filesystem::remove_all(root_);
    }

    [[nodiscard]] std::string directory(std::string_view relative) const {
        const auto result = root_ + "/" + std::string(relative);
        std::filesystem::create_directories(result);
        std::filesystem::permissions(result, std::filesystem::perms::owner_all);
        return result;
    }

    [[nodiscard]] std::string file(std::string_view relative) const {
        return root_ + "/" + std::string(relative);
    }

    [[nodiscard]] const std::string& root() const {
        return root_;
    }

  private:
    std::string root_;
};

std::string read_bytes(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::string& filename, std::string_view bytes, mode_t mode = 0600) {
    const int fd = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    REQUIRE(fd >= 0);
    std::string_view remaining = bytes;
    while (!remaining.empty()) {
        const auto count = ::write(fd, remaining.data(), remaining.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        REQUIRE(count > 0);
        remaining.remove_prefix(static_cast<std::size_t>(count));
    }
    REQUIRE(::close(fd) == 0);
    REQUIRE(::chmod(filename.c_str(), mode) == 0);
}

TdLogConfiguration logging_at(const std::string& directory, std::uint64_t max_size = 1024 * 1024) {
    return {.file_path = directory + "/tdlib.log", .max_file_size = max_size};
}

std::unique_ptr<TdLogSink> require_sink(const TdLogConfiguration& configuration) {
    std::string error;
    auto sink = TdLogSink::create(configuration, ::getuid(), error);
    INFO(error);
    REQUIRE(sink != nullptr);
    return sink;
}

} // namespace

TEST_CASE("TDLib log records rotate whole and retain exactly four generations",
          "[logging][rotation]") {
    const PrivateTree tree;
    const auto directory = tree.directory("state");
    const auto configuration = logging_at(directory, 12);
    auto sink = require_sink(configuration);
    const std::vector<std::string> sentinels{"phone-secret", "code-secret", "password-secret",
                                             "database-key-secret"};
    std::string error;
    for (const auto& sentinel : sentinels) {
        REQUIRE(sink->append(tgcli::core::kTdLogInfoVerbosity,
                             "serialized request credential=" + sentinel + "\n", error));
    }
    for (int index = 1; index <= 20; ++index) {
        const auto record = (index < 10 ? "0" : "") + std::to_string(index) + "\n";
        REQUIRE(sink->append(tgcli::core::kTdLogVerbosity, record, error));
    }

    const auto paths = sink->log_paths();
    REQUIRE(paths.size() == tgcli::core::kTdLogRotatedFileCount + 1);
    CHECK(read_bytes(paths[0]) == "17\n18\n19\n20\n");
    CHECK(read_bytes(paths[1]) == "13\n14\n15\n16\n");
    CHECK(read_bytes(paths[2]) == "09\n10\n11\n12\n");
    CHECK(read_bytes(paths[3]) == "05\n06\n07\n08\n");
    CHECK(read_bytes(paths[4]) == "01\n02\n03\n04\n");
    CHECK_FALSE(std::filesystem::exists(configuration.file_path + ".5"));
    for (const auto& log_path : paths) {
        const auto content = read_bytes(log_path);
        for (const auto& sentinel : sentinels) {
            CHECK(content.find(sentinel) == std::string::npos);
        }
        struct stat status {};
        REQUIRE(::lstat(log_path.c_str(), &status) == 0);
        CHECK(S_ISREG(status.st_mode));
        CHECK((status.st_mode & 07777) == 0600);
        CHECK(status.st_uid == ::getuid());
        CHECK(status.st_nlink == 1);
    }
}

TEST_CASE("TDLib log sink serializes concurrent records without tearing",
          "[logging][concurrency]") {
    const PrivateTree tree;
    const auto configuration = logging_at(tree.directory("state"));
    auto sink = require_sink(configuration);
    constexpr int kThreads = 8;
    constexpr int kRecords = 100;
    std::atomic<bool> append_ok{true};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&, thread] {
            for (int record = 0; record < kRecords; ++record) {
                std::string error;
                const auto line =
                    "thread=" + std::to_string(thread) + ",record=" + std::to_string(record) + "\n";
                if (!sink->append(tgcli::core::kTdLogVerbosity, line, error)) {
                    append_ok.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE(append_ok.load(std::memory_order_relaxed));

    std::set<std::string> lines;
    std::string content = read_bytes(configuration.file_path);
    while (!content.empty()) {
        const auto newline = content.find('\n');
        REQUIRE(newline != std::string::npos);
        lines.insert(content.substr(0, newline));
        content.erase(0, newline + 1);
    }
    CHECK(lines.size() == kThreads * kRecords);
}

TEST_CASE("production and test-DC accounts write only their isolated log roots",
          "[logging][process][test-dc]") {
    const PrivateTree tree;
    tgcli::paths::Environment production;
    production.xdg_state_home = tree.root();
    production.home = tree.root();
    production.uid = ::getuid();
    auto test_dc = production;
    test_dc.test_dc = true;
    const auto production_dir = tree.directory("tgcli/accounts/main");
    const auto test_dir = tree.directory("tgcli-test/accounts/main");
    const std::array configurations{
        logging_at(production_dir),
        logging_at(test_dir),
    };
    const std::array records{"production-account\n", "test-dc-account\n"};

    std::array<pid_t, 2> children{};
    for (std::size_t index = 0; index < children.size(); ++index) {
        children[index] = ::fork();
        REQUIRE(children[index] >= 0);
        if (children[index] == 0) {
            std::string error;
            auto sink = TdLogSink::create(configurations[index], ::getuid(), error);
            if (sink == nullptr ||
                !sink->append(tgcli::core::kTdLogVerbosity, records[index], error)) {
                ::_exit(1);
            }
            ::_exit(0);
        }
    }
    for (const auto child : children) {
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }

    CHECK(configurations[0].file_path == tgcli::paths::tdlib_log_file("main", production));
    CHECK(configurations[1].file_path == tgcli::paths::tdlib_log_file("main", test_dc));
    CHECK(read_bytes(configurations[0].file_path) == records[0]);
    CHECK(read_bytes(configurations[1].file_path) == records[1]);
}

TEST_CASE("TDLib log setup and rotation reject unsafe filesystem entries", "[logging][security]") {
    const PrivateTree tree;
    const auto target = tree.file("target");
    write_bytes(target, "unchanged");

    SECTION("symlink parent") {
        const auto real_parent = tree.directory("real-parent");
        const auto link_parent = tree.file("link-parent");
        REQUIRE(::symlink(real_parent.c_str(), link_parent.c_str()) == 0);
        std::string error;
        CHECK(TdLogSink::create(logging_at(link_parent), ::getuid(), error) == nullptr);
        CHECK_FALSE(error.empty());
    }

    SECTION("symlink active file") {
        const auto directory = tree.directory("symlink-active");
        REQUIRE(::symlink(target.c_str(), (directory + "/tdlib.log").c_str()) == 0);
        std::string error;
        CHECK(TdLogSink::create(logging_at(directory), ::getuid(), error) == nullptr);
        CHECK(read_bytes(target) == "unchanged");
    }

    SECTION("hard-linked active file") {
        const auto directory = tree.directory("hardlink-active");
        REQUIRE(::link(target.c_str(), (directory + "/tdlib.log").c_str()) == 0);
        std::string error;
        CHECK(TdLogSink::create(logging_at(directory), ::getuid(), error) == nullptr);
        CHECK(read_bytes(target) == "unchanged");
    }

    SECTION("unsafe active file mode") {
        const auto directory = tree.directory("mode-active");
        write_bytes(directory + "/tdlib.log", "unsafe", 0644);
        std::string error;
        CHECK(TdLogSink::create(logging_at(directory), ::getuid(), error) == nullptr);
    }

    SECTION("unsafe rotated entry") {
        const auto directory = tree.directory("rotated-symlink");
        REQUIRE(::symlink(target.c_str(), (directory + "/tdlib.log.1").c_str()) == 0);
        std::string error;
        CHECK(TdLogSink::create(logging_at(directory), ::getuid(), error) == nullptr);
        CHECK(read_bytes(target) == "unchanged");
    }

    SECTION("post-open active replacement") {
        const auto directory = tree.directory("replacement");
        const auto configuration = logging_at(directory, 8);
        auto sink = require_sink(configuration);
        std::string error;
        REQUIRE(sink->append(tgcli::core::kTdLogVerbosity, "first\n", error));
        REQUIRE(::unlink(configuration.file_path.c_str()) == 0);
        REQUIRE(::symlink(target.c_str(), configuration.file_path.c_str()) == 0);
        CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "second\n", error));
        CHECK_FALSE(error.empty());
        CHECK(read_bytes(target) == "unchanged");
    }

    SECTION("post-open directory replacement") {
        const auto directory = tree.directory("directory-replacement");
        const auto moved_directory = tree.file("directory-replacement-moved");
        const auto configuration = logging_at(directory);
        auto sink = require_sink(configuration);
        std::string error;
        REQUIRE(sink->append(tgcli::core::kTdLogVerbosity, "first\n", error));
        REQUIRE(::rename(directory.c_str(), moved_directory.c_str()) == 0);
        REQUIRE(::mkdir(directory.c_str(), 0700) == 0);
        CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "second\n", error));
        CHECK_FALSE(error.empty());
        CHECK(read_bytes(moved_directory + "/tdlib.log") == "first\n");
        CHECK_FALSE(std::filesystem::exists(configuration.file_path));
    }

    SECTION("post-open regular generation replacement") {
        const auto directory = tree.directory("regular-generation-replacement");
        const auto configuration = logging_at(directory, 8);
        auto sink = require_sink(configuration);
        std::string error;
        REQUIRE(sink->append(tgcli::core::kTdLogVerbosity, "first\n", error));
        const auto generation = configuration.file_path + ".4";
        const auto displaced = generation + ".displaced";
        REQUIRE(::rename(generation.c_str(), displaced.c_str()) == 0);
        write_bytes(generation, "sentinel");
        struct stat before {};
        REQUIRE(::lstat(generation.c_str(), &before) == 0);

        CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "second\n", error));
        CHECK_FALSE(error.empty());
        struct stat after {};
        REQUIRE(::lstat(generation.c_str(), &after) == 0);
        CHECK(before.st_dev == after.st_dev);
        CHECK(before.st_ino == after.st_ino);
        CHECK(read_bytes(generation) == "sentinel");
        CHECK(read_bytes(configuration.file_path) == "first\n");

        const auto poisoned_error = error;
        CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "third\n", error));
        CHECK(error != poisoned_error);
        CHECK(read_bytes(generation) == "sentinel");
        CHECK(read_bytes(configuration.file_path) == "first\n");
    }
}

TEST_CASE("TDLib log rotation closes the validation-to-mutation replacement race",
          "[logging][security][race]") {
    const PrivateTree tree;
    const auto directory = tree.directory("rotation-race");
    const auto configuration = logging_at(directory, 8);
    const auto generation = configuration.file_path + ".4";
    const auto displaced = generation + ".displaced";
    bool hook_ran = false;
    struct stat sentinel_before {};
    tgcli::core::detail::TdLogTestHooks hooks;
    hooks.after_rotation_validation = [&] {
        hook_ran = true;
        REQUIRE(::rename(generation.c_str(), displaced.c_str()) == 0);
        write_bytes(generation, "race-sentinel");
        REQUIRE(::lstat(generation.c_str(), &sentinel_before) == 0);
    };
    std::string error;
    auto sink = TdLogSink::create_for_test(configuration, ::getuid(), std::move(hooks), error);
    INFO(error);
    REQUIRE(sink != nullptr);
    REQUIRE(sink->append(tgcli::core::kTdLogVerbosity, "first\n", error));

    CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "second\n", error));
    CHECK(hook_ran);
    CHECK_FALSE(error.empty());
    struct stat sentinel_after {};
    REQUIRE(::lstat(generation.c_str(), &sentinel_after) == 0);
    CHECK(sentinel_before.st_dev == sentinel_after.st_dev);
    CHECK(sentinel_before.st_ino == sentinel_after.st_ino);
    CHECK(read_bytes(generation) == "race-sentinel");
    CHECK(read_bytes(configuration.file_path) == "first\n");
}

TEST_CASE("TDLib log sink rolls back partial records and suppresses later writes",
          "[logging][failure]") {
    const PrivateTree tree;
    const auto configuration = logging_at(tree.directory("partial-record"));
    bool inject_failure = false;
    int injected_calls = 0;
    tgcli::core::detail::TdLogTestHooks hooks;
    hooks.write_at = [&](int fd, const void* bytes, std::size_t size, off_t offset) -> ssize_t {
        if (!inject_failure) {
            return ::pwrite(fd, bytes, size, offset);
        }
        ++injected_calls;
        if (injected_calls == 1) {
            return ::pwrite(fd, bytes, std::min<std::size_t>(3, size), offset);
        }
        errno = ENOSPC;
        return -1;
    };
    std::string error;
    auto sink = TdLogSink::create_for_test(configuration, ::getuid(), std::move(hooks), error);
    INFO(error);
    REQUIRE(sink != nullptr);
    REQUIRE(sink->append(tgcli::core::kTdLogVerbosity, "stable\n", error));
    inject_failure = true;
    const std::string partial_secret = "partial-secret\n";
    CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, partial_secret, error));
    CHECK(injected_calls == 2);
    CHECK(error.find(partial_secret) == std::string::npos);
    CHECK(read_bytes(configuration.file_path) == "stable\n");

    CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "later-secret\n", error));
    CHECK(injected_calls == 2);
    CHECK(error == "TDLib logging is disabled after a previous sink failure");
    CHECK(read_bytes(configuration.file_path) == "stable\n");
}

TEST_CASE("TDLib log sink rolls back the full generation set and poisons rotation",
          "[logging][failure][rotation]") {
    const PrivateTree tree;
    const auto configuration = logging_at(tree.directory("partial-rotation"), 8);
    bool inject_failure = false;
    int injected_calls = 0;
    tgcli::core::detail::TdLogTestHooks hooks;
    hooks.write_at = [&](int fd, const void* bytes, std::size_t size, off_t offset) -> ssize_t {
        if (!inject_failure) {
            return ::pwrite(fd, bytes, size, offset);
        }
        ++injected_calls;
        if (injected_calls == 1) {
            return ::pwrite(fd, bytes, size, offset);
        }
        if (injected_calls == 2) {
            return ::pwrite(fd, bytes, std::min<std::size_t>(3, size), offset);
        }
        errno = ENOSPC;
        return -1;
    };
    std::string error;
    auto sink = TdLogSink::create_for_test(configuration, ::getuid(), std::move(hooks), error);
    INFO(error);
    REQUIRE(sink != nullptr);
    for (const std::string_view record : {"one001\n", "two002\n", "three03\n", "four004\n"}) {
        REQUIRE(sink->append(tgcli::core::kTdLogVerbosity, record, error));
    }
    const auto paths = sink->log_paths();
    std::vector<std::string> snapshots;
    snapshots.reserve(paths.size());
    std::ranges::transform(paths, std::back_inserter(snapshots), read_bytes);
    inject_failure = true;

    CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "five005\n", error));
    CHECK(injected_calls == 3);
    for (std::size_t index = 0; index < paths.size(); ++index) {
        CHECK(read_bytes(paths[index]) == snapshots[index]);
    }
    CHECK_FALSE(sink->append(tgcli::core::kTdLogVerbosity, "later\n", error));
    CHECK(injected_calls == 3);
    for (std::size_t index = 0; index < paths.size(); ++index) {
        CHECK(read_bytes(paths[index]) == snapshots[index]);
    }
}
