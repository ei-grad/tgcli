#include "common/config.hpp"
#include "common/config_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using namespace tgcli::config;

namespace {

class TempConfig {
  public:
    explicit TempConfig(bool create_config_directory = true) {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-config-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        config_parent_ = root_ / "xdg";
        std::filesystem::create_directory(config_parent_);
        REQUIRE(::chmod(config_parent_.c_str(), 0700) == 0);
        config_dir_ = config_parent_ / "tgcli";
        if (create_config_directory) {
            std::filesystem::create_directory(config_dir_);
            REQUIRE(::chmod(config_dir_.c_str(), 0700) == 0);
        }
    }

    ~TempConfig() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    TempConfig(const TempConfig&) = delete;
    TempConfig& operator=(const TempConfig&) = delete;
    TempConfig(TempConfig&&) = delete;
    TempConfig& operator=(TempConfig&&) = delete;

    [[nodiscard]] std::string file() const {
        return (config_dir_ / "config.toml").string();
    }

    [[nodiscard]] const std::filesystem::path& dir() const {
        return config_dir_;
    }

    [[nodiscard]] const std::filesystem::path& root() const {
        return root_;
    }

    [[nodiscard]] const std::filesystem::path& parent() const {
        return config_parent_;
    }

    void write(std::string_view bytes, mode_t mode = 0600) const {
        std::ofstream output(file(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        REQUIRE(::chmod(file().c_str(), mode) == 0);
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path config_parent_;
    std::filesystem::path config_dir_;
};

std::string valid_config(std::string_view extra = {}) {
    return "default_account = \"main\"\n"
           "title = \"preserve me\"\n"
           "[extension]\n"
           "enabled = true\n"
           "[accounts.main]\n"
           "api_id = 12345\n"
           "api_hash = \"hash\"\n"
           "allow_write = true\n" +
           std::string(extra);
}

} // namespace

TEST_CASE("config load distinguishes missing valid and invalid TOML", "[config]") {
    const TempConfig temp;
    const Store store(temp.file());

    auto loaded = store.load();
    REQUIRE(loaded);
    CHECK(loaded.snapshot->identity == "missing");
    CHECK(loaded.snapshot->accounts.empty());

    temp.write(valid_config());
    loaded = store.load();
    REQUIRE(loaded);
    CHECK(loaded.snapshot->default_account == "main");
    REQUIRE(loaded.snapshot->accounts.contains("main"));
    const auto& main = loaded.snapshot->accounts.at("main");
    CHECK(main.api_id == 12345);
    CHECK(main.api_hash == "hash");
    CHECK(main.allow_write);
    CHECK(loaded.snapshot->identity.starts_with("sha256:"));
    CHECK(loaded.snapshot->identity.find(";dev:") != std::string::npos);

    temp.write("");
    loaded = store.load();
    REQUIRE(loaded);
    CHECK(loaded.snapshot->identity.starts_with(
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855;"));

    temp.write("[accounts.main\n");
    loaded = store.load();
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error->reason == ConfigReason::ParseError);

    temp.write("[accounts.main]\nallow_write = \"yes\"\n");
    loaded = store.load();
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error->reason == ConfigReason::TypeError);
}

TEST_CASE("known config fields are strict and secret plain values are rejected", "[config]") {
    const TempConfig temp;
    const Store store(temp.file());

    const std::vector<std::string> invalid = {
        "[accounts.main]\napi_id = 0\n",
        "[accounts.main]\napi_id = 1\napi_id_cmd = \"get-id\"\n",
        "[accounts.main]\napi_hash = \"x\"\napi_hash_cmd = \"get-hash\"\n",
        "[accounts.main]\nidle_exit = 0\n",
        "[accounts.main]\nbot_token = \"secret\"\n",
        "default_account = \"missing\"\n[accounts.main]\nallow_write = false\n",
        "[accounts.\"bad.name\"]\nallow_write = false\n",
    };
    for (const auto& contents : invalid) {
        temp.write(contents);
        INFO(contents);
        REQUIRE_FALSE(store.load());
    }

    temp.write("[accounts.main]\npassword_cmd = \"\"\ndb_key_cmd = \"\"\n");
    const auto loaded = store.load();
    REQUIRE(loaded);
    CHECK_FALSE(loaded.snapshot->accounts.at("main").password_cmd);
    CHECK_FALSE(loaded.snapshot->accounts.at("main").db_key_cmd);
}

TEST_CASE("config read rejects modes symlinks hardlinks and oversized input", "[config]") {
    const TempConfig temp;
    const Store store(temp.file());
    temp.write("[accounts.main]\nallow_write = false\n");

    SECTION("mode") {
        REQUIRE(::chmod(temp.file().c_str(), 0644) == 0);
        const auto loaded = store.load();
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error->reason == ConfigReason::WrongMode);
    }
    SECTION("symlink") {
        const auto target = temp.dir() / "target";
        REQUIRE(::rename(temp.file().c_str(), target.c_str()) == 0);
        REQUIRE(::symlink(target.c_str(), temp.file().c_str()) == 0);
        const auto loaded = store.load();
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error->reason == ConfigReason::WrongType);
    }
    SECTION("hardlink") {
        REQUIRE(::link(temp.file().c_str(), (temp.dir() / "second-link").c_str()) == 0);
        const auto loaded = store.load();
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error->reason == ConfigReason::WrongLinkCount);
    }
    SECTION("oversized") {
        temp.write(std::string(kMaxConfigBytes + 1, 'x'));
        const auto loaded = store.load();
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error->reason == ConfigReason::TooLarge);
    }
    SECTION("owner") {
        const Store foreign_owner(temp.file(), getuid() + 1);
        const auto loaded = foreign_owner.load();
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error->reason == ConfigReason::WrongOwner);
    }
    SECTION("directory mode") {
        REQUIRE(::chmod(temp.dir().c_str(), 0755) == 0);
        const auto loaded = store.load();
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error->reason == ConfigReason::WrongMode);
    }
}

