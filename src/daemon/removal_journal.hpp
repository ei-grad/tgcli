#pragma once

#include "daemon/destructive_contract.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace tgcli::daemon {

enum class RemovalJournalFault {
    DirectoryOpen,
    TombstoneWrite,
    TombstoneSync,
    AuditWrite,
    AuditSync,
    AuditRotate,
};

namespace testing {

struct RemovalJournalHooks {
    std::function<bool(RemovalJournalFault)> should_fail;
    std::function<void(std::string_view, AuditStage)> after_tombstone_sync;
    std::function<void(std::string_view)> after_audit_sync;
    std::size_t rotation_bytes = std::size_t{32} * 1024 * 1024;
};

} // namespace testing

struct RemovalJournalFailure {
    std::string reason;
};

struct RemovalTombstone {
    std::string invocation_id;
    std::string account;
    AuditStage stage = AuditStage::Planned;
    std::vector<AuditStage> completed_stages;
    std::optional<AuditStage> next_stage;
    proto::AccountRemovePlan plan;
};

enum class RemovalInspectionStatus { Clean, Incomplete, Invalid };

struct RemovalInspection {
    RemovalInspectionStatus status = RemovalInspectionStatus::Clean;
    std::optional<RemovalTombstone> tombstone;
    std::string path;
    RemovalJournalFailure failure;
};

struct RemovalAuditPresence {
    bool intent = false;
    bool outcome = false;
};

class RemovalJournal final {
  public:
    RemovalJournal(std::string directory, uid_t expected_uid,
                   std::shared_ptr<const testing::RemovalJournalHooks> hooks = {});

    [[nodiscard]] const std::string& directory() const;
    [[nodiscard]] std::string audit_path() const;
    [[nodiscard]] std::string tombstone_path(std::string_view invocation_id) const;

    [[nodiscard]] bool create(const std::string& invocation_id,
                              const proto::AccountRemovePlan& plan,
                              RemovalJournalFailure& failure) const;
    [[nodiscard]] bool advance(const std::string& invocation_id, AuditStage stage,
                               RemovalJournalFailure& failure) const;
    [[nodiscard]] std::optional<RemovalTombstone> load(std::string_view invocation_id,
                                                       RemovalJournalFailure& failure) const;
    [[nodiscard]] RemovalInspection inspect_account(std::string_view account) const;

    [[nodiscard]] bool append_intent(const AuditIntent& intent,
                                     RemovalJournalFailure& failure) const;
    [[nodiscard]] bool append_outcome(const AuditOutcome& outcome,
                                      RemovalJournalFailure& failure) const;
    [[nodiscard]] std::optional<RemovalAuditPresence>
    audit_presence(std::string_view invocation_id, RemovalJournalFailure& failure) const;
    [[nodiscard]] std::optional<nlohmann::json> audit_outcome(std::string_view invocation_id,
                                                              RemovalJournalFailure& failure) const;

  private:
    std::string directory_;
    uid_t expected_uid_;
    std::shared_ptr<const testing::RemovalJournalHooks> hooks_;
};

[[nodiscard]] nlohmann::json serialize(const RemovalTombstone& tombstone);

} // namespace tgcli::daemon
