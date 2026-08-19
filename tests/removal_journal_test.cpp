#include "daemon/removal_journal.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
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
constexpr std::string_view kOtherSnapshot =
    "sha256:1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
    "ino:2;size:3;ctime_ns:4";
constexpr std::string_view kTimestamp = "2026-08-04T10:11:12Z";

tgcli::proto::AccountRemovePlan plan(bool keep = false, std::string account = "work",
                                     std::optional<std::string> reassign = "main",
                                     std::string snapshot = std::string(kSnapshot)) {
    std::string error;
    const auto data = "/data/tgcli/accounts/" + account;
    const auto state = "/state/tgcli/accounts/" + account;
    auto result =
        tgcli::proto::make_account_remove_plan({.account = std::move(account),
                                                .keep_session = keep,
                                                .delete_paths = {data, state},
                                                .config_path = "/config/tgcli/config.toml",
                                                .config_snapshot = std::move(snapshot),
                                                .data_root = std::nullopt,
                                                .state_root = std::nullopt,
                                                .reassign_default = std::move(reassign)},
                                               error);
    INFO(error);
    REQUIRE(result);
    return *result;
}

enum class RemoteBranch { Confirmed, SentNotPresent, NotPresent, Kept };

std::vector<AuditStage> completed(RemoteBranch branch) {
    constexpr std::array suffix{AuditStage::ClientCloseStarted,  AuditStage::ClientClosed,
                                AuditStage::ConfigRemoveStarted, AuditStage::ConfigRemoved,
                                AuditStage::DataRemoveStarted,   AuditStage::DataRemoved,
                                AuditStage::StateRemoveStarted,  AuditStage::StateRemoved};
    std::vector<AuditStage> result;
    result.reserve(4 + suffix.size());
    result.push_back(AuditStage::Planned);
    result.push_back(AuditStage::IntentSynced);
    if (branch == RemoteBranch::Confirmed || branch == RemoteBranch::SentNotPresent) {
        result.push_back(AuditStage::RemoteLogoutSendStarted);
    }
    switch (branch) {
    case RemoteBranch::Confirmed:
        result.push_back(AuditStage::RemoteConfirmed);
        break;
    case RemoteBranch::SentNotPresent:
    case RemoteBranch::NotPresent:
        result.push_back(AuditStage::RemoteNotPresent);
        break;
    case RemoteBranch::Kept:
        result.push_back(AuditStage::RemoteKept);
        break;
    }
    for (const auto stage : suffix) {
        result.push_back(stage);
    }
    return result;
}

void advance_to(const RemovalJournal& journal, std::string_view invocation,
                const std::vector<AuditStage>& stages) {
    RemovalJournalFailure failure;
    for (const auto stage : stages | std::views::drop(1)) {
        INFO(audit_stage_name(stage));
        REQUIRE(journal.advance(std::string(invocation), stage, failure));
    }
}

void advance_not_present(const RemovalJournal& journal, std::string_view invocation) {
    advance_to(journal, invocation, completed(RemoteBranch::NotPresent));
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

AccountRemoveRemoteResult remote_result(RemoteBranch branch) {
    switch (branch) {
    case RemoteBranch::Confirmed:
        return AccountRemoveRemoteResult::Confirmed;
    case RemoteBranch::SentNotPresent:
    case RemoteBranch::NotPresent:
        return AccountRemoveRemoteResult::NotPresent;
    case RemoteBranch::Kept:
        return AccountRemoveRemoteResult::Kept;
    }
    return AccountRemoveRemoteResult::NotPresent;
}

AuditOutcome outcome(std::string invocation, const tgcli::proto::AccountRemovePlan& removal,
                     RemoteBranch branch = RemoteBranch::NotPresent) {
    std::string error;
    auto record = make_account_remove_success_audit_outcome(
        {std::move(invocation), std::string(kTimestamp)}, removal, remote_result(branch),
        completed(branch), error);
    INFO(error);
    REQUIRE(record);
    return *record;
}

AuditOutcome failure_outcome(std::string invocation, const tgcli::proto::AccountRemovePlan& removal,
                             const std::vector<AuditStage>& stages) {
    std::string error;
    auto structured = parse_structured_outcome_error(
        {{"code", "INTERNAL"},
         {"details", {{"operation", "account_remove"}, {"reason", "internal_error"}}}},
        error);
    INFO(error);
    REQUIRE(structured);
    auto record = make_failure_audit_outcome({std::move(invocation), std::string(kTimestamp)},
                                             tgcli::proto::DestructivePlan{removal}, stages,
                                             *structured, error);
    INFO(error);
    REQUIRE(record);
    return *record;
}

void overwrite_json(const std::filesystem::path& file, const nlohmann::json& document) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << document.dump() << '\n';
    output.close();
    REQUIRE(output.good());
}

