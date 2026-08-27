#pragma once

#include "common/deadline.hpp"
#include "daemon/account_audit_limits.hpp"
#include "daemon/file_spool.hpp"
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
    ContactAdd,
    ContactRemove,
    ContactBlock,
    ContactUnblock,
    FolderCreate,
    FolderEdit,
    FolderDelete,
    FolderAddChat,
    FolderRemoveChat,
    TopicCreate,
    TopicEdit,
    TopicClose,
    TopicReopen,
    ChatSetTitle,
    ChatSetPhoto,
    ChatSetDescription,
    ChatInviteLink,
    ChatPromote,
    ChatDemote,
    ChatBan,
    ChatUnban,
    ChatKick,
    ChatSetPermissions,
    StorageOptimize,
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
bool validate_account_audit_persisted_plan(AccountAuditOperation operation,
                                           const nlohmann::json& plan, std::string_view account);
bool validate_account_audit_persisted_arguments(AccountAuditOperation operation,
                                                const nlohmann::json& arguments);
bool validate_account_audit_persisted_result(AccountAuditOperation operation,
                                             const nlohmann::json& result);
bool validate_account_audit_persisted_stored_terminal(AccountAuditOperation operation,
                                                      const nlohmann::json& terminal);
bool validate_account_audit_persisted_terminal(AccountAuditOperation operation,
                                               const nlohmann::json& terminal,
                                               const nlohmann::json& plan,
                                               std::string_view account);
bool validate_account_audit_persisted_temporary_ids(AccountAuditOperation operation,
                                                    const nlohmann::json& temporary_ids,
                                                    const nlohmann::json& plan);
bool validate_account_audit_persisted_forward_progress(AccountAuditOperation operation,
                                                       const nlohmann::json& items,
                                                       const nlohmann::json& plan);
bool validate_account_audit_persisted_spool(const SpoolRef& spool, std::string_view invocation_id);
std::uint32_t account_audit_terminal_reservation(AccountAuditOperation operation);
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
    RequestDeadline deadline;
    std::function<bool()> cancelled;
};

class AccountAuditAppendPermit;
struct AccountAuditAppendReceipt;
class AccountAuditLog;
class AccountAuditRecoveryPermit;
struct AccountAuditSpoolHoldAccess;

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
        [[nodiscard]] bool interrupted(AccountAuditFailure& failure) const;
        [[nodiscard]] FileSpoolControl constrain_file_spool_control(FileSpoolControl control) const;

      private:
        friend class AccountAuditAppendPermit;
        friend class AccountAuditCoordinator;
        friend class AccountAuditLog;
        friend class AccountAuditRecoveryPermit;
        friend struct AccountAuditSpoolHoldAccess;
        [[nodiscard]] bool enter_post_intent_durability(const AccountAuditAppendReceipt& receipt,
                                                        AccountAuditFailure& failure);
        Guard(std::unique_lock<std::timed_mutex> lock,
              std::shared_ptr<const AccountAuditCoordinator> owner,
              AccountAuditScanControl scan_control);
        std::unique_lock<std::timed_mutex> lock_;
        std::shared_ptr<const AccountAuditCoordinator> owner_;
        AccountAuditScanControl scan_control_;
        bool post_intent_durability_ = false;
    };

    static std::shared_ptr<AccountAuditCoordinator>
    create(std::string state_directory, std::string account, uid_t expected_uid,
           std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease, std::string& error);
    using LockResult = std::variant<Guard, AccountAuditFailure>;
    [[nodiscard]] Guard lock();
    [[nodiscard]] LockResult lock(AccountAuditScanControl scan_control);
    [[nodiscard]] bool validate_lease(std::string& error) const;

  private:
    AccountAuditCoordinator(std::string state_directory, std::string account, uid_t expected_uid,
                            std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease);

    std::string state_directory_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<const daemon_lock::LifetimeLease> daemon_lock_lease_;
    mutable std::timed_mutex mutex_;
};

struct AccountAuditAppendReceipt {
    std::uint64_t audit_generation = 0;
    std::string invocation_id;
    std::string request_fingerprint;
    AccountAuditOperation operation = AccountAuditOperation::Send;

  private:
    std::shared_ptr<const AccountAuditCoordinator> coordinator_;
    friend class AccountAuditCoordinator::Guard;
    friend class AccountAuditLog;
};