TEST_CASE("config mutation rejects unsafe global lock and replaced config paths", "[config]") {
    const TempConfig temp;
    temp.write("[accounts.main]\nallow_write = false\n");
    const Store store(temp.file());
    const auto planned = store.load();
    REQUIRE(planned);

    SECTION("symlink lock") {
        const auto target = temp.dir() / "lock-target";
        { std::ofstream(target) << ""; }
        REQUIRE(::chmod(target.c_str(), 0600) == 0);
        REQUIRE(::symlink(target.c_str(), (temp.dir() / "config.lock").c_str()) == 0);
        const auto result = store.add_account(planned.snapshot->identity, "work");
        CHECK(result.status == MutationStatus::IoError);
        REQUIRE(result.error);
        CHECK((result.error->reason == ConfigReason::WrongType ||
               result.error->reason == ConfigReason::PathInvalid));
    }
    SECTION("hard-linked lock") {
        const auto lock = temp.dir() / "config.lock";
        { std::ofstream(lock) << ""; }
        REQUIRE(::chmod(lock.c_str(), 0600) == 0);
        REQUIRE(::link(lock.c_str(), (temp.dir() / "lock-link").c_str()) == 0);
        const auto result = store.add_account(planned.snapshot->identity, "work");
        CHECK(result.status == MutationStatus::IoError);
        REQUIRE(result.error);
        CHECK(result.error->reason == ConfigReason::WrongLinkCount);
    }
    SECTION("config path replaced with symlink") {
        const auto target = temp.dir() / "config-target";
        REQUIRE(::rename(temp.file().c_str(), target.c_str()) == 0);
        REQUIRE(::symlink(target.c_str(), temp.file().c_str()) == 0);
        const auto result = store.add_account(planned.snapshot->identity, "work");
        CHECK(result.status == MutationStatus::Invalid);
        REQUIRE(result.error);
        CHECK(result.error->reason == ConfigReason::WrongType);
    }
}

TEST_CASE("config mutation rejects canonical directory changes after locking", "[config]") {
    const TempConfig temp;
    const std::string original = "[accounts.main]\nallow_write = false\n";
    temp.write(original);
    const Store reader(temp.file());
    const auto planned = reader.load();
    REQUIRE(planned);

    SECTION("config directory inode is replaced") {
        const auto detached = temp.root() / "detached-tgcli";
        auto hooks = std::make_shared<testing::StoreHooks>();
        hooks->at_stage = [&](testing::MutationStage stage) {
            if (stage != testing::MutationStage::AfterLock) {
                return;
            }
            REQUIRE(::rename(temp.dir().c_str(), detached.c_str()) == 0);
            REQUIRE(std::filesystem::create_directory(temp.dir()));
            REQUIRE(::chmod(temp.dir().c_str(), 0700) == 0);
            temp.write(original);
        };
        const Store store(temp.file(), hooks);
        const auto result = store.add_account(planned.snapshot->identity, "work");
        CHECK(result.status == MutationStatus::IoError);
        REQUIRE(result.error);
        CHECK(result.error->reason == ConfigReason::PathInvalid);
        CHECK(std::ifstream(temp.file()).good());
        CHECK(std::ifstream(detached / "config.toml").good());
        CHECK(reader.load().snapshot->raw_bytes == original);
        CHECK(Store((detached / "config.toml").string()).load().snapshot->raw_bytes == original);
    }

    SECTION("config directory mode changes") {
        auto hooks = std::make_shared<testing::StoreHooks>();
        hooks->at_stage = [&](testing::MutationStage stage) {
            if (stage == testing::MutationStage::AfterLock) {
                REQUIRE(::chmod(temp.dir().c_str(), 0755) == 0);
            }
        };
        const Store store(temp.file(), hooks);
        const auto result = store.add_account(planned.snapshot->identity, "work");
        CHECK(result.status == MutationStatus::IoError);
        REQUIRE(result.error);
        CHECK(result.error->reason == ConfigReason::PathInvalid);
        REQUIRE(::chmod(temp.dir().c_str(), 0700) == 0);
        CHECK(reader.load().snapshot->raw_bytes == original);
    }
}