void overwrite_bytes(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << bytes;
    output.close();
    REQUIRE(output.good());
}

void force_terminal(const RemovalJournal& journal, std::string_view invocation,
                    std::vector<AuditStage> prefix) {
    RemovalJournalFailure failure;
    auto tombstone = journal.load(invocation, failure);
    REQUIRE(tombstone);
    auto document = serialize(*tombstone);
    prefix.push_back(AuditStage::OutcomeSynced);
    nlohmann::json stages = nlohmann::json::array();
    for (const auto stage : prefix) {
        stages.push_back(audit_stage_name(stage));
    }
    document["stage"] = "outcome_synced";
    document["completed_stages"] = std::move(stages);
    document["next_stage"] = nullptr;
    overwrite_json(journal.tombstone_path(invocation), document);
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

void create_failed_audited_removal(const RemovalJournal& journal, const std::string& invocation,
                                   const tgcli::proto::AccountRemovePlan& removal) {
    const std::vector<AuditStage> stages{AuditStage::Planned, AuditStage::IntentSynced};
    RemovalJournalFailure failure;
    REQUIRE(journal.create(invocation, removal, failure));
    REQUIRE(journal.append_intent(intent(invocation, removal), failure));
    REQUIRE(journal.advance(invocation, AuditStage::IntentSynced, failure));
    REQUIRE(journal.append_outcome(failure_outcome(invocation, removal, stages), failure));
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

    REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
    advance_not_present(journal, kFirst);
    REQUIRE(journal.append_outcome(outcome(std::string(kFirst), removal), failure));
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

    auto replacement = keep_journal.load(kSecond, failure);
    REQUIRE(replacement);
    auto replacement_document = serialize(*replacement);
    replacement_document["invocation_id"] = kFirst;
    overwrite_json(keep_journal.tombstone_path(kSecond), replacement_document);
    CHECK_FALSE(keep_journal.load(kSecond, failure));
    CHECK(failure.reason == "path_invalid");
}

TEST_CASE("terminal removal tombstone requires the durable intent prefix",
          "[removal][journal][audit]") {
    const TempJournal temp;
    const RemovalJournal journal(temp.directory(), ::getuid());
    RemovalJournalFailure failure;
    REQUIRE(journal.create(std::string(kFirst), plan(), failure));
    auto tombstone = journal.load(kFirst, failure);
    REQUIRE(tombstone);
    auto document = serialize(*tombstone);
    document["stage"] = "outcome_synced";
    document["completed_stages"] = nlohmann::json::array({"planned", "outcome_synced"});
    document["next_stage"] = nullptr;
    std::ofstream output(temp.file(std::string(kFirst) + ".json"),
                         std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << document.dump() << '\n';
    output.close();

    CHECK_FALSE(journal.load(kFirst, failure));
    CHECK(failure.reason == "path_invalid");
}

TEST_CASE("terminal transition and inspection require the exact canonical audit group",
          "[removal][journal][audit]") {
    const TempJournal temp;
    const RemovalJournal journal(temp.directory(), ::getuid());
    const auto removal = plan();
    RemovalJournalFailure failure;
    REQUIRE(journal.create(std::string(kFirst), removal, failure));

    std::vector<AuditStage> tombstone_prefix{AuditStage::Planned, AuditStage::IntentSynced};
    const auto write_other_group = [&](const tgcli::proto::AccountRemovePlan& other) {
        overwrite_bytes(
            temp.file("audit.log"),
            serialize(intent(std::string(kFirst), other)).dump() + '\n' +
                serialize(failure_outcome(std::string(kFirst), other, tombstone_prefix)).dump() +
                '\n');
        REQUIRE(::chmod(temp.file("audit.log").c_str(), 0600) == 0);
    };

    SECTION("missing intent and outcome") {
        REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
    }

    SECTION("missing outcome") {
        REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
    }

    SECTION("mismatched plan") {
        const auto other = plan(false, "work", "main", std::string(kOtherSnapshot));
        REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
        write_other_group(other);
    }

    SECTION("mismatched account") {
        const auto other = plan(false, "other");
        REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
        write_other_group(other);
    }

    SECTION("mismatched keep-session policy") {
        const auto other = plan(true);
        REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
        write_other_group(other);
    }

    SECTION("mismatched completed prefix") {
        REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        REQUIRE(journal.append_outcome(
            failure_outcome(std::string(kFirst), removal, tombstone_prefix), failure));
        tombstone_prefix = completed(RemoteBranch::NotPresent);
        advance_to(journal, kFirst, tombstone_prefix);
    }

    SECTION("outcome success fields contradict its failure flag") {
        REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        tombstone_prefix = completed(RemoteBranch::NotPresent);
        advance_to(journal, kFirst, tombstone_prefix);
        auto malformed = serialize(outcome(std::string(kFirst), removal));
        malformed["success"] = false;
        overwrite_bytes(temp.file("audit.log"),
                        serialize(intent(std::string(kFirst), removal)).dump() + '\n' +
                            malformed.dump() + '\n');
    }

    SECTION("truncated audit") {
        const auto record = serialize(intent(std::string(kFirst), removal)).dump();
        REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
        overwrite_bytes(temp.file("audit.log"), record);
    }

    CHECK_FALSE(journal.advance(std::string(kFirst), AuditStage::OutcomeSynced, failure));
    CHECK(failure.reason == "path_invalid");
    auto loaded = journal.load(kFirst, failure);
    REQUIRE(loaded);
    CHECK(loaded->stage == tombstone_prefix.back());

    force_terminal(journal, kFirst, tombstone_prefix);
    const auto inspection = journal.inspect_account("work");
    CHECK(inspection.status == RemovalInspectionStatus::Invalid);
    CHECK(inspection.failure.reason == "path_invalid");
}

TEST_CASE("split audit generations never verify or rotate a terminal group",
          "[removal][journal][audit][rotation][restart]") {
    for (const bool intent_is_oldest : {true, false}) {
        INFO(intent_is_oldest);
        const TempJournal temp;
        auto hooks = std::make_shared<testing::RemovalJournalHooks>();
        hooks->rotation_bytes = 1;
        const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
        const auto removal = plan();
        RemovalJournalFailure failure;
        REQUIRE(journal.create(std::string(kFirst), removal, failure));
        const auto intent_line = serialize(intent(std::string(kFirst), removal)).dump() + '\n';
        REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        const auto stages = completed(RemoteBranch::NotPresent);
        advance_to(journal, kFirst, stages);
        const auto outcome_line = serialize(outcome(std::string(kFirst), removal)).dump() + '\n';
        overwrite_bytes(temp.file("audit.log"), intent_is_oldest ? outcome_line : intent_line);
        overwrite_bytes(temp.file("audit.log.4"), intent_is_oldest ? intent_line : outcome_line);
        REQUIRE(::chmod(temp.file("audit.log.4").c_str(), 0600) == 0);

        CHECK_FALSE(journal.advance(std::string(kFirst), AuditStage::OutcomeSynced, failure));
        CHECK(failure.reason == "path_invalid");
        force_terminal(journal, kFirst, stages);

        const RemovalJournal restarted(temp.directory(), ::getuid(), hooks);
        const auto inspection = restarted.inspect_account("work");
        CHECK(inspection.status == RemovalInspectionStatus::Invalid);
        CHECK(inspection.path == restarted.audit_path());
        CHECK(inspection.failure.reason == "path_invalid");

        const auto active_size = std::filesystem::file_size(temp.file("audit.log"));
        const auto oldest_size = std::filesystem::file_size(temp.file("audit.log.4"));
        const auto trigger = numbered_invocation(intent_is_oldest ? 224 : 225);
        const auto other = plan(false, "other");
        REQUIRE(restarted.create(trigger, other, failure));
        CHECK_FALSE(restarted.append_intent(intent(trigger, other), failure));
        CHECK(failure.reason == "path_invalid");
        CHECK(std::filesystem::file_size(temp.file("audit.log")) == active_size);
        CHECK(std::filesystem::file_size(temp.file("audit.log.4")) == oldest_size);
        CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));
        CHECK(std::filesystem::exists(temp.file(std::string(kFirst) + ".json")));
    }
}

