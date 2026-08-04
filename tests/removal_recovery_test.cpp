#include "daemon/removal_recovery.hpp"

#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::daemon;

namespace {

constexpr std::string_view kSnapshot =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;dev:1;"
    "ino:2;size:3;ctime_ns:4";

tgcli::proto::AccountRemovePlan plan(bool roots_present = true) {
    std::string error;
    auto value = tgcli::proto::make_account_remove_plan(
        {.account = "work",
         .keep_session = false,
         .delete_paths = {"/data/accounts/work", "/state/accounts/work"},
         .config_path = "/config/tgcli/config.toml",
         .config_snapshot = std::string(kSnapshot),
         .data_root = roots_present ? std::optional{tgcli::proto::RootIdentity{
                                          "/data/accounts/work", 1, 2, 1000}}
                                    : std::nullopt,
         .state_root = roots_present ? std::optional{tgcli::proto::RootIdentity{
                                           "/state/accounts/work", 1, 3, 1000}}
                                     : std::nullopt,
         .reassign_default = "main"},
        error);
    INFO(error);
    REQUIRE(value);
    return *value;
}

RemovalTombstone tombstone(AuditStage stage, bool roots_present = true) {
    const std::vector full{AuditStage::Planned,          AuditStage::IntentSynced,
                           AuditStage::RemoteNotPresent, AuditStage::ClientCloseStarted,
                           AuditStage::ClientClosed,     AuditStage::ConfigRemoveStarted,
                           AuditStage::ConfigRemoved,    AuditStage::DataRemoveStarted,
                           AuditStage::DataRemoved,      AuditStage::StateRemoveStarted,
                           AuditStage::StateRemoved,     AuditStage::OutcomeSynced};
    const auto found = std::ranges::find(full, stage);
    REQUIRE(found != full.end());
    std::vector<AuditStage> completed(full.begin(), found + 1);
    return {"00112233445566778899aabbccddeeff",
            "work",
            stage,
            std::move(completed),
            std::nullopt,
            plan(roots_present)};
}

tgcli::config::ConfigSnapshot captured_config() {
    tgcli::config::ConfigSnapshot value;
    value.identity = kSnapshot;
    value.default_account = "work";
    value.accounts.emplace("main", tgcli::config::AccountConfig{});
    value.accounts.emplace("work", tgcli::config::AccountConfig{});
    return value;
}

tgcli::config::ConfigSnapshot removed_config() {
    auto value = captured_config();
    value.identity =
        "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789;dev:1;"
        "ino:4;size:5;ctime_ns:6";
    value.default_account = "main";
    value.accounts.erase("work");
    return value;
}

RemovalRecoveryAction decide(const RemovalTombstone& value,
                             const tgcli::config::ConfigSnapshot& config,
                             RemovalRootObservation data, RemovalRootObservation state,
                             bool intent = true, bool outcome = false) {
    const auto decision = decide_removal_recovery(value, {config, data, state, {intent, outcome}});
    INFO(decision.reason);
    REQUIRE(decision);
    return *decision.action;
}

} // namespace