TEST_CASE("config mutation revalidates the canonical directory immediately before commit",
          "[config]") {
    const TempConfig temp;
    const std::string original = "[accounts.main]\nallow_write = false\n";
    temp.write(original);
    const Store reader(temp.file());
    const auto planned = reader.load();
    REQUIRE(planned);
    const auto detached = temp.root() / "detached-before-commit";
    auto hooks = std::make_shared<testing::StoreHooks>();
    hooks->at_stage = [&](testing::MutationStage stage) {
        if (stage != testing::MutationStage::BeforeCommit) {
            return;
        }
        REQUIRE(::rename(temp.dir().c_str(), detached.c_str()) == 0);
        REQUIRE(std::filesystem::create_directory(temp.dir()));
        REQUIRE(::chmod(temp.dir().c_str(), 0700) == 0);
        temp.write(original);
    };
    const Store store(temp.file(), hooks);
    const auto result = store.add_account(planned.snapshot->identity, "work");
    CHECK(result.status == MutationStatus::IoError);
    REQUIRE(result.error);
    CHECK(result.error->reason == ConfigReason::PathInvalid);
    CHECK(reader.load().snapshot->raw_bytes == original);
    CHECK(Store((detached / "config.toml").string()).load().snapshot->raw_bytes == original);
}

TEST_CASE("config mutation rejects canonical parent and transaction-entry replacement races",
          "[config]") {
    const auto run_race = [](const auto& race) {
        const TempConfig temp;
        const std::string original = "[accounts.main]\nallow_write = false\n";
        temp.write(original);
        const Store reader(temp.file());
        const auto before = reader.load();
        REQUIRE(before);
        auto hooks = std::make_shared<testing::StoreHooks>();
        hooks->at_stage = [&](testing::MutationStage stage) {
            if (stage == testing::MutationStage::BeforeCommit) {
                race(temp);
            }
        };
        const Store store(temp.file(), hooks);
        const auto result = store.add_account(before.snapshot->identity, "work");
        CHECK(result.status == MutationStatus::IoError);
        REQUIRE(result.error);
        CHECK(result.error->reason == ConfigReason::PathInvalid);
    };

    SECTION("canonical parent inode changes as with a mount-over") {
        run_race([](const TempConfig& temp) {
            const auto detached = temp.root() / "detached-parent";
            REQUIRE(::rename(temp.parent().c_str(), detached.c_str()) == 0);
            REQUIRE(std::filesystem::create_directory(temp.parent()));
            REQUIRE(::chmod(temp.parent().c_str(), 0700) == 0);
            REQUIRE(std::filesystem::create_directory(temp.dir()));
            REQUIRE(::chmod(temp.dir().c_str(), 0700) == 0);
            temp.write("[accounts.main]\nallow_write = false\n");
        });
    }
    SECTION("lock entry changes") {
        run_race([](const TempConfig& temp) {
            const auto lock = temp.dir() / "config.lock";
            REQUIRE(::rename(lock.c_str(), (temp.dir() / "detached-lock").c_str()) == 0);
            { std::ofstream(lock) << ""; }
            REQUIRE(::chmod(lock.c_str(), 0600) == 0);
        });
    }
    SECTION("replacement entry changes") {
        run_race([](const TempConfig& temp) {
            for (const auto& entry : std::filesystem::directory_iterator(temp.dir())) {
                const auto name = entry.path().filename().string();
                if (!name.starts_with(".config.toml.tmp.")) {
                    continue;
                }
                REQUIRE(::rename(entry.path().c_str(),
                                 (temp.dir() / "detached-replacement").c_str()) == 0);
                { std::ofstream(entry.path()) << "[accounts.attacker]\nallow_write = true\n"; }
                REQUIRE(::chmod(entry.path().c_str(), 0600) == 0);
                return;
            }
            FAIL("replacement entry was not present at the pre-commit stage");
        });
    }
}