TEST_CASE("canonical terminal audit accepts every removal remote branch",
          "[removal][journal][audit]") {
    constexpr std::array branches{RemoteBranch::Confirmed, RemoteBranch::SentNotPresent,
                                  RemoteBranch::NotPresent, RemoteBranch::Kept};
    unsigned index = 1;
    for (const auto branch : branches) {
        INFO(index);
        const TempJournal temp;
        const RemovalJournal journal(temp.directory(), ::getuid());
        const auto removal = plan(branch == RemoteBranch::Kept);
        const auto invocation = numbered_invocation(index++);
        RemovalJournalFailure failure;
        REQUIRE(journal.create(invocation, removal, failure));
        REQUIRE(journal.append_intent(intent(invocation, removal), failure));
        advance_to(journal, invocation, completed(branch));
        REQUIRE(journal.append_outcome(outcome(invocation, removal, branch), failure));
        REQUIRE(journal.advance(invocation, AuditStage::OutcomeSynced, failure));
        CHECK(journal.inspect_account("work").status == RemovalInspectionStatus::Clean);
        auto loaded = journal.load(invocation, failure);
        REQUIRE(loaded);
        CHECK(serialize(*loaded)["completed_stages"].back() == "outcome_synced");
    }
}

TEST_CASE("canonical early failure terminalizes default and keep-session policies",
          "[removal][journal][audit]") {
    unsigned index = 32;
    for (const bool keep_session : {false, true}) {
        INFO(keep_session);
        const TempJournal temp;
        const RemovalJournal journal(temp.directory(), ::getuid());
        const auto removal = plan(keep_session);
        const auto invocation = numbered_invocation(index++);
        const std::vector<AuditStage> stages{AuditStage::Planned, AuditStage::IntentSynced};
        RemovalJournalFailure failure;
        REQUIRE(journal.create(invocation, removal, failure));
        REQUIRE(journal.append_intent(intent(invocation, removal), failure));
        REQUIRE(journal.advance(invocation, AuditStage::IntentSynced, failure));
        REQUIRE(journal.append_outcome(failure_outcome(invocation, removal, stages), failure));
        REQUIRE(journal.advance(invocation, AuditStage::OutcomeSynced, failure));
        CHECK(journal.inspect_account("work").status == RemovalInspectionStatus::Clean);
    }
}