TEST_CASE("recovery table maps every durable boundary to one deterministic action",
          "[removal][recovery]") {
    const auto before = captured_config();
    const auto after = removed_config();
    constexpr auto captured = RemovalRootObservation::Captured;
    constexpr auto absent = RemovalRootObservation::Absent;

    CHECK(decide(tombstone(AuditStage::Planned), before, captured, captured, false) ==
          RemovalRecoveryAction::EnsureIntent);
    CHECK(decide(tombstone(AuditStage::IntentSynced), before, captured, captured) ==
          RemovalRecoveryAction::ReevaluateRemote);
    CHECK(decide(tombstone(AuditStage::RemoteNotPresent), before, captured, captured) ==
          RemovalRecoveryAction::QuiesceClient);
    CHECK(decide(tombstone(AuditStage::ClientCloseStarted), before, captured, captured) ==
          RemovalRecoveryAction::QuiesceClient);
    CHECK(decide(tombstone(AuditStage::ClientClosed), before, captured, captured) ==
          RemovalRecoveryAction::BeginConfigRemoval);
    CHECK(decide(tombstone(AuditStage::ConfigRemoveStarted), before, captured, captured) ==
          RemovalRecoveryAction::RetryConfigRemoval);
    CHECK(decide(tombstone(AuditStage::ConfigRemoveStarted), after, captured, captured) ==
          RemovalRecoveryAction::RecordConfigRemoved);
    CHECK(decide(tombstone(AuditStage::ConfigRemoved), after, captured, captured) ==
          RemovalRecoveryAction::FinishDataRemoval);
    CHECK(decide(tombstone(AuditStage::DataRemoveStarted), after, RemovalRootObservation::Staged,
                 captured) == RemovalRecoveryAction::FinishDataRemoval);
    CHECK(decide(tombstone(AuditStage::DataRemoveStarted), after, absent, captured) ==
          RemovalRecoveryAction::FinishDataRemoval);
    CHECK(decide(tombstone(AuditStage::DataRemoved), after, absent, captured) ==
          RemovalRecoveryAction::FinishStateRemoval);
    CHECK(decide(tombstone(AuditStage::StateRemoveStarted), after, absent,
                 RemovalRootObservation::Staged) == RemovalRecoveryAction::FinishStateRemoval);
    CHECK(decide(tombstone(AuditStage::StateRemoved), after, absent, absent, true, false) ==
          RemovalRecoveryAction::EnsureOutcome);
    CHECK(decide(tombstone(AuditStage::StateRemoved), after, absent, absent, true, true) ==
          RemovalRecoveryAction::RecordOutcomeSynced);
    CHECK(decide(tombstone(AuditStage::OutcomeSynced), before, RemovalRootObservation::Changed,
                 RemovalRootObservation::Changed, false, false) == RemovalRecoveryAction::Complete);
}

TEST_CASE("recovery accepts only planned absence for roots captured as absent",
          "[removal][recovery]") {
    const auto before = captured_config();
    const auto after = removed_config();
    const auto planned_absent = RemovalRootObservation::PlannedAbsent;
    CHECK(decide(tombstone(AuditStage::IntentSynced, false), before, planned_absent,
                 planned_absent) == RemovalRecoveryAction::ReevaluateRemote);
    CHECK(decide(tombstone(AuditStage::ConfigRemoved, false), after, planned_absent,
                 planned_absent) == RemovalRecoveryAction::FinishDataRemoval);
    CHECK(decide(tombstone(AuditStage::StateRemoved, false), after, planned_absent,
                 planned_absent) == RemovalRecoveryAction::EnsureOutcome);
}

TEST_CASE("recovery rejects changed resurrected and prematurely absent objects",
          "[removal][recovery]") {
    const auto before = captured_config();
    const auto after = removed_config();
    const auto changed = RemovalRootObservation::Changed;
    const auto captured = RemovalRootObservation::Captured;
    const auto absent = RemovalRootObservation::Absent;

    for (const auto& decision : {
             decide_removal_recovery(tombstone(AuditStage::IntentSynced),
                                     {before, changed, captured, {true, false}}),
             decide_removal_recovery(tombstone(AuditStage::ConfigRemoved),
                                     {before, captured, captured, {true, false}}),
             decide_removal_recovery(tombstone(AuditStage::ConfigRemoved),
                                     {after, absent, captured, {true, false}}),
             decide_removal_recovery(tombstone(AuditStage::DataRemoved),
                                     {after, absent, changed, {true, false}}),
             decide_removal_recovery(tombstone(AuditStage::StateRemoved),
                                     {after, absent, absent, {false, true}}),
         }) {
        CHECK_FALSE(decision);
        CHECK(decision.reason == "identity_ambiguous");
    }
}

TEST_CASE("local recovery starts only after durable remote proof", "[removal][recovery]") {
    const auto can_resume = [](AuditStage stage) {
        auto value = tombstone(AuditStage::IntentSynced);
        value.stage = stage;
        return can_resume_removal_without_tdlib(value);
    };
    for (const auto stage :
         {AuditStage::Planned, AuditStage::IntentSynced, AuditStage::RemoteLogoutSendStarted}) {
        CHECK_FALSE(can_resume(stage));
    }
    for (const auto stage :
         {AuditStage::RemoteConfirmed, AuditStage::RemoteNotPresent, AuditStage::RemoteKept,
          AuditStage::ClientCloseStarted, AuditStage::ClientClosed, AuditStage::ConfigRemoveStarted,
          AuditStage::ConfigRemoved, AuditStage::DataRemoveStarted, AuditStage::DataRemoved,
          AuditStage::StateRemoveStarted, AuditStage::StateRemoved}) {
        CHECK(can_resume(stage));
    }
}