TEST_CASE("prepared replacement identity remains bound through atomic exchange", "[config]") {
    const auto run_race = [](testing::MutationStage target_stage) {
        const TempConfig temp;
        const std::string original = "[accounts.main]\nallow_write = false\n";
        temp.write(original);
        const Store reader(temp.file());
        const auto before = reader.load();
        REQUIRE(before);

        auto hooks = std::make_shared<testing::StoreHooks>();
        hooks->at_stage = [&](testing::MutationStage stage) {
            if (stage != target_stage) {
                return;
            }
            REQUIRE(std::filesystem::exists(temp.dir() / ".config.toml.transaction"));
            const auto canonical = stage == testing::MutationStage::AfterPrepare
                                       ? temp.dir() / ".config.toml.replacement"
                                       : temp.dir() / "config.toml";
            const auto detached = stage == testing::MutationStage::AfterPrepare
                                      ? temp.dir() / "detached-prepared"
                                      : temp.dir() / "detached-exchanged";
            REQUIRE(::rename(canonical.c_str(), detached.c_str()) == 0);
            { std::ofstream(canonical) << "[accounts.attacker]\nallow_write = true\n"; }
            REQUIRE(::chmod(canonical.c_str(), 0600) == 0);
        };

        const Store store(temp.file(), hooks);
        const auto result = store.add_account(before.snapshot->identity, "work");
        CHECK(result.status == MutationStatus::IoError);
        REQUIRE(result.error);
        CHECK(result.error->reason == ConfigReason::PathInvalid);
        const auto restored = reader.load();
        REQUIRE(restored);
        CHECK(restored.snapshot->raw_bytes == original);
        if (target_stage == testing::MutationStage::AfterPrepare) {
            CHECK(restored.snapshot->identity == before.snapshot->identity);
        }
        CHECK_FALSE(restored.snapshot->accounts.contains("attacker"));
        CHECK_FALSE(restored.snapshot->accounts.contains("work"));
        CHECK_FALSE(std::filesystem::exists(temp.dir() / ".config.toml.transaction"));
        CHECK_FALSE(std::filesystem::exists(temp.dir() / ".config.toml.replacement"));
    };

    SECTION("substitution after the transaction becomes durable") {
        run_race(testing::MutationStage::AfterPrepare);
    }
    SECTION("substitution immediately after the atomic exchange") {
        run_race(testing::MutationStage::AfterExchange);
    }
}

TEST_CASE("config replacement faults preserve the reported filesystem state", "[config]") {
    const auto run_fault = [](testing::MutationFault fault, MutationStatus expected_status) {
        const TempConfig temp;
        const std::string original = "[accounts.main]\nallow_write = false\n";
        temp.write(original);
        const Store reader(temp.file());
        const auto before = reader.load();
        REQUIRE(before);
        auto hooks = std::make_shared<testing::StoreHooks>();
        hooks->should_fail = [fault](testing::MutationFault candidate) {
            return candidate == fault;
        };
        const Store store(temp.file(), hooks);
        const auto result = store.add_account(before.snapshot->identity, "work");
        CHECK(result.status == expected_status);

        if (expected_status == MutationStatus::IoError) {
            const auto after = reader.load();
            REQUIRE(after);
            CHECK(after.snapshot->identity == before.snapshot->identity);
            CHECK(after.snapshot->raw_bytes == original);
            for (const auto& entry : std::filesystem::directory_iterator(temp.dir())) {
                CHECK_FALSE(entry.path().filename().string().starts_with(".config.toml."));
            }
        } else {
            const auto ambiguous = reader.load();
            REQUIRE_FALSE(ambiguous);
            CHECK(ambiguous.error->reason == ConfigReason::SyncError);
        }
    };

    SECTION("replacement file fsync") {
        run_fault(testing::MutationFault::TemporaryFileSync, MutationStatus::IoError);
    }
    SECTION("atomic replacement rename") {
        run_fault(testing::MutationFault::ReplacementRename, MutationStatus::IoError);
    }
    SECTION("commit directory fsync rolls back") {
        run_fault(testing::MutationFault::CommitDirectorySync, MutationStatus::DurabilityUnknown);
    }
}