TEST_CASE("canonical failure terminalizes every removal remote branch prefix",
          "[removal][journal][audit]") {
    constexpr std::array branches{RemoteBranch::Confirmed, RemoteBranch::SentNotPresent,
                                  RemoteBranch::NotPresent, RemoteBranch::Kept};
    unsigned index = 48;
    for (const auto branch : branches) {
        INFO(index);
        const TempJournal temp;
        const RemovalJournal journal(temp.directory(), ::getuid());
        const auto removal = plan(branch == RemoteBranch::Kept);
        const auto invocation = numbered_invocation(index++);
        auto stages = completed(branch);
        stages.erase(std::ranges::find(stages, AuditStage::ClientCloseStarted), stages.end());
        RemovalJournalFailure failure;
        REQUIRE(journal.create(invocation, removal, failure));
        REQUIRE(journal.append_intent(intent(invocation, removal), failure));
        advance_to(journal, invocation, stages);
        REQUIRE(journal.append_outcome(failure_outcome(invocation, removal, stages), failure));
        REQUIRE(journal.advance(invocation, AuditStage::OutcomeSynced, failure));
        CHECK(journal.inspect_account("work").status == RemovalInspectionStatus::Clean);
    }
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

TEST_CASE("nonterminal tombstone without an audit group pins rotation",
          "[removal][journal][audit][rotation]") {
    const TempJournal temp;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->rotation_bytes = 1;
    const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
    const auto removal = plan();
    RemovalJournalFailure failure;

    create_failed_audited_removal(journal, std::string(kSecond), removal);
    for (unsigned value = 176; value <= 179; ++value) {
        const auto invocation = numbered_invocation(value);
        create_failed_audited_removal(journal, invocation, removal);
    }
    constexpr std::array<const char*, 4> rotated_names{"audit.log.1", "audit.log.2", "audit.log.3",
                                                       "audit.log.4"};
    std::array<struct stat, rotated_names.size()> rotated_before{};
    for (std::size_t index = 0; index < rotated_names.size(); ++index) {
        REQUIRE(::stat(temp.file(rotated_names.at(index)).c_str(), &rotated_before.at(index)) == 0);
    }
    REQUIRE(journal.create(std::string(kFirst), removal, failure));
    struct stat before {};
    REQUIRE(::stat(temp.file("audit.log").c_str(), &before) == 0);
    const auto trigger = numbered_invocation(208);
    const auto other = plan(false, "other");
    REQUIRE(journal.create(trigger, other, failure));
    REQUIRE(journal.append_intent(intent(trigger, other), failure));

    struct stat after {};
    REQUIRE(::stat(temp.file("audit.log").c_str(), &after) == 0);
    CHECK(after.st_dev == before.st_dev);
    CHECK(after.st_ino == before.st_ino);
    for (std::size_t index = 0; index < rotated_names.size(); ++index) {
        struct stat rotated_after {};
        REQUIRE(::stat(temp.file(rotated_names.at(index)).c_str(), &rotated_after) == 0);
        CHECK(rotated_after.st_dev == rotated_before.at(index).st_dev);
        CHECK(rotated_after.st_ino == rotated_before.at(index).st_ino);
        CHECK(rotated_after.st_size == rotated_before.at(index).st_size);
    }
    auto nonterminal = journal.load(kFirst, failure);
    REQUIRE(nonterminal);
    CHECK(nonterminal->stage == AuditStage::Planned);
    auto completed_group = journal.audit_presence(kSecond, failure);
    REQUIRE(completed_group);
    CHECK(completed_group->outcome);
}

TEST_CASE("incoming Planned rotation exception rejects existing stages records and policy drift",
          "[removal][journal][audit][rotation]") {
    const TempJournal temp;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->rotation_bytes = 1;
    const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
    const auto removal = plan();
    RemovalJournalFailure failure;
    create_failed_audited_removal(journal, std::string(kSecond), removal);
    REQUIRE(journal.create(std::string(kFirst), removal, failure));

    SECTION("extra durable stage") {
        REQUIRE(journal.advance(std::string(kFirst), AuditStage::IntentSynced, failure));
        CHECK_FALSE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        CHECK(failure.reason == "path_invalid");
        CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));
    }

    SECTION("existing audit record") {
        REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        const auto active_size = std::filesystem::file_size(temp.file("audit.log"));
        CHECK_FALSE(journal.append_intent(intent(std::string(kFirst), removal), failure));
        CHECK(failure.reason == "path_invalid");
        CHECK(std::filesystem::file_size(temp.file("audit.log")) == active_size);
    }

    SECTION("policy mismatch") {
        CHECK_FALSE(journal.append_intent(intent(std::string(kFirst), plan(true)), failure));
        CHECK(failure.reason == "path_invalid");
        CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));
    }

    SECTION("account mismatch") {
        CHECK_FALSE(
            journal.append_intent(intent(std::string(kFirst), plan(false, "other")), failure));
        CHECK(failure.reason == "path_invalid");
        CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));
    }
}