struct AccountAuditOpenGroup {
    std::uint64_t audit_generation = 0;
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

struct AccountAuditCompletedGroupView {
    std::uint64_t audit_generation = 0;
    std::string invocation_id;
    std::string account;
    AccountAuditOperation operation = AccountAuditOperation::Send;
    std::string request_fingerprint;
    std::optional<std::string> idempotency_key_hash;
    nlohmann::json plan;
    std::string intent_timestamp;
    std::int64_t intent_unix_seconds = 0;
    std::optional<nlohmann::json> idempotency_pending;
    std::optional<SpoolRef> spool;
    nlohmann::json temporary_message_ids = nlohmann::json::array();
    nlohmann::json forward_progress = nlohmann::json::array();
    std::optional<nlohmann::json> mutation_proof;
    std::vector<AccountAuditStage> completed_stages;
    std::optional<nlohmann::json> outcome;
};

using AccountAuditCompletedGroupVisitor =
    std::function<void(const AccountAuditCompletedGroupView&)>;

class AccountAuditSpoolReleaseReceipt;

class AccountAuditSpoolHold final {
  public:
    AccountAuditSpoolHold(AccountAuditSpoolHold&& other) noexcept;
    AccountAuditSpoolHold& operator=(AccountAuditSpoolHold&& other) noexcept;
    ~AccountAuditSpoolHold() = default;
    AccountAuditSpoolHold(const AccountAuditSpoolHold&) = delete;
    AccountAuditSpoolHold& operator=(const AccountAuditSpoolHold&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::uint64_t audit_generation() const;
    [[nodiscard]] const std::string& invocation_id() const;
    [[nodiscard]] const SpoolRef& spool() const;

  private:
    AccountAuditSpoolHold(std::uint64_t permit_id, std::uint64_t hold_id,
                          std::string state_directory, std::string account, uid_t expected_uid,
                          std::shared_ptr<const AccountAuditCoordinator> coordinator,
                          std::uint64_t audit_generation, std::string invocation_id,
                          SpoolRef spool);
    void invalidate();

    std::uint64_t permit_id_ = 0;
    std::uint64_t hold_id_ = 0;
    std::string state_directory_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<const AccountAuditCoordinator> coordinator_;
    std::uint64_t audit_generation_ = 0;
    std::string invocation_id_;
    SpoolRef spool_;

    friend class AccountAuditAppendPermit;
    friend class AccountAuditRecoveryPermit;
    friend class AccountAuditLog;
    friend class AccountAuditSpoolReleaseReceipt;
    friend struct AccountAuditSpoolHoldAccess;
};

class AccountAuditSpoolReleaseReceipt final {
  public:
    AccountAuditSpoolReleaseReceipt(AccountAuditSpoolReleaseReceipt&& other) noexcept;
    AccountAuditSpoolReleaseReceipt& operator=(AccountAuditSpoolReleaseReceipt&& other) noexcept;
    ~AccountAuditSpoolReleaseReceipt() = default;
    AccountAuditSpoolReleaseReceipt(const AccountAuditSpoolReleaseReceipt&) = delete;
    AccountAuditSpoolReleaseReceipt& operator=(const AccountAuditSpoolReleaseReceipt&) = delete;

    [[nodiscard]] bool valid() const;

  private:
    explicit AccountAuditSpoolReleaseReceipt(AccountAuditSpoolHold hold);
    void invalidate();

    std::uint64_t permit_id_ = 0;
    std::uint64_t hold_id_ = 0;
    std::string state_directory_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<const AccountAuditCoordinator> coordinator_;
    std::uint64_t audit_generation_ = 0;
    std::string invocation_id_;
    SpoolRef spool_;