TEST_CASE("uncertain rollback fails closed and is recovered under the config lock", "[config]") {
    for (const auto fault :
         {testing::MutationFault::RollbackRename, testing::MutationFault::RollbackDirectorySync}) {
        const TempConfig temp;
        temp.write("[accounts.main]\nallow_write = false\n");
        const Store reader(temp.file());
        const auto before = reader.load();
        REQUIRE(before);
        auto hooks = std::make_shared<testing::StoreHooks>();
        hooks->should_fail = [fault](testing::MutationFault candidate) {
            return candidate == testing::MutationFault::CommitDirectorySync || candidate == fault;
        };
        const Store faulting_store(temp.file(), hooks);
        const auto failed = faulting_store.add_account(before.snapshot->identity, "work");
        CHECK(failed.status == MutationStatus::DurabilityUnknown);

        const auto ambiguous = reader.load();
        REQUIRE_FALSE(ambiguous);
        CHECK(ambiguous.error->reason == ConfigReason::SyncError);

        const auto recovery = reader.add_account(before.snapshot->identity, "recovered");
        REQUIRE(recovery.status == MutationStatus::Conflict);
        CHECK_FALSE(recovery.snapshot->accounts.contains("work"));
        const auto recovered = reader.add_account(recovery.snapshot->identity, "recovered");
        REQUIRE(recovered.status == MutationStatus::Applied);
        CHECK(recovered.snapshot->accounts.contains("recovered"));
    }
}

TEST_CASE("first config-directory creation synchronizes its canonical parent", "[config]") {
    const TempConfig temp(false);
    const Store reader(temp.file());
    const auto missing = reader.load();
    REQUIRE(missing);
    REQUIRE(missing.snapshot->identity == "missing");

    auto hooks = std::make_shared<testing::StoreHooks>();
    hooks->should_fail = [](testing::MutationFault fault) {
        return fault == testing::MutationFault::ParentDirectorySync;
    };
    const Store store(temp.file(), hooks);
    const auto result = store.add_account(missing.snapshot->identity, "main");
    CHECK(result.status == MutationStatus::IoError);
    CHECK_FALSE(std::filesystem::exists(temp.file()));
}

TEST_CASE("first-run rollback recovery recognizes a restored missing config", "[config]") {
    const TempConfig temp(false);
    const Store reader(temp.file());
    const auto missing = reader.load();
    REQUIRE(missing);

    auto hooks = std::make_shared<testing::StoreHooks>();
    hooks->should_fail = [](testing::MutationFault fault) {
        return fault == testing::MutationFault::CommitDirectorySync ||
               fault == testing::MutationFault::RollbackDirectorySync;
    };
    const Store faulting_store(temp.file(), hooks);
    const auto failed = faulting_store.add_account(missing.snapshot->identity, "work");
    REQUIRE(failed.status == MutationStatus::DurabilityUnknown);
    REQUIRE_FALSE(reader.load());

    const auto recovered = reader.add_account(missing.snapshot->identity, "main");
    REQUIRE(recovered.status == MutationStatus::Applied);
    CHECK(recovered.snapshot->accounts.contains("main"));
    CHECK_FALSE(recovered.snapshot->accounts.contains("work"));
}

TEST_CASE("config mutation cannot grow the file past one MiB", "[config]") {
    const TempConfig temp;
    const std::string prefix = "default_account = \"main\"\nnote = \"";
    const std::string suffix = "\"\n[accounts.main]\nallow_write = false\n";
    REQUIRE(prefix.size() + suffix.size() < kMaxConfigBytes);
    temp.write(prefix + std::string(kMaxConfigBytes - prefix.size() - suffix.size(), 'x') + suffix);
    const Store store(temp.file());
    const auto before = store.load();
    REQUIRE(before);

    const auto result = store.add_account(before.snapshot->identity, "work");
    CHECK(result.status == MutationStatus::Invalid);
    REQUIRE(result.error);
    CHECK(result.error->reason == ConfigReason::TooLarge);
    const auto after = store.load();
    REQUIRE(after);
    CHECK(after.snapshot->identity == before.snapshot->identity);
}

