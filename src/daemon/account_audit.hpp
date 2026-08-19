#pragma once

#include "daemon/account_audit_limits.hpp"
#include "daemon/logout_audit.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon_lock {
class LifetimeLease;
}

namespace tgcli::daemon {

enum class AccountAuditOperation {
    Send,
    MsgEdit,
    MsgDelete,
    MsgForward,
    MsgReact,
    MsgPin,
    MsgUnpin,
    ChatMarkRead,
    ChatMute,
    ChatUnmute,
    ChatPin,
    ChatUnpin,
    ChatArchive,
    ChatUnarchive,
    ChatJoin,
    ChatLeave,
    SavedAttach,
    SessionTerminate,
};

enum class AccountAuditStage {
    IdempotencyPending,
    SpoolReady,
    DispatchStarted,
    TemporaryIdsObserved,
    ForwardProgress,
    MutationConfirmed,
};

enum class AccountAuditMutationState { None, Possible, Confirmed };

enum class AccountAuditDurabilityReason {
    PathInvalid,
    WrongOwner,
    WrongType,
    WrongMode,
    WrongLinkCount,
    TooLarge,
    CapacityExhausted,
    OpenFailed,
    LockFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    DirectorySyncFailed,
    ParseError,
    SchemaError,
    Contradiction,
};

std::string_view account_audit_operation_name(AccountAuditOperation operation);
std::optional<AccountAuditOperation> parse_account_audit_operation(std::string_view value);
std::string_view account_audit_stage_name(AccountAuditStage stage);
std::optional<AccountAuditStage> parse_account_audit_stage(std::string_view value);
std::string_view account_audit_mutation_state_name(AccountAuditMutationState state);
std::string_view account_audit_durability_reason_name(AccountAuditDurabilityReason reason);

struct AccountAuditRecordIdentity {
    std::string invocation_id;
    std::string timestamp;
};

struct AccountAuditIntentInput {
    AccountAuditRecordIdentity identity;
    std::string account;
    AccountAuditOperation operation = AccountAuditOperation::Send;
    nlohmann::json arguments;
    nlohmann::json plan;
    std::string request_fingerprint;
    std::string config_snapshot;
    std::string authority_source;
    std::optional<std::string> confirmation_source;
    std::optional<std::string> idempotency_key_hash;
    std::uint64_t request_source_bytes = 0;
};

struct AccountAuditCheckpointInput {
    AccountAuditRecordIdentity identity;
    std::string account;
    AccountAuditOperation operation = AccountAuditOperation::Send;
    std::uint32_t checkpoint_sequence = 0;
    AccountAuditStage stage = AccountAuditStage::DispatchStarted;
    nlohmann::json data;
};

struct AccountAuditOutcomeInput {
    AccountAuditRecordIdentity identity;
    std::string account;
    AccountAuditOperation operation = AccountAuditOperation::Send;
    bool success = false;
    AccountAuditMutationState mutation_state = AccountAuditMutationState::None;
    std::vector<AccountAuditStage> completed_stages;
    nlohmann::json terminal;
};

class AccountAuditIntent final {
  public:
    [[nodiscard]] const nlohmann::json& document() const;

  private:
    explicit AccountAuditIntent(nlohmann::json document);
    nlohmann::json document_;
    friend std::optional<AccountAuditIntent>
    make_account_audit_intent(AccountAuditIntentInput input, std::string& error);
};

class AccountAuditCheckpoint final {
  public:
    [[nodiscard]] const nlohmann::json& document() const;

  private:
    explicit AccountAuditCheckpoint(nlohmann::json document);
    nlohmann::json document_;
    friend std::optional<AccountAuditCheckpoint>
    make_account_audit_checkpoint(AccountAuditCheckpointInput input, std::string& error);
};

class AccountAuditOutcome final {
  public:
    [[nodiscard]] const nlohmann::json& document() const;

  private:
    explicit AccountAuditOutcome(nlohmann::json document);
    nlohmann::json document_;
    friend std::optional<AccountAuditOutcome>
    make_account_audit_outcome(AccountAuditOutcomeInput input, std::string& error);
};

std::optional<AccountAuditIntent> make_account_audit_intent(AccountAuditIntentInput input,
                                                            std::string& error);
std::optional<AccountAuditCheckpoint>
make_account_audit_checkpoint(AccountAuditCheckpointInput input, std::string& error);
std::optional<AccountAuditOutcome> make_account_audit_outcome(AccountAuditOutcomeInput input,
                                                              std::string& error);
bool validate_account_audit_intent(const nlohmann::json& document, std::string& error);
bool validate_account_audit_checkpoint(const nlohmann::json& document, std::string& error);
bool validate_account_audit_outcome(const nlohmann::json& document, std::string& error);
bool validate_account_audit_stage_history(AccountAuditOperation operation,
                                          const std::vector<AccountAuditCheckpointInput>& history,
                                          std::string& error);
std::string serialize_account_audit_record(const nlohmann::json& document);

struct AccountAuditPin {
    std::uint64_t audit_generation = 0;
    std::string invocation_id;
    std::string request_fingerprint;
    AccountAuditOperation operation = AccountAuditOperation::Send;
};

struct KnownAccountAuditPins {
    std::vector<AccountAuditPin> pins;
};
struct UnavailableAccountAuditPins {
    AccountAuditDurabilityReason reason = AccountAuditDurabilityReason::ReadFailed;
};
struct AbsentAccountAuditPinsByPolicy {};
using AccountAuditPinSource = std::variant<KnownAccountAuditPins, UnavailableAccountAuditPins,
                                           AbsentAccountAuditPinsByPolicy>;

struct AccountAuditFailure {
    enum class Interruption { Deadline, Cancelled };

