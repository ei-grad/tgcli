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