TEST_CASE("CAS mutations replace atomically and preserve unrelated TOML", "[config]") {
    const TempConfig temp;
    temp.write(valid_config());
    const Store store(temp.file());
    const auto before = store.load();
    REQUIRE(before);

    struct stat old_stat {};
    REQUIRE(::lstat(temp.file().c_str(), &old_stat) == 0);
    const auto added = store.add_account(before.snapshot->identity, "work");
    REQUIRE(added.status == MutationStatus::Applied);
    struct stat new_stat {};
    REQUIRE(::lstat(temp.file().c_str(), &new_stat) == 0);
    CHECK(new_stat.st_ino != old_stat.st_ino);
    CHECK((new_stat.st_mode & 07777) == 0600);
    CHECK(new_stat.st_nlink == 1);
    for (const auto& entry : std::filesystem::directory_iterator(temp.dir())) {
        CHECK_FALSE(entry.path().filename().string().starts_with(".config.toml.tmp."));
    }

    const auto after = store.load();
    REQUIRE(after);
    CHECK(after.snapshot->accounts.contains("work"));
    CHECK(after.snapshot->raw_bytes.find("preserve me") != std::string::npos);
    CHECK(after.snapshot->raw_bytes.find("[extension]") != std::string::npos);
    CHECK(after.snapshot->raw_bytes.find("enabled = true") != std::string::npos);

    const auto stale = store.use_account(before.snapshot->identity, "work");
    CHECK(stale.status == MutationStatus::Conflict);
    CHECK(store.load().snapshot->default_account == "main");

    const auto used = store.use_account(after.snapshot->identity, "work");
    REQUIRE(used.status == MutationStatus::Applied);
    const auto removed = store.remove_account(used.snapshot->identity, "main", std::nullopt);
    REQUIRE(removed.status == MutationStatus::Applied);
    CHECK_FALSE(removed.snapshot->accounts.contains("main"));
    CHECK(removed.snapshot->default_account == "work");
}

TEST_CASE("config lock serializes cross-process snapshot CAS", "[config][process]") {
    const TempConfig temp;
    temp.write("[accounts.main]\nallow_write = false\n");
    const Store store(temp.file());
    const auto initial = store.load();
    REQUIRE(initial);

    std::vector<pid_t> children;
    children.reserve(2);
    for (const std::string name : {"one", "two"}) {
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            const Store child_store(temp.file());
            const auto result = child_store.add_account(initial.snapshot->identity, name);
            ::_exit(20 + static_cast<int>(result.status));
        }
        children.push_back(child);
    }

    std::vector<int> statuses;
    for (const pid_t child : children) {
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        statuses.push_back(WEXITSTATUS(status) - 20);
    }
    std::ranges::sort(statuses);
    CHECK(statuses == std::vector{static_cast<int>(MutationStatus::Applied),
                                  static_cast<int>(MutationStatus::Conflict)});

    const auto current = store.load();
    REQUIRE(current);
    CHECK(current.snapshot->accounts.size() == 2);
}

TEST_CASE("cross-process use and remove plans cannot commit from one stale identity",
          "[config][process]") {
    const TempConfig temp;
    temp.write("default_account = \"main\"\n[accounts.main]\nallow_write = false\n"
               "[accounts.work]\nallow_write = false\n");
    const Store store(temp.file());
    const auto initial = store.load();
    REQUIRE(initial);

    std::vector<pid_t> children;
    children.reserve(2);
    for (int operation = 0; operation < 2; ++operation) {
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            const Store child_store(temp.file());
            const auto result =
                operation == 0
                    ? child_store.use_account(initial.snapshot->identity, "work")
                    : child_store.remove_account(initial.snapshot->identity, "work", std::nullopt);
            ::_exit(20 + static_cast<int>(result.status));
        }
        children.push_back(child);
    }

    std::vector<int> statuses;
    for (const pid_t child : children) {
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        statuses.push_back(WEXITSTATUS(status) - 20);
    }
    std::ranges::sort(statuses);
    CHECK(statuses == std::vector{static_cast<int>(MutationStatus::Applied),
                                  static_cast<int>(MutationStatus::Conflict)});

    const auto current = store.load();
    REQUIRE(current);
    if (current.snapshot->accounts.contains("work")) {
        CHECK(current.snapshot->default_account == "work");
    } else {
        CHECK(current.snapshot->default_account == "main");
    }
}

TEST_CASE("config mutation lock acquisition obeys deadline and cancellation", "[config]") {
    const TempConfig temp;
    temp.write("[accounts.main]\nallow_write = false\n");
    const Store store(temp.file());
    const auto initial = store.load();
    REQUIRE(initial);

    const auto lock_path = temp.dir() / "config.lock";
    const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    REQUIRE(lock_fd >= 0);
    REQUIRE(::flock(lock_fd, LOCK_EX | LOCK_NB) == 0);

    SECTION("deadline") {
        MutationControl control;
        control.deadline = std::chrono::steady_clock::now() + 50ms;
        const auto result = store.add_account(initial.snapshot->identity, "work", control);
        CHECK(result.status == MutationStatus::TimedOut);
    }
    SECTION("cancellation") {
        const std::stop_source source;
        source.request_stop();
        MutationControl control;
        control.cancellation = source.get_token();
        const auto result = store.add_account(initial.snapshot->identity, "work", control);
        CHECK(result.status == MutationStatus::Cancelled);
    }

    REQUIRE(::flock(lock_fd, LOCK_UN) == 0);
    REQUIRE(::close(lock_fd) == 0);
    const auto after = store.load();
    REQUIRE(after);
    CHECK(after.snapshot->identity == initial.snapshot->identity);
    for (const auto& entry : std::filesystem::directory_iterator(temp.dir())) {
        CHECK_FALSE(entry.path().filename().string().starts_with(".config.toml.tmp."));
    }
}