TEST_CASE("incoming audit intent without its owned Planned tombstone fails closed",
          "[removal][journal][audit][rotation]") {
    const TempJournal temp;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->rotation_bytes = 1;
    const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
    const auto removal = plan();
    create_failed_audited_removal(journal, std::string(kSecond), removal);
    const auto active_size = std::filesystem::file_size(temp.file("audit.log"));
    RemovalJournalFailure failure;
    const auto invocation = numbered_invocation(216);

    CHECK_FALSE(journal.append_intent(intent(invocation, removal), failure));
    CHECK(failure.reason == "path_invalid");
    CHECK(std::filesystem::file_size(temp.file("audit.log")) == active_size);
    CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));
    CHECK_FALSE(std::filesystem::exists(journal.tombstone_path(invocation)));
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
        create_failed_audited_removal(restarted, invocation, removal);
    }
    first = restarted.audit_presence(kFirst, failure);
    REQUIRE(first);
    CHECK_FALSE(first->intent);
    CHECK_FALSE(first->outcome);
}

TEST_CASE("rotation durably retires a verified terminal before its audit group is evicted",
          "[removal][journal][audit][rotation][restart]") {
    const TempJournal temp;
    bool fail_rotation = false;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->rotation_bytes = 1;
    hooks->should_fail = [&fail_rotation](RemovalJournalFault fault) {
        return fail_rotation && fault == RemovalJournalFault::AuditRotate;
    };
    const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
    const auto removal = plan();
    RemovalJournalFailure failure;

    REQUIRE(journal.create(std::string(kFirst), removal, failure));
    REQUIRE(journal.append_intent(intent(std::string(kFirst), removal), failure));
    finish_audited_removal(journal, std::string(kFirst), removal);
    for (unsigned value = 1; value <= 4; ++value) {
        const auto invocation = numbered_invocation(value);
        create_failed_audited_removal(journal, invocation, removal);
    }
    REQUIRE(std::filesystem::exists(temp.file("audit.log.4")));
    REQUIRE(std::filesystem::exists(temp.file(std::string(kFirst) + ".json")));

    const auto trigger = numbered_invocation(240);
    const auto other = plan(false, "other");
    REQUIRE(journal.create(trigger, other, failure));
    fail_rotation = true;
    CHECK_FALSE(journal.append_intent(intent(trigger, other), failure));
    CHECK(failure.reason == "rotate_failed");
    CHECK_FALSE(std::filesystem::exists(temp.file(std::string(kFirst) + ".json")));
    CHECK(std::filesystem::exists(temp.file("audit.log.4")));

    const RemovalJournal restarted(temp.directory(), ::getuid(), hooks);
    CHECK(restarted.inspect_account("work").status == RemovalInspectionStatus::Clean);
    CHECK(restarted.inspect_account("other").status == RemovalInspectionStatus::Incomplete);
    auto first = restarted.audit_presence(kFirst, failure);
    REQUIRE(first);
    CHECK(first->intent);
    CHECK(first->outcome);

    fail_rotation = false;
    REQUIRE(restarted.append_intent(intent(trigger, other), failure));
    first = restarted.audit_presence(kFirst, failure);
    REQUIRE(first);
    CHECK_FALSE(first->intent);
    CHECK_FALSE(first->outcome);
}

