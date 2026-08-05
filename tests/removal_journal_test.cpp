#include "daemon/removal_journal.hpp"
#include "schema_matcher.hpp"

#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli::daemon;

namespace {

class TempJournal final {
  public:
    TempJournal() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "tgcli-removal-journal-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root_ = created;
        directory_ = root_ / "state" / "tgcli" / "removals";
    }

    ~TempJournal() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempJournal(const TempJournal&) = delete;
    TempJournal& operator=(const TempJournal&) = delete;
    TempJournal(TempJournal&&) = delete;
    TempJournal& operator=(TempJournal&&) = delete;

    [[nodiscard]] std::string directory() const {
        return directory_.string();
    }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return directory_ / name;
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path directory_;
};

constexpr std::string_view kFirst = "00112233445566778899aabbccddeeff";
constexpr std::string_view kSecond = "ffeeddccbbaa99887766554433221100";
constexpr std::string_view kSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
    "ino:2;size:3;ctime_ns:4";
constexpr std::string_view kTimestamp = "2026-08-04T10:11:12Z";

tgcli::proto::AccountRemovePlan plan(bool keep = false, std::string account = "work") {
    std::string error;
    auto result = tgcli::proto::make_account_remove_plan(
        {.account = std::move(account),
         .keep_session = keep,
         .delete_paths = {"/data/tgcli/accounts/work", "/state/tgcli/accounts/work"},
         .config_path = "/config/tgcli/config.toml",
         .config_snapshot = std::string(kSnapshot),
         .data_root = std::nullopt,
         .state_root = std::nullopt,
         .reassign_default = "main"},
        error);
    INFO(error);
    REQUIRE(result);
    return *result;
}

std::vector<AuditStage> completed_not_present() {
    return {AuditStage::Planned,          AuditStage::IntentSynced,
            AuditStage::RemoteNotPresent, AuditStage::ClientCloseStarted,
            AuditStage::ClientClosed,     AuditStage::ConfigRemoveStarted,
            AuditStage::ConfigRemoved,    AuditStage::DataRemoveStarted,
            AuditStage::DataRemoved,      AuditStage::StateRemoveStarted,
            AuditStage::StateRemoved};
}

void advance_not_present(const RemovalJournal& journal, std::string_view invocation) {
    RemovalJournalFailure failure;
    for (const auto stage : std::vector{AuditStage::IntentSynced, AuditStage::RemoteNotPresent,
                                        AuditStage::ClientCloseStarted, AuditStage::ClientClosed,
                                        AuditStage::ConfigRemoveStarted, AuditStage::ConfigRemoved,
                                        AuditStage::DataRemoveStarted, AuditStage::DataRemoved,
                                        AuditStage::StateRemoveStarted, AuditStage::StateRemoved}) {
        INFO(audit_stage_name(stage));
        REQUIRE(journal.advance(std::string(invocation), stage, failure));
    }
}

AuditIntent intent(std::string invocation, const tgcli::proto::AccountRemovePlan& removal) {
    std::string error;
    auto record =
        make_account_remove_audit_intent({std::move(invocation), std::string(kTimestamp)}, removal,
                                         AuthoritySource::Request, ConfirmationSource::Yes, error);
    INFO(error);
    REQUIRE(record);
    return *record;
}

AuditOutcome outcome(std::string invocation, const tgcli::proto::AccountRemovePlan& removal) {
    std::string error;
    auto record = make_account_remove_success_audit_outcome(
        {std::move(invocation), std::string(kTimestamp)}, removal,
        AccountRemoveRemoteResult::NotPresent, completed_not_present(), error);
    INFO(error);
    REQUIRE(record);
    return *record;
}

std::string numbered_invocation(unsigned value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(30, '0');
    result.push_back(digits[(value >> 4U) & 0xFU]);
    result.push_back(digits[value & 0xFU]);
    return result;
}

void finish_audited_removal(const RemovalJournal& journal, const std::string& invocation,
                            const tgcli::proto::AccountRemovePlan& removal) {
    RemovalJournalFailure failure;
    advance_not_present(journal, invocation);
    REQUIRE(journal.append_outcome(outcome(invocation, removal), failure));
    REQUIRE(journal.advance(invocation, AuditStage::OutcomeSynced, failure));
}

} // namespace