TEST_CASE("remove rejects default reassignment outside a replaceable default", "[config]") {
    const TempConfig temp;
    const Store store(temp.file());

    const auto rejects_without_change = [&](std::string_view contents, std::string_view target,
                                            std::string_view replacement) {
        temp.write(contents);
        const auto before = store.load();
        REQUIRE(before);
        const auto result = store.remove_account(before.snapshot->identity, target, replacement);
        CHECK(result.status == MutationStatus::Invalid);
        const auto after = store.load();
        REQUIRE(after);
        CHECK(after.snapshot->identity == before.snapshot->identity);
    };

    SECTION("target is not the current default") {
        rejects_without_change("default_account = \"main\"\n"
                               "[accounts.main]\nallow_write = false\n"
                               "[accounts.work]\nallow_write = false\n",
                               "work", "main");
    }
    SECTION("configuration has no default") {
        rejects_without_change("[accounts.main]\nallow_write = false\n"
                               "[accounts.work]\nallow_write = false\n",
                               "main", "work");
    }
    SECTION("target is the sole account") {
        rejects_without_change("[accounts.main]\nallow_write = false\n", "main", "main");
    }
}

TEST_CASE("atomic config replacement never exposes a partial snapshot to concurrent readers",
          "[config][process]") {
    const TempConfig temp;
    temp.write("default_account = \"main\"\n[accounts.main]\nallow_write = false\n"
               "[accounts.work]\nallow_write = false\n");
    const Store store(temp.file());
    std::atomic<bool> stop = false;
    std::atomic<int> invalid = 0;
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto loaded = store.load();
                if (!loaded) {
                    invalid.store(10 + static_cast<int>(loaded.error->reason),
                                  std::memory_order_relaxed);
                    break;
                }
                if (!loaded.snapshot->default_account ||
                    (*loaded.snapshot->default_account != "main" &&
                     *loaded.snapshot->default_account != "work")) {
                    invalid.store(2, std::memory_order_relaxed);
                    break;
                }
                if (loaded.snapshot->accounts.size() != 2) {
                    invalid.store(3, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }
    auto current = store.load();
    REQUIRE(current);
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto changed =
            store.use_account(current.snapshot->identity, iteration % 2 == 0 ? "work" : "main");
        REQUIRE(changed.status == MutationStatus::Applied);
        current.snapshot = changed.snapshot;
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) {
        reader.join();
    }
    CHECK(invalid.load() == 0);
}

TEST_CASE("implicit main materialization is deterministic and CAS guarded", "[config]") {
    const TempConfig temp;
    const Store store(temp.file());
    const auto missing = store.load();
    REQUIRE(missing);

    PromptedAppCredentials prompted;
    prompted.api_id = 777;
    prompted.api_hash = "prompted-hash";
    const auto materialized = store.materialize_implicit_main(missing.snapshot->identity, prompted);
    REQUIRE(materialized.status == MutationStatus::Applied);
    REQUIRE(materialized.snapshot->accounts.contains("main"));
    CHECK(materialized.snapshot->default_account == "main");
    CHECK(materialized.snapshot->accounts.at("main").api_id == 777);
    CHECK(materialized.snapshot->accounts.at("main").api_hash == "prompted-hash");
    CHECK_FALSE(materialized.snapshot->accounts.at("main").allow_write);

    CHECK(store.materialize_implicit_main("missing", {}).status == MutationStatus::Conflict);
}

