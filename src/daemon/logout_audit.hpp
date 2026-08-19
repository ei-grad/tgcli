#pragma once

#include "daemon/destructive_contract.hpp"
#include "proto/destructive_plan.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

enum class LogoutAuditFault { Open, Write, Sync, InspectSync, Rotate };

namespace testing {

struct LogoutAuditHooks {
    std::function<bool(LogoutAuditFault)> should_fail;
    std::function<void(std::string_view)> after_sync;
    std::size_t rotation_bytes = std::size_t{32} * 1024 * 1024;
};

} // namespace testing

struct LogoutAuditFailure {
    std::string reason;
};

struct IncompleteLogoutAudit {
    std::string invocation_id;
    proto::LogoutPlan plan;
    std::vector<AuditStage> completed_stages;
};

enum class LogoutAuditInspectionStatus { Clean, Incomplete, Invalid };

struct LogoutAuditInspection {
    LogoutAuditInspectionStatus status = LogoutAuditInspectionStatus::Clean;
    std::optional<IncompleteLogoutAudit> incomplete;
    LogoutAuditFailure failure;
};

class LogoutAuditRecordAdapter final {
  public:
    explicit LogoutAuditRecordAdapter(std::string account);
    ~LogoutAuditRecordAdapter();
    LogoutAuditRecordAdapter(LogoutAuditRecordAdapter&&) noexcept;
    LogoutAuditRecordAdapter& operator=(LogoutAuditRecordAdapter&&) noexcept;
    LogoutAuditRecordAdapter(const LogoutAuditRecordAdapter&) = delete;
    LogoutAuditRecordAdapter& operator=(const LogoutAuditRecordAdapter&) = delete;

    [[nodiscard]] bool consume(const nlohmann::json& record, bool invocation_previously_seen,
                               LogoutAuditFailure& failure);
    [[nodiscard]] LogoutAuditInspection finish();
    [[nodiscard]] bool has_incomplete() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LogoutAuditLog final {
  public:
    LogoutAuditLog(std::string state_directory, std::string account, uid_t expected_uid,
                   std::shared_ptr<const testing::LogoutAuditHooks> hooks = {});

    [[nodiscard]] const std::string& path() const;
    [[nodiscard]] bool append(const nlohmann::json& record, LogoutAuditFailure& failure,
                              bool begin_group = false) const;
    [[nodiscard]] LogoutAuditInspection inspect() const;

  private:
    std::string state_directory_;
    std::string audit_path_;
    std::string account_;
    uid_t expected_uid_;
    std::shared_ptr<const testing::LogoutAuditHooks> hooks_;
};

enum class LogoutAuditReconcileStatus { Clean, ObservationRequired, Invalid, AppendFailed };

struct LogoutAuditReconcileResult {
    LogoutAuditReconcileStatus status = LogoutAuditReconcileStatus::Clean;
    std::optional<IncompleteLogoutAudit> incomplete;
    LogoutAuditFailure failure;
};

LogoutAuditReconcileResult
reconcile_definite_logout_audit(const LogoutAuditLog& audit,
                                const std::function<std::string()>& timestamp);

} // namespace tgcli::daemon