TEST_CASE("restart and concurrent rotation preserve an unverified terminal tombstone",
          "[removal][journal][audit][rotation][concurrency]") {
    const TempJournal temp;
    auto hooks = std::make_shared<testing::RemovalJournalHooks>();
    hooks->rotation_bytes = 1;
    const RemovalJournal journal(temp.directory(), ::getuid(), hooks);
    const auto removal = plan();
    const auto unverified = numbered_invocation(0);
    RemovalJournalFailure failure;

    REQUIRE(journal.create(unverified, removal, failure));
    REQUIRE(journal.advance(unverified, AuditStage::IntentSynced, failure));
    force_terminal(journal, unverified, {AuditStage::Planned, AuditStage::IntentSynced});
    create_failed_audited_removal(journal, std::string(kSecond), removal);

    std::vector<std::future<std::pair<bool, std::string>>> appends;
    for (unsigned value = 1; value <= 6; ++value) {
        const auto invocation = numbered_invocation(value);
        REQUIRE(journal.create(invocation, removal, failure));
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
    CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));

    const RemovalJournal restarted(temp.directory(), ::getuid(), hooks);
    const auto inspection = restarted.inspect_account("work");
    CHECK(inspection.status == RemovalInspectionStatus::Invalid);
    CHECK(inspection.path == restarted.tombstone_path(unverified));
    CHECK(inspection.failure.reason == "path_invalid");
    CHECK_FALSE(restarted.advance(unverified, AuditStage::OutcomeSynced, failure));
    CHECK(failure.reason == "path_invalid");
    CHECK_FALSE(std::filesystem::exists(temp.file("audit.log.1")));
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
