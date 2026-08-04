#include "common/daemon_lock.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;

namespace {

constexpr std::string_view kToken = "0123456789abcdef0123456789abcdef";

std::string valid_start() {
#if defined(__linux__)
    return "linux:42";
#elif defined(__APPLE__)
    return "macos:42:7";
#else
    return "unsupported:42";
#endif
}

std::string valid_record(std::string_view pid = "1") {
    return "tgcli-lock-v1 " + std::string(pid) + " " + valid_start() + " " + std::string(kToken) +
           "\n";
}

class TempLockFile {
  public:
    TempLockFile() {
        std::string pattern = "/tmp/tgcli-daemon-lock-test-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        filename_ = root_ + "/daemon.lock";
    }

    TempLockFile(const TempLockFile&) = delete;
    TempLockFile& operator=(const TempLockFile&) = delete;
    TempLockFile(TempLockFile&&) = delete;
    TempLockFile& operator=(TempLockFile&&) = delete;

    ~TempLockFile() {
        std::filesystem::remove_all(root_);
    }

    [[nodiscard]] const std::string& filename() const {
        return filename_;
    }

    void write(std::string_view bytes) const {
        const int fd = ::open(filename_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        REQUIRE(fd >= 0);
        REQUIRE(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
        REQUIRE(::close(fd) == 0);
        REQUIRE(::chmod(filename_.c_str(), 0600) == 0);
    }

    [[nodiscard]] std::string read() const {
        const std::ifstream input(filename_, std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

  private:
    std::string root_;
    std::string filename_;
};

struct AcquireBarrierContext {
    int events = -1;
    int resumes = -1;
};

void acquire_barrier(daemon_lock::detail::AcquireStage stage, void* raw_context) {
    auto* context = static_cast<AcquireBarrierContext*>(raw_context);
    const char event = static_cast<char>('0' + static_cast<int>(stage));
    char resume = 0;
    if (::write(context->events, &event, 1) != 1 || ::read(context->resumes, &resume, 1) != 1 ||
        resume != '1') {
        ::_exit(90);
    }
}

char read_byte(int fd) {
    char value = 0;
    ssize_t count = -1;
    do {
        count = ::read(fd, &value, 1);
    } while (count < 0 && errno == EINTR);
    REQUIRE(count == 1);
    return value;
}

void write_byte(int fd, char value) {
    ssize_t count = -1;
    do {
        count = ::write(fd, &value, 1);
    } while (count < 0 && errno == EINTR);
    REQUIRE(count == 1);
}

} // namespace

TEST_CASE("frozen daemon identity record accepts PID 1", "[daemon-lock][r4]") {
    daemon_lock::Identity identity;
    std::string error;
    REQUIRE(daemon_lock::parse_identity_record(valid_record(), identity, error));
    CHECK(identity.pid == 1);
    CHECK(identity.process_start == valid_start());
    CHECK(identity.control_token == kToken);
    CHECK(daemon_lock::owner_pid_matches(identity.pid, 1, error));
}

TEST_CASE("frozen daemon identity owner and fields fail closed", "[daemon-lock][r4]") {
    daemon_lock::Identity identity;
    std::string error;
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("0"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record() + "extra\n", identity, error));
    REQUIRE(daemon_lock::parse_identity_record(valid_record(), identity, error));
    CHECK_FALSE(daemon_lock::owner_pid_matches(identity.pid, 2, error));
}

TEST_CASE("frozen daemon identity parser is byte exact", "[daemon-lock][r1]") {
    daemon_lock::Identity identity;
    std::string error;
    const std::string valid = valid_record();

    CHECK_FALSE(
        daemon_lock::parse_identity_record(valid.substr(0, valid.size() - 1), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid + " ", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 " + valid_start() + " " + std::string(kToken) + " \n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1\t1 " + valid_start() + " " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("01"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("+1"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("-1"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        valid_record("999999999999999999999999999999999999"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 arbitrary:42 " + std::string(kToken) + "\n", identity, error));
#if defined(__linux__)
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:42:7 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:0 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:01 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:+1 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record("tgcli-lock-v1 1 linux:18446744073709551616 " +
                                                       std::string(kToken) + "\n",
                                                   identity, error));
#elif defined(__APPLE__)
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:42 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:01:7 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:42:07 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:42:1000000 " + std::string(kToken) + "\n", identity, error));
#endif
    CHECK_FALSE(daemon_lock::parse_identity_record("tgcli-lock-v1 1 " + valid_start() +
                                                       " 0123456789abcdef0123456789abcdeF\n",
                                                   identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record("tgcli-lock-v1 1 " + valid_start() +
                                                       " 0123456789abcdef0123456789abcdeg\n",
                                                   identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 " + valid_start() + " 0123456789abcdef\n", identity, error));
}

TEST_CASE("owner observation distinguishes transitions from stable corruption",
          "[daemon-lock][r3]") {
    const std::string record = valid_record("42");
    daemon_lock::Identity identity{42, valid_start(), std::string(kToken)};
    std::string error;
    using daemon_lock::detail::ObservationStatus;
    const auto classify = [&](bool final_held, pid_t final_owner,
                              std::optional<std::string_view> initial_record,
                              std::optional<std::string_view> final_record,
                              const daemon_lock::Identity* parsed,
                              std::optional<std::string_view> live_start) {
        return daemon_lock::detail::classify_owner_observation(
            42, final_held, final_owner, initial_record, final_record, parsed, live_start, error);
    };

    CHECK(classify(true, 42, record, record, &identity, identity.process_start) ==
          ObservationStatus::Stable);
    CHECK(classify(false, -1, record, record, &identity, identity.process_start) ==
          ObservationStatus::Transition);
    CHECK(classify(true, 43, record, record, &identity, identity.process_start) ==
          ObservationStatus::Transition);
    CHECK(classify(true, 42, record, record + " ", &identity, identity.process_start) ==
          ObservationStatus::Transition);
    CHECK(classify(true, 42, std::nullopt, record, nullptr, std::nullopt) ==
          ObservationStatus::Transition);

    daemon_lock::Identity wrong_pid = identity;
    wrong_pid.pid = 43;
    CHECK(classify(true, 42, record, record, &wrong_pid, identity.process_start) ==
          ObservationStatus::Invalid);
    CHECK(classify(true, 42, record, record, &identity, "linux:999") == ObservationStatus::Invalid);
    CHECK(classify(true, 42, std::nullopt, std::nullopt, nullptr, std::nullopt) ==
          ObservationStatus::Invalid);
}

TEST_CASE("daemon identity publication is hidden behind a nonblocking bootstrap handoff",
          "[daemon-lock][daemon-control]") {
    const TempLockFile lock;
    lock.write(valid_record());
    std::array<int, 2> events{-1, -1};
    std::array<int, 2> resumes{-1, -1};
    REQUIRE(::pipe(events.data()) == 0);
    REQUIRE(::pipe(resumes.data()) == 0);
    const pid_t publisher = ::fork();
    REQUIRE(publisher >= 0);
    if (publisher == 0) {
        ::close(events[0]);
        ::close(resumes[1]);
        AcquireBarrierContext context{events[1], resumes[0]};
        const daemon_lock::detail::AcquireHooks hooks{acquire_barrier, &context};
        daemon_lock::Identity identity;
        std::string error;
        const int lock_fd = daemon_lock::acquire(lock.filename(), identity, error, &hooks);
        if (lock_fd < 0) {
            ::_exit(91);
        }
        constexpr char published = 'P';
        if (::write(events[1], &published, 1) != 1) {
            ::_exit(92);
        }
        char release = 0;
        if (::read(resumes[0], &release, 1) != 1 || release != '1') {
            ::_exit(93);
        }
        ::close(lock_fd);
        ::_exit(0);
    }
    ::close(events[1]);
    ::close(resumes[0]);

    for (const auto stage : {daemon_lock::detail::AcquireStage::BootstrapLocked,
                             daemon_lock::detail::AcquireStage::OwnerLocked,
                             daemon_lock::detail::AcquireStage::RecordTruncated,
                             daemon_lock::detail::AcquireStage::RecordPublished}) {
        REQUIRE(read_byte(events[0]) == static_cast<char>('0' + static_cast<int>(stage)));
        std::optional<daemon_lock::OwnerWatch> owner;
        std::string error;
        CHECK(daemon_lock::inspect_owner(lock.filename(), getuid(), owner, error) ==
              daemon_lock::OwnerStatus::Transition);
        CHECK(error.empty());
        owner.reset();
        write_byte(resumes[1], '1');
    }
    REQUIRE(read_byte(events[0]) == 'P');

    std::optional<daemon_lock::OwnerWatch> owner;
    std::string error;
    REQUIRE(daemon_lock::inspect_owner(lock.filename(), getuid(), owner, error) ==
            daemon_lock::OwnerStatus::Held);
    REQUIRE(owner.has_value());
    CHECK(owner->identity().pid == publisher);
    const std::string published_record = lock.read();
    owner.reset();

    daemon_lock::Identity competing_identity;
    CHECK(daemon_lock::acquire(lock.filename(), competing_identity, error) < 0);
    CHECK(error.find("cannot establish daemon identity lock") != std::string::npos);
    CHECK(lock.read() == published_record);

    write_byte(resumes[1], '1');
    int status = 0;
    REQUIRE(::waitpid(publisher, &status, 0) == publisher);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    owner.reset();
    CHECK(daemon_lock::inspect_owner(lock.filename(), getuid(), owner, error) ==
          daemon_lock::OwnerStatus::Released);
    CHECK(lock.read() == published_record);
    ::close(events[0]);
    ::close(resumes[1]);
}

TEST_CASE("crash after identity truncation remains a stable invalid unlocked record",
          "[daemon-lock][daemon-control]") {
    const TempLockFile lock;
    lock.write(valid_record());
    std::array<int, 2> events{-1, -1};
    std::array<int, 2> resumes{-1, -1};
    REQUIRE(::pipe(events.data()) == 0);
    REQUIRE(::pipe(resumes.data()) == 0);
    const pid_t publisher = ::fork();
    REQUIRE(publisher >= 0);
    if (publisher == 0) {
        ::close(events[0]);
        ::close(resumes[1]);
        AcquireBarrierContext context{events[1], resumes[0]};
        const daemon_lock::detail::AcquireHooks hooks{acquire_barrier, &context};
        daemon_lock::Identity identity;
        std::string error;
        static_cast<void>(daemon_lock::acquire(lock.filename(), identity, error, &hooks));
        ::_exit(94);
    }
    ::close(events[1]);
    ::close(resumes[0]);

    for (const auto stage : {daemon_lock::detail::AcquireStage::BootstrapLocked,
                             daemon_lock::detail::AcquireStage::OwnerLocked,
                             daemon_lock::detail::AcquireStage::RecordTruncated}) {
        REQUIRE(read_byte(events[0]) == static_cast<char>('0' + static_cast<int>(stage)));
        if (stage != daemon_lock::detail::AcquireStage::RecordTruncated) {
            write_byte(resumes[1], '1');
        }
    }
    std::optional<daemon_lock::OwnerWatch> owner;
    std::string error;
    CHECK(daemon_lock::inspect_owner(lock.filename(), getuid(), owner, error) ==
          daemon_lock::OwnerStatus::Transition);
    REQUIRE(::kill(publisher, SIGKILL) == 0);
    int status = 0;
    REQUIRE(::waitpid(publisher, &status, 0) == publisher);
    REQUIRE(WIFSIGNALED(status));
    owner.reset();
    CHECK(daemon_lock::inspect_owner(lock.filename(), getuid(), owner, error) ==
          daemon_lock::OwnerStatus::Invalid);
    CHECK(error.find("invalid size") != std::string::npos);
    CHECK(lock.read().empty());
    ::close(events[0]);
    ::close(resumes[1]);
}