TEST_CASE("removal tombstone has the exact schema and durable ordered transitions",
          "[removal][journal]") {
    const TempJournal temp;
    const RemovalJournal journal(temp.directory(), ::getuid());
    const auto removal = plan();
    RemovalJournalFailure failure;
    REQUIRE(journal.create(std::string(kFirst), removal, failure));

    auto loaded = journal.load(kFirst, failure);
    REQUIRE(loaded);
    CHECK(loaded->stage == AuditStage::Planned);
    CHECK(loaded->completed_stages == std::vector{AuditStage::Planned});
    CHECK(loaded->next_stage == AuditStage::IntentSynced);
    const auto document = serialize(*loaded);
    CHECK(document.size() == 10);
    CHECK(document["plan"] == tgcli::proto::serialize(removal));
    CHECK(document["data_root"].is_null());
    CHECK_THAT(document, tgcli::test::matches_json_schema("removal-tombstone.schema.json"));

    advance_not_present(journal, kFirst);
    REQUIRE(journal.advance(std::string(kFirst), AuditStage::OutcomeSynced, failure));
    loaded = journal.load(kFirst, failure);
    REQUIRE(loaded);
    CHECK(loaded->stage == AuditStage::OutcomeSynced);
    CHECK_FALSE(loaded->next_stage);
    CHECK_THAT(serialize(*loaded),
               tgcli::test::matches_json_schema("removal-tombstone.schema.json"));

    struct stat directory {};
    struct stat tombstone {};
    struct stat journal_lock {};
    REQUIRE(::stat(temp.directory().c_str(), &directory) == 0);
    REQUIRE(::stat(temp.file(std::string(kFirst) + ".json").c_str(), &tombstone) == 0);
    REQUIRE(::stat(temp.file(".journal.lock").c_str(), &journal_lock) == 0);
    CHECK((directory.st_mode & 07777) == 0700);
    CHECK((tombstone.st_mode & 07777) == 0600);
    CHECK(tombstone.st_nlink == 1);
    CHECK((journal_lock.st_mode & 07777) == 0600);
    CHECK(journal_lock.st_uid == ::getuid());
    CHECK(journal_lock.st_nlink == 1);
}

TEST_CASE("removal tombstone rejects skips wrong branches and replacement files",
          "[removal][journal]") {
    const TempJournal temp;
    const RemovalJournal journal(temp.directory(), ::getuid());
    RemovalJournalFailure failure;
    REQUIRE(journal.create(std::string(kFirst), plan(), failure));
    CHECK_FALSE(journal.advance(std::string(kFirst), AuditStage::RemoteConfirmed, failure));
    CHECK(failure.reason == "path_invalid");
    REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
    CHECK_FALSE(journal.advance(std::string(kFirst), AuditStage::RemoteKept, failure));
    CHECK(failure.reason == "path_invalid");

    const RemovalJournal keep_journal(temp.directory(), ::getuid());
    REQUIRE(keep_journal.create(std::string(kSecond), plan(true), failure));
    REQUIRE(keep_journal.advance(std::string(kSecond), AuditStage::IntentSynced, failure));
    CHECK_FALSE(keep_journal.advance(std::string(kSecond), AuditStage::RemoteNotPresent, failure));
    CHECK(failure.reason == "path_invalid");
}

TEST_CASE("global removal audit binds exact intent outcome and tombstone completion",
          "[removal][journal][audit]") {
    const TempJournal temp;
    const RemovalJournal journal(temp.directory(), ::getuid());
    const auto removal = plan();
    RemovalJournalFailure failure;
    REQUIRE(journal.create(std::string(kFirst), removal, failure));
    const auto intent_record = intent(std::string(kFirst), removal);
    CHECK_THAT(serialize(intent_record),
               tgcli::test::matches_json_schema("audit-intent.schema.json"));
    REQUIRE(journal.append_intent(intent_record, failure));
    REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));

    auto presence = journal.audit_presence(kFirst, failure);
    REQUIRE(presence);
    CHECK(presence->intent);
    CHECK_FALSE(presence->outcome);
    REQUIRE(journal.append_outcome(outcome(std::string(kFirst), removal), failure));
    presence = journal.audit_presence(kFirst, failure);
    REQUIRE(presence);
    CHECK(presence->intent);
    CHECK(presence->outcome);
    const auto stored_outcome = journal.audit_outcome(kFirst, failure);
    REQUIRE(stored_outcome);
    CHECK_THAT(*stored_outcome, tgcli::test::matches_json_schema("audit-outcome.schema.json"));

    advance_not_present(journal, kFirst);
    auto inspection = journal.inspect_account("work");
    REQUIRE(inspection.status == RemovalInspectionStatus::Incomplete);
    REQUIRE(inspection.tombstone);
    CHECK(inspection.tombstone->stage == AuditStage::StateRemoved);
    REQUIRE(journal.advance(std::string(kFirst), AuditStage::OutcomeSynced, failure));
    CHECK(journal.inspect_account("work").status == RemovalInspectionStatus::Clean);
}