    friend class AccountAuditAppendPermit;
    friend class AccountAuditRecoveryPermit;
    friend class AccountAuditLog;
    friend struct AccountAuditSpoolHoldAccess;
};

using AccountAuditSpoolCleanupCallResult =
    std::variant<AccountAuditSpoolReleaseReceipt, FileSpoolError>;

[[nodiscard]] AccountAuditSpoolCleanupCallResult
cleanup_spool_file_with_hold(AccountAuditSpoolHold hold,
                             const AccountAuditCoordinator::Guard& guard,
                             const FileSpoolControl& control = {},
                             const std::shared_ptr<const testing::FileSpoolHooks>& hooks = {});

class AccountAuditAppendPermit final {
  public:
    AccountAuditAppendPermit();
    AccountAuditAppendPermit(AccountAuditAppendPermit&&) noexcept;
    AccountAuditAppendPermit& operator=(AccountAuditAppendPermit&&) noexcept;
    ~AccountAuditAppendPermit();
    AccountAuditAppendPermit(const AccountAuditAppendPermit&) = delete;
    AccountAuditAppendPermit& operator=(const AccountAuditAppendPermit&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::vector<AccountAuditSpoolHold> issue_spool_holds();
    [[nodiscard]] bool release_spool_hold(AccountAuditSpoolReleaseReceipt receipt,
                                          const AccountAuditCoordinator::Guard& guard,
                                          AccountAuditFailure& failure);
    [[nodiscard]] bool narrow_pins(std::vector<AccountAuditPin> surviving,
                                   AccountAuditFailure& failure);

  private:
    struct Impl;
    explicit AccountAuditAppendPermit(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;

    friend class AccountAuditLog;
};

class AccountAuditRecoveryPermit final {
  public:
    AccountAuditRecoveryPermit();
    AccountAuditRecoveryPermit(AccountAuditRecoveryPermit&&) noexcept;
    AccountAuditRecoveryPermit& operator=(AccountAuditRecoveryPermit&&) noexcept;
    ~AccountAuditRecoveryPermit();
    AccountAuditRecoveryPermit(const AccountAuditRecoveryPermit&) = delete;
    AccountAuditRecoveryPermit& operator=(const AccountAuditRecoveryPermit&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::vector<AccountAuditSpoolHold> issue_spool_holds();
    [[nodiscard]] bool release_spool_hold(AccountAuditSpoolReleaseReceipt receipt,
                                          const AccountAuditCoordinator::Guard& guard,
                                          AccountAuditFailure& failure);

  private:
    struct Impl;
    explicit AccountAuditRecoveryPermit(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;

    friend class AccountAuditLog;
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
    bool complete_store = false;
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
    std::function<void(std::string_view)> before_segment_scan;
    std::uint64_t rotation_bytes = account_audit_limits::kRotationBytes;
};
} // namespace testing

class AccountAuditLog final {
  public:
    AccountAuditLog(std::string state_directory, std::string account, uid_t expected_uid,
                    std::shared_ptr<const testing::AccountAuditHooks> hooks = {});

    [[nodiscard]] const std::string& path() const;
    [[nodiscard]] AccountAuditInspection inspect(const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] AccountAuditInspection
    prepare_append(const AccountAuditIntent& intent, const AccountAuditPinSource& pins,
                   const AccountAuditCoordinator::Guard& guard, AccountAuditAppendPermit& permit,
                   const AccountAuditCompletedGroupVisitor& completed_visitor = {}) const;
    [[nodiscard]] AccountAuditInspection
    prepare_recovery(const AccountAuditPinSource& pins, const AccountAuditCoordinator::Guard& guard,
                     AccountAuditRecoveryPermit& permit,
                     const AccountAuditCompletedGroupVisitor& completed_visitor = {}) const;
    [[nodiscard]] bool append_intent(const AccountAuditIntent& intent,
                                     AccountAuditAppendPermit permit,
                                     AccountAuditCoordinator::Guard& guard,
                                     AccountAuditAppendReceipt& receipt,
                                     AccountAuditFailure& failure) const;
    [[nodiscard]] bool append_checkpoint(const AccountAuditCheckpoint& checkpoint,
                                         const AccountAuditCoordinator::Guard& guard,
                                         AccountAuditFailure& failure) const;
    [[nodiscard]] bool append_outcome(const AccountAuditOutcome& outcome,
                                      const AccountAuditCoordinator::Guard& guard,
                                      AccountAuditFailure& failure) const;
    [[nodiscard]] std::optional<AccountAuditSpoolHold>
    hold_current_spool(const AccountAuditAppendReceipt& receipt, const SpoolRef& spool,
                       const AccountAuditCoordinator::Guard& guard,
                       AccountAuditFailure& failure) const;
    [[nodiscard]] bool release_current_spool(AccountAuditSpoolReleaseReceipt release,
                                             const AccountAuditAppendReceipt& receipt,
                                             const AccountAuditCoordinator::Guard& guard,
                                             AccountAuditFailure& failure) const;

  private:
    [[nodiscard]]
    AccountAuditInspection inspect_unfinalized(
        const AccountAuditCoordinator::Guard& guard, const AccountAuditPinSource* pins = nullptr,
        const AccountAuditCompletedGroupVisitor* completed_visitor = nullptr,
        AccountAuditAppendPermit* permit = nullptr, const AccountAuditIntent* next_intent = nullptr,
        AccountAuditRecoveryPermit* recovery_permit = nullptr) const;
    std::string state_directory_;
    std::string audit_path_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<const testing::AccountAuditHooks> hooks_;
};

} // namespace tgcli::daemon
