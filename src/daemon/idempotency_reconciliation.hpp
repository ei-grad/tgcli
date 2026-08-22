#pragma once

#include "daemon/idempotency_store.hpp"

#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>
#include <variant>

namespace tgcli::daemon_lock {
class LifetimeLease;
}

namespace tgcli::daemon {

enum class IdempotencyCoreGateStatus {
    Clean,
    Interrupted,
    SpoolUnavailable,
    AuditUnavailable,
    AuditIncomplete,
    StoreUnavailable,
};

struct IdempotencyCoreGateResult {
    IdempotencyCoreGateStatus status = IdempotencyCoreGateStatus::StoreUnavailable;
    IdempotencySnapshot snapshot;
    std::optional<nlohmann::json> terminal;
    IdempotencyFailure store_failure;
    AccountAuditFailure audit_failure;
    std::optional<FileSpoolError> spool_failure;
};

enum class IdempotencyUnexpectedIncumbentClosureStatus { DurableFatal, AuditFatal };

struct IdempotencyUnexpectedIncumbentClosure {
    IdempotencyUnexpectedIncumbentClosureStatus status =
        IdempotencyUnexpectedIncumbentClosureStatus::AuditFatal;
    std::optional<nlohmann::json> terminal;
    AccountAuditFailure audit_failure;
};

using IdempotencyRecoveryTimestamp = std::function<std::string()>;

namespace testing {
struct IdempotencyReconciliationHooks {
    std::function<void(std::string_view)> after_boundary;
};
} // namespace testing

class IdempotencyFoundation final {
  public:
    static std::variant<IdempotencyFoundation, IdempotencyFailure>
    create(std::string state_directory, std::string account, uid_t expected_uid,
           std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease,
           std::shared_ptr<const testing::AccountAuditHooks> audit_hooks = {},
           std::shared_ptr<const testing::IdempotencyStoreHooks> store_hooks = {});

    using EpochResult = AccountAuditCoordinator::LockResult;
    [[nodiscard]] AccountAuditCoordinator::Guard acquire_epoch();
    [[nodiscard]] EpochResult acquire_epoch(AccountAuditScanControl control);
    [[nodiscard]] IdempotencyCoreGateResult
    run_core_gate(const AccountAuditCoordinator::Guard& guard, std::uint64_t sampled_now,
                  const IdempotencyRecoveryTimestamp& timestamp = {},
                  const FileSpoolControl& spool_control = {},
                  const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks = {},
                  const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks = {});
    [[nodiscard]] IdempotencyCoreGateResult run_absent_by_policy_gate(
        const AccountAuditCoordinator::Guard& guard, std::uint64_t sampled_now,
        const IdempotencyRecoveryTimestamp& timestamp = {},
        const FileSpoolControl& spool_control = {},
        const std::shared_ptr<const testing::FileSpoolHooks>& spool_hooks = {},
        const std::shared_ptr<const testing::IdempotencyReconciliationHooks>& hooks = {});
    [[nodiscard]] IdempotencyUnexpectedIncumbentClosure
    close_unexpected_incumbent(const AccountAuditAppendReceipt& intent_receipt,
                               const AccountAuditCoordinator::Guard& guard,
                               const IdempotencyRecoveryTimestamp& timestamp = {});

    [[nodiscard]] IdempotencyStore& store();
    [[nodiscard]] AccountAuditLog& audit();
    [[nodiscard]] const std::shared_ptr<AccountAuditCoordinator>& coordinator() const;
    [[nodiscard]] const std::string& state_directory() const noexcept;
    [[nodiscard]] uid_t expected_uid() const noexcept;

  private:
    IdempotencyFoundation(std::string state_directory, std::string account, uid_t expected_uid,
                          std::shared_ptr<AccountAuditCoordinator> coordinator,
                          IdempotencyStore store,
                          std::shared_ptr<const testing::AccountAuditHooks> audit_hooks);

    std::string state_directory_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<AccountAuditCoordinator> coordinator_;
    IdempotencyStore store_;
    AccountAuditLog audit_;
};

[[nodiscard]] nlohmann::json
unexpected_idempotency_incumbent_terminal(AccountAuditOperation operation);

} // namespace tgcli::daemon