TEST_CASE("audit rotation waits for a complete intent outcome group", "[removal][journal][audit]") {
    const TempJournal temp;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->rotation_bytes = 1;
    const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
    const auto removal = plan();
    RemovalJournalFailure failure;
    REQUIRE(journal.create(std::string(kFirst), removal, failure));
    REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
    CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));
    advance_not_present(journal, kFirst);
    REQUIRE(journal.append_outcome(outcome(std::string(kFirst), removal), failure));
    REQUIRE(journal.advance(std::string(kFirst), AuditStage::OutcomeSynced, failure));
    REQUIRE(journal.create(std::string(kSecond), removal, failure));
    REQUIRE(journal.append_intent(intent(std::string(kSecond), removal), failure));

    CHECK(std::filesystem::exists(temp.file("audit.log.1")));
    auto first = journal.audit_presence(kFirst, failure);
    auto second = journal.audit_presence(kSecond, failure);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->outcome);
    CHECK(second->intent);
    CHECK_FALSE(second->outcome);
}

TEST_CASE("rotation pins outcome until the tombstone terminal marker survives restart",
          "[removal][journal][audit][rotation][concurrency]") {
    const TempJournal temp;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->rotation_bytes = 1;
    const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
    const auto removal = plan();
    RemovalJournalFailure failure;

    REQUIRE(journal.create(std::string(kFirst), removal, failure));
    REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
    advance_not_present(journal, kFirst);
    REQUIRE(journal.append_outcome(outcome(std::string(kFirst), removal), failure));

    std::vector<std::string> concurrent_invocations;
    std::vector<std::future<std::pair<bool, std::string>>> appends;
    for (unsigned value = 1; value <= 6; ++value) {
        auto invocation = numbered_invocation(value);
        REQUIRE(journal.create(invocation, removal, failure));
        concurrent_invocations.push_back(invocation);
        auto record = intent(invocation, removal);
        appends.push_back(std::async(std::launch::async, [&journal, record = std::move(record)] {
            RemovalJournalFailure thread_failure;
            const bool appended = journal.append_intent(record, thread_failure);
            return std::pair{appended, thread_failure.reason};
        }));
    }
    for (auto& append : appends) {
        const auto [appended, reason] = append.get();
        INFO(reason);
        REQUIRE(appended);
    }
    for (const auto& invocation : concurrent_invocations) {
        finish_audited_removal(journal, invocation, removal);
    }
    CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));

    const RemovalJournal restarted(temp.directory(), ::getuid(), hooks);
    auto first = restarted.audit_presence(kFirst, failure);
    REQUIRE(first);
    CHECK(first->intent);
    CHECK(first->outcome);
    auto first_outcome = restarted.audit_outcome(kFirst, failure);
    REQUIRE(first_outcome);
    REQUIRE(restarted.advance(std::string(kFirst), AuditStage::OutcomeSynced, failure));

    for (unsigned value = 16; value <= 21; ++value) {
        const auto invocation = numbered_invocation(value);
        REQUIRE(restarted.create(invocation, removal, failure));
        REQUIRE(restarted.append_intent(intent(invocation, removal), failure));
        finish_audited_removal(restarted, invocation, removal);
    }
    first = restarted.audit_presence(kFirst, failure);
    REQUIRE(first);
    CHECK_FALSE(first->intent);
    CHECK_FALSE(first->outcome);
}

TEST_CASE("journal faults and unsafe tombstones fail closed", "[removal][journal][audit]") {
    const TempJournal temp;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->should_fail = [](RemovalJournalFault fault) {
        return fault == RemovalJournalFault::TombstoneSync;
    };
    const RemovalJournal failing(temp.directory(), ::getuid(), hooks);
    RemovalJournalFailure failure;
    CHECK_FALSE(failing.create(std::string(kFirst), plan(), failure));
    CHECK(failure.reason == "sync_failed");

    const RemovalJournal journal(temp.directory(), ::getuid());
    REQUIRE(journal.create(std::string(kSecond), plan(), failure));
    const auto tombstone = temp.file(std::string(kSecond) + ".json");
    REQUIRE(::chmod(tombstone.c_str(), 0644) == 0);
    auto inspection = journal.inspect_account("work");
    CHECK(inspection.status == RemovalInspectionStatus::Invalid);
    CHECK(inspection.path == tombstone);

    const TempJournal unsafe_lock_temp;
    const RemovalJournal unsafe_lock_journal(unsafe_lock_temp.directory(), ::getuid());
    REQUIRE(unsafe_lock_journal.create(std::string(kFirst), plan(), failure));
    REQUIRE(::chmod(unsafe_lock_temp.file(".journal.lock").c_str(), 0644) == 0);
    CHECK_FALSE(unsafe_lock_journal.append_intent(intent(std::string(kFirst), plan()), failure));
    CHECK(failure.reason == "path_invalid");
}