TEST_CASE("invalid reload retains last good snapshot and disables standing grants",
          "[config][snapshot]") {
    const TempConfig temp;
    temp.write(valid_config());
    const Store store(temp.file());
    SnapshotManager manager(store);
    REQUIRE(manager.initialize());

    auto admitted = manager.current();
    REQUIRE(admitted->snapshot);
    CHECK(admitted->snapshot->accounts.at("main").allow_write);
    CHECK(admitted->standing_write_grants_valid);

    temp.write("[accounts.main\n");
    CHECK(manager.reload() == ReloadStatus::Invalid);
    admitted = manager.current();
    REQUIRE(admitted->snapshot);
    CHECK(admitted->snapshot->accounts.at("main").allow_write);
    CHECK_FALSE(admitted->standing_write_grants_valid);
    REQUIRE(admitted->error);

    temp.write(valid_config("idle_exit = 5\n"));
    CHECK(manager.reload() == ReloadStatus::Published);
    admitted = manager.current();
    CHECK(admitted->standing_write_grants_valid);
    CHECK(admitted->snapshot->accounts.at("main").idle_exit == 5s);
}

TEST_CASE("hook sources are immutable per admission and reload only for later admissions",
          "[config][snapshot]") {
    const TempConfig temp;
    temp.write("default_account = \"main\"\n"
               "[accounts.main]\n"
               "api_id_cmd = \"id-old\"\n"
               "api_hash_cmd = \"hash-old\"\n"
               "db_key_cmd = \"db-old\"\n"
               "password_cmd = \"password-old\"\n"
               "bot_token_cmd = \"bot-old\"\n");
    const Store store(temp.file());
    SnapshotManager manager(store);
    REQUIRE(manager.initialize());
    const auto admitted = manager.current();

    temp.write("default_account = \"main\"\n"
               "[accounts.main]\n"
               "api_id_cmd = \"id-new\"\n"
               "api_hash_cmd = \"hash-new\"\n"
               "db_key_cmd = \"db-new\"\n"
               "password_cmd = \"password-new\"\n"
               "bot_token_cmd = \"bot-new\"\n");
    REQUIRE(manager.reload() == ReloadStatus::Published);
    const auto later = manager.current();

    const auto& old_account = admitted->snapshot->accounts.at("main");
    const auto& new_account = later->snapshot->accounts.at("main");
    CHECK(old_account.api_id_cmd == "id-old");
    CHECK(old_account.api_hash_cmd == "hash-old");
    CHECK(old_account.db_key_cmd == "db-old");
    CHECK(old_account.password_cmd == "password-old");
    CHECK(old_account.bot_token_cmd == "bot-old");
    CHECK(new_account.api_id_cmd == "id-new");
    CHECK(new_account.api_hash_cmd == "hash-new");
    CHECK(new_account.db_key_cmd == "db-new");
    CHECK(new_account.password_cmd == "password-new");
    CHECK(new_account.bot_token_cmd == "bot-new");
}

TEST_CASE("snapshot publication is complete under concurrent readers", "[config][snapshot]") {
    const TempConfig temp;
    temp.write(valid_config("idle_exit = 1\n"));
    const Store store(temp.file());
    SnapshotManager manager(store);
    REQUIRE(manager.initialize());

    std::atomic<bool> stopped = false;
    std::atomic<bool> inconsistent = false;
    std::vector<std::thread> readers;
    readers.reserve(8);
    for (int index = 0; index < 8; ++index) {
        readers.emplace_back([&] {
            while (!stopped.load(std::memory_order_relaxed)) {
                const auto current = manager.current();
                if (!current || !current->snapshot ||
                    !current->snapshot->accounts.contains("main")) {
                    inconsistent.store(true, std::memory_order_relaxed);
                    break;
                }
                const auto idle = current->snapshot->accounts.at("main").idle_exit;
                if (idle != 1s && idle != 2s) {
                    inconsistent.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }
    for (int iteration = 0; iteration < 100; ++iteration) {
        temp.write(valid_config(iteration % 2 == 0 ? "idle_exit = 2\n" : "idle_exit = 1\n"));
        REQUIRE(manager.reload() == ReloadStatus::Published);
    }
    stopped.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) {
        reader.join();
    }
    CHECK_FALSE(inconsistent.load());
}

TEST_CASE("one-second monotonic poll reloads while otherwise idle", "[config][snapshot]") {
    const TempConfig temp;
    temp.write(valid_config("idle_exit = 1\n"));
    const Store store(temp.file());
    SnapshotManager manager(store);
    const auto start = SnapshotManager::Clock::now();
    REQUIRE(manager.initialize(start));

    temp.write(valid_config("idle_exit = 9\n"));
    CHECK(manager.poll(start + 999ms) == ReloadStatus::NotDue);
    CHECK(manager.current()->snapshot->accounts.at("main").idle_exit == 1s);
    CHECK(manager.poll(start + 1s) == ReloadStatus::Published);
    CHECK(manager.current()->snapshot->accounts.at("main").idle_exit == 9s);
}
