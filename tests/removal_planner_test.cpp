#include "daemon/removal_planner.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::daemon;

namespace {

class TempPlanningTree final {
  public:
    explicit TempPlanningTree(const std::string& config) {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-removal-plan-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        environment_.home = root_.string();
        environment_.xdg_config_home = (root_ / "config").string();
        environment_.xdg_data_home = (root_ / "data").string();
        environment_.xdg_state_home = (root_ / "state").string();
        environment_.uid = ::getuid();
        std::filesystem::create_directories(root_ / "config" / "tgcli");
        REQUIRE(::chmod((root_ / "config" / "tgcli").c_str(), 0700) == 0);
        std::ofstream output(config_path(), std::ios::binary);
        REQUIRE(output.good());
        output << config;
        output.close();
        REQUIRE(::chmod(config_path().c_str(), 0600) == 0);
    }

    ~TempPlanningTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempPlanningTree(const TempPlanningTree&) = delete;
    TempPlanningTree& operator=(const TempPlanningTree&) = delete;
    TempPlanningTree(TempPlanningTree&&) = delete;
    TempPlanningTree& operator=(TempPlanningTree&&) = delete;

    [[nodiscard]] const tgcli::paths::Environment& environment() const {
        return environment_;
    }

    [[nodiscard]] std::filesystem::path config_path() const {
        return root_ / "config" / "tgcli" / "config.toml";
    }

    [[nodiscard]] std::filesystem::path account_data() const {
        return root_ / "data" / "tgcli" / "accounts" / "work";
    }

    void create_account_data() const {
        std::filesystem::create_directories(account_data());
        REQUIRE(::chmod((root_ / "data" / "tgcli").c_str(), 0700) == 0);
        REQUIRE(::chmod((root_ / "data" / "tgcli" / "accounts").c_str(), 0700) == 0);
        REQUIRE(::chmod(account_data().c_str(), 0700) == 0);
    }

    [[nodiscard]] tgcli::config::Store store() const {
        return tgcli::config::Store(config_path().string(), environment_.uid);
    }

    [[nodiscard]] RemovalJournal journal() const {
        return {tgcli::paths::removals_state_dir(environment_), environment_.uid};
    }

  private:
    std::filesystem::path root_;
    tgcli::paths::Environment environment_;
};

std::string two_accounts(std::string_view default_account = "main") {
    return "default_account = \"" + std::string(default_account) +
           "\"\n[accounts.main]\nallow_write = true\n"
           "[accounts.work]\nallow_write = false\n";
}

} // namespace

TEST_CASE("removal planner resolves non-default roots snapshot and resulting default",
          "[removal][planner]") {
    const TempPlanningTree temp(two_accounts());
    temp.create_account_data();
    const auto result = plan_account_removal(temp.store(), temp.journal(), temp.environment(),
                                             "work", false, std::nullopt);
    REQUIRE(result);
    const auto& planned = *result.planned;
    CHECK(planned.plan.account() == "work");
    CHECK(planned.plan.remote_logout());
    CHECK(planned.plan.reassign_default() == "main");
    REQUIRE(planned.plan.data_root());
    CHECK_FALSE(planned.plan.state_root());
    CHECK(planned.plan.config_snapshot() == planned.config->identity);
    REQUIRE(planned.account_config);
    CHECK_FALSE(planned.account_config->allow_write);
}

TEST_CASE("default removal requires an exact explicit reassignment", "[removal][planner]") {
    const TempPlanningTree temp(two_accounts("work"));
    const auto store = temp.store();
    const auto journal = temp.journal();

    auto result =
        plan_account_removal(store, journal, temp.environment(), "work", false, std::nullopt);
    REQUIRE(result.error);
    CHECK(result.error->code == "DEFAULT_REASSIGNMENT_REQUIRED");
    CHECK(result.error->details ==
          nlohmann::json{{"account", "work"}, {"candidates", nlohmann::json::array({"main"})}});

    result = plan_account_removal(store, journal, temp.environment(), "work", false, "work");
    REQUIRE(result.error);
    CHECK(result.error->code == "USAGE");
    result = plan_account_removal(store, journal, temp.environment(), "work", false, "missing");
    REQUIRE(result.error);
    CHECK(result.error->code == "ACCOUNT_NOT_FOUND");
    result = plan_account_removal(store, journal, temp.environment(), "work", false, "main");
    REQUIRE(result);
    CHECK(result.planned->plan.reassign_default() == "main");
}

TEST_CASE("sole and non-default removals reject meaningless reassignment flags",
          "[removal][planner]") {
    const TempPlanningTree sole(
        "default_account = \"work\"\n[accounts.work]\nallow_write = false\n");
    auto result = plan_account_removal(sole.store(), sole.journal(), sole.environment(), "work",
                                       true, std::nullopt);
    REQUIRE(result);
    CHECK_FALSE(result.planned->plan.reassign_default());
    result = plan_account_removal(sole.store(), sole.journal(), sole.environment(), "work", true,
                                  "main");
    REQUIRE(result.error);
    CHECK(result.error->code == "USAGE");

    const TempPlanningTree non_default(two_accounts());
    result = plan_account_removal(non_default.store(), non_default.journal(),
                                  non_default.environment(), "work", false, "main");
    REQUIRE(result.error);
    CHECK(result.error->code == "USAGE");
}

TEST_CASE("removal planner rejects missing accounts and unsafe captured roots",
          "[removal][planner]") {
    const TempPlanningTree temp(two_accounts());
    auto result = plan_account_removal(temp.store(), temp.journal(), temp.environment(), "absent",
                                       false, std::nullopt);
    REQUIRE(result.error);
    CHECK(result.error->code == "ACCOUNT_NOT_FOUND");

    temp.create_account_data();
    REQUIRE(::chmod((temp.account_data().parent_path()).c_str(), 0755) == 0);
    result = plan_account_removal(temp.store(), temp.journal(), temp.environment(), "work", false,
                                  std::nullopt);
    REQUIRE(result.error);
    CHECK(result.error->code == "LOCAL_CLEANUP_FAILED");
    CHECK(result.error->details["reason"] == "path_invalid");
    CHECK(result.error->details["removed"].empty());
    CHECK(result.error->details["retained"].size() == 2);
}

TEST_CASE("repeated removal resolves its tombstone before the missing config account",
          "[removal][planner][recovery]") {
    const TempPlanningTree temp(two_accounts());
    const auto store = temp.store();
    const auto journal = temp.journal();
    auto fresh =
        plan_account_removal(store, journal, temp.environment(), "work", true, std::nullopt);
    REQUIRE(fresh);
    RemovalJournalFailure failure;
    REQUIRE(journal.create("00112233445566778899aabbccddeeff", fresh.planned->plan, failure));

    const auto removed =
        store.remove_account(fresh.planned->config->identity, "work", std::nullopt);
    REQUIRE(removed.status == tgcli::config::MutationStatus::Applied);
    auto resumed =
        plan_account_removal(store, journal, temp.environment(), "work", true, std::nullopt);
    REQUIRE(resumed);
    REQUIRE(resumed.planned->recovery);
    CHECK(resumed.planned->recovery->invocation_id == "00112233445566778899aabbccddeeff");
    CHECK_FALSE(resumed.planned->account_config);

    resumed = plan_account_removal(store, journal, temp.environment(), "work", false, std::nullopt);
    REQUIRE(resumed.error);
    CHECK(resumed.error->code == "REMOVAL_INCOMPLETE");
    CHECK(resumed.error->details["reason"] == "identity_ambiguous");
}