    AccountAuditFailure() = default;
    AccountAuditFailure(AccountAuditDurabilityReason reason_value, std::string detail_value,
                        std::optional<Interruption> interruption_value = std::nullopt)
        : reason(reason_value), detail(std::move(detail_value)), interruption(interruption_value) {}

    AccountAuditDurabilityReason reason = AccountAuditDurabilityReason::PathInvalid;
    std::string detail;
    std::optional<Interruption> interruption;
};

struct AccountAuditScanControl {
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    std::function<bool()> cancelled;
};

struct AccountAuditAppendReceipt {
    std::uint64_t audit_generation = 0;
    std::string invocation_id;
    std::string request_fingerprint;
    AccountAuditOperation operation = AccountAuditOperation::Send;
};

struct AccountAuditOpenGroup {
    nlohmann::json intent;
    std::vector<nlohmann::json> checkpoints;
    std::vector<AccountAuditStage> completed_stages;
    bool keyed = false;
    bool has_spool = false;
    bool dispatch_started = false;
    bool mutation_confirmed = false;
    bool forward_complete = false;
    bool any_forward_sent = false;
};

enum class AccountAuditInspectionStatus {
    Clean,
    LegacyOpen,
    Open,
    Contradiction,
    Unavailable,
    Interrupted,
};

struct AccountAuditInspection {
    AccountAuditInspectionStatus status = AccountAuditInspectionStatus::Clean;
    std::optional<IncompleteLogoutAudit> legacy_logout;
    std::optional<AccountAuditOpenGroup> oldest_open;
    std::optional<nlohmann::json> terminal;
    AccountAuditFailure failure;
};

enum class AccountAuditRecoveryBoundary {
    DeleteSpoolAndSyncRoot,
    AppendMutationProofAndSync,
    AppendOutcomeAndSync,
    TransitionStoreAndSync,
    CleanupSpoolAndSyncRoot,
};

struct AccountAuditRecoveryPlan {
    AccountAuditMutationState mutation_state = AccountAuditMutationState::None;
    nlohmann::json terminal;
    std::vector<AccountAuditRecoveryBoundary> boundaries;
    bool continue_current_request = false;
    bool retain_store = false;
    bool retain_spool = false;
};

std::optional<AccountAuditRecoveryPlan>
classify_account_audit_recovery(const AccountAuditOpenGroup& group, std::string_view account,
                                std::string_view audit_path, const AccountAuditPinSource& pins,
                                std::string& error);

enum class AccountAuditFault {
    Open,
    Read,
    Write,
    FileSync,
    DirectorySync,
    Unlink,
    Rename,
};

namespace testing {
struct AccountAuditHooks {
    std::function<bool(AccountAuditFault)> should_fail;
    std::function<void(std::string_view)> after_rotation_step;
    std::function<void()> before_identity_rescan;
    std::function<void()> after_parser_poll;
    std::function<void()> before_final_classification;
    std::uint64_t rotation_bytes = account_audit_limits::kRotationBytes;
};
} // namespace testing

class AccountAuditCoordinator final : public std::enable_shared_from_this<AccountAuditCoordinator> {
  public:
    class Guard final {
      public:
        Guard(Guard&&) noexcept = default;
        Guard& operator=(Guard&&) noexcept = default;
        ~Guard() = default;
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        [[nodiscard]] bool valid() const;
        [[nodiscard]] bool validate_lease(std::string& error) const;
        [[nodiscard]] bool validate_lease(std::string_view state_directory,
                                          std::string_view account, uid_t expected_uid,
                                          std::string& error) const;

      private:
        friend class AccountAuditCoordinator;
        friend class AccountAuditLog;
        Guard(std::unique_lock<std::mutex> lock,
              std::shared_ptr<const AccountAuditCoordinator> owner,
              AccountAuditScanControl scan_control);
        std::unique_lock<std::mutex> lock_;
        std::shared_ptr<const AccountAuditCoordinator> owner_;
        AccountAuditScanControl scan_control_;
    };

    static std::shared_ptr<AccountAuditCoordinator>
    create(std::string state_directory, std::string account, uid_t expected_uid,
           std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease, std::string& error);
    [[nodiscard]] Guard lock(AccountAuditScanControl scan_control = {});
    [[nodiscard]] bool validate_lease(std::string& error) const;

  private:
    AccountAuditCoordinator(std::string state_directory, std::string account, uid_t expected_uid,
                            std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease);

    std::string state_directory_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease_;
    mutable std::mutex mutex_;
};

class AccountAuditLog final {
  public:
    AccountAuditLog(std::string state_directory, std::string account, uid_t expected_uid,
                    std::shared_ptr<const testing::AccountAuditHooks> hooks = {});

    [[nodiscard]] const std::string& path() const;
    [[nodiscard]] AccountAuditInspection inspect(const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] bool append_intent(const AccountAuditIntent& intent,
                                     const AccountAuditPinSource& pins,
                                     const AccountAuditCoordinator::Guard& guard,
                                     AccountAuditAppendReceipt& receipt,
                                     AccountAuditFailure& failure) const;
    [[nodiscard]] bool append_checkpoint(const AccountAuditCheckpoint& checkpoint,
                                         const AccountAuditCoordinator::Guard& guard,
                                         AccountAuditFailure& failure) const;
    [[nodiscard]] bool append_outcome(const AccountAuditOutcome& outcome,
                                      const AccountAuditCoordinator::Guard& guard,
                                      AccountAuditFailure& failure) const;

  private:
    [[nodiscard]]
    AccountAuditInspection inspect_unfinalized(const AccountAuditCoordinator::Guard& guard) const;
    std::string state_directory_;
    std::string audit_path_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<const testing::AccountAuditHooks> hooks_;
};

} // namespace tgcli::daemon
