#pragma once

#include "daemon/account_audit.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

inline constexpr std::uint64_t kIdempotencyStoreMaximumBytes = 16ULL * 1024 * 1024;
inline constexpr std::size_t kIdempotencyStoreMaximumEntries = 10'000;
inline constexpr std::uint64_t kIdempotencyRetentionSeconds = 604'800;
inline constexpr std::uint64_t kIdempotencyMaximumUnixSeconds = 253'402'300'799ULL;

class IdempotencyKeyHash final {
  public:
    [[nodiscard]] const std::string& value() const;
    friend bool operator==(const IdempotencyKeyHash&, const IdempotencyKeyHash&) = default;

  private:
    explicit IdempotencyKeyHash(std::string value);
    std::string value_;
    friend std::optional<IdempotencyKeyHash> parse_idempotency_key_hash(std::string value);
};

class IdempotencyRequestFingerprint final {
  public:
    [[nodiscard]] const std::string& value() const;
    friend bool operator==(const IdempotencyRequestFingerprint&,
                           const IdempotencyRequestFingerprint&) = default;

  private:
    explicit IdempotencyRequestFingerprint(std::string value);
    std::string value_;
    friend std::optional<IdempotencyRequestFingerprint>
    parse_idempotency_request_fingerprint(std::string value);
};

[[nodiscard]] std::optional<IdempotencyKeyHash> parse_idempotency_key_hash(std::string value);
[[nodiscard]] std::optional<IdempotencyRequestFingerprint>
parse_idempotency_request_fingerprint(std::string value);

enum class IdempotencyEntryState { Pending, Completed };

struct IdempotencyEntry {
    IdempotencyKeyHash key_hash;
    IdempotencyRequestFingerprint request_fingerprint;
    AccountAuditOperation operation = AccountAuditOperation::Send;
    IdempotencyEntryState state = IdempotencyEntryState::Pending;
    std::string invocation_id;
    std::uint64_t audit_generation = 0;
    std::uint64_t created_at = 0;
    std::uint64_t expires_at = 0;
    std::uint32_t reserved_terminal_bytes = 0;
    nlohmann::json plan;
    nlohmann::json temporary_message_ids = nlohmann::json::array();
    nlohmann::json forward_progress = nlohmann::json::array();
    std::optional<SpoolRef> spool;
    std::optional<nlohmann::json> terminal;

    friend bool operator==(const IdempotencyEntry&, const IdempotencyEntry&) = default;
};

struct IdempotencyPendingInput {
    IdempotencyKeyHash key_hash;
    IdempotencyRequestFingerprint request_fingerprint;
    AccountAuditOperation operation = AccountAuditOperation::Send;
    std::string invocation_id;
    std::uint64_t audit_generation = 0;
    std::uint64_t created_at = 0;
    nlohmann::json plan;
};

struct IdempotencyFailure {
    AccountAuditDurabilityReason reason = AccountAuditDurabilityReason::PathInvalid;
    std::string account;
    std::string path;
    std::string detail;
    std::optional<AccountAuditFailure::Interruption> interruption;
};

using IdempotencyEntryResult = std::variant<IdempotencyEntry, IdempotencyFailure>;

[[nodiscard]] IdempotencyEntryResult make_idempotency_pending_entry(IdempotencyPendingInput input,
                                                                    std::string_view account,
                                                                    std::string_view store_path);

struct IdempotencySnapshot {
    std::vector<IdempotencyEntry> entries;
    std::string canonical_bytes;

    friend bool operator==(const IdempotencySnapshot&, const IdempotencySnapshot&) = default;
};

enum class IdempotencyInspectionStatus { Clean, Unavailable, Interrupted };

struct IdempotencyStoreInspection {
    IdempotencyInspectionStatus status = IdempotencyInspectionStatus::Unavailable;
    IdempotencySnapshot snapshot;
    bool stale_temp_present = false;
    IdempotencyFailure failure;
};

enum class IdempotencyLookupStatus { Miss, Pending, Completed, Conflict };

struct IdempotencyLookup {
    IdempotencyLookupStatus status = IdempotencyLookupStatus::Miss;
    const IdempotencyEntry* incumbent = nullptr;
};

enum class IdempotencyInsertStatus { Inserted, UnexpectedIncumbent, Failed };

struct IdempotencyInsertResult {
    IdempotencyInsertStatus status = IdempotencyInsertStatus::Failed;
    std::optional<IdempotencyEntry> incumbent;
    IdempotencySnapshot snapshot;
    IdempotencyFailure failure;
};

enum class IdempotencySpoolPreflightStatus { Ready, Failed };

struct IdempotencySpoolPreflightResult {
    IdempotencySpoolPreflightStatus status = IdempotencySpoolPreflightStatus::Failed;
    IdempotencySnapshot prospective_snapshot;
    IdempotencyFailure failure;
};

enum class IdempotencyWriteStatus { Applied, Unchanged, IncumbentPreserved, Failed };

struct IdempotencyWriteResult {
    IdempotencyWriteStatus status = IdempotencyWriteStatus::Failed;
    IdempotencySnapshot snapshot;
    IdempotencyFailure failure;
};

struct IdempotencySweepResult {
    IdempotencyWriteStatus status = IdempotencyWriteStatus::Failed;
    IdempotencySnapshot snapshot;
    std::vector<IdempotencyEntry> removed;
    IdempotencyFailure failure;
};

enum class IdempotencyStoreFault {
    Open,
    Read,
    Write,
    FileSync,
    DirectorySync,
    Unlink,
    Rename,
};

enum class IdempotencyStoreStage {
    BeforeStateOpen,
    AfterStateOpen,
    BeforeFinalInspect,
    AfterFinalOpen,
    DuringFinalRead,
    BeforeTempInspect,
    BeforeTempUnlink,
    BeforeTempCleanupDirectorySync,
    BeforeCapacity,
    BeforeTempCreate,
    AfterTempCreate,
    DuringTempWrite,
    BeforeTempFileSync,
    BeforeTempRevalidate,
    BeforeFinalRevalidate,
    BeforeRename,
    AfterRename,
    BeforeFinalNameRevalidate,
    BeforeDirectorySync,
};

enum class IdempotencyStoreMetadata {
    StateEntry,
    StateDescriptor,
    FinalEntry,
    FinalDescriptor,
    TempEntry,
    TempDescriptor
};

namespace testing {

struct IdempotencyStoreHooks {
    std::function<void(IdempotencyStoreStage)> at_stage;
    std::function<bool(IdempotencyStoreFault)> should_fail;
    std::function<ssize_t(int, void*, std::size_t)> read;
    std::function<ssize_t(int, const void*, std::size_t)> write;
    std::function<int(IdempotencyStoreStage, int)> sync;
    std::function<void(IdempotencyStoreMetadata, struct stat&)> mutate_metadata;
};

} // namespace testing

class IdempotencyStore final {
  public:
    static std::variant<IdempotencyStore, IdempotencyFailure>
    create(std::string state_directory, std::string account, uid_t expected_uid,
           std::shared_ptr<const testing::IdempotencyStoreHooks> hooks = {});

    [[nodiscard]] const std::string& path() const;
    [[nodiscard]] const std::string& account() const;
    [[nodiscard]] IdempotencyStoreInspection
    inspect(const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    cleanup_stale_temp(const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] static KnownAccountAuditPins pins(const IdempotencySnapshot& snapshot);
    [[nodiscard]] static IdempotencyLookup lookup(const IdempotencySnapshot& snapshot,
                                                  const IdempotencyKeyHash& key_hash,
                                                  const IdempotencyRequestFingerprint& fingerprint);
    [[nodiscard]] IdempotencyInsertResult
    insert_if_absent(const IdempotencyEntry& entry,
                     const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencySpoolPreflightResult
    preflight_spool_update(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                           const SpoolRef& spool,
                           const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    update_spool(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                 const SpoolRef& spool, const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    update_temporary_message_ids(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                                 nlohmann::json temporary_ids,
                                 const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    update_forward_progress(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                            nlohmann::json items,
                            const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    complete(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
             nlohmann::json terminal, const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    remove_owned(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                 const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    clear_spool(const IdempotencyKeyHash& key_hash, std::string_view invocation_id,
                const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencySweepResult
    sweep_expired(std::uint64_t sampled_now, const AccountAuditCoordinator::Guard& guard) const;
    [[nodiscard]] IdempotencyWriteResult
    apply_reconciled_snapshot(const IdempotencySnapshot& expected, IdempotencySnapshot desired,
                              const AccountAuditCoordinator::Guard& guard) const;

  private:
    IdempotencyStore(std::string state_directory, std::string account, uid_t expected_uid,
                     std::shared_ptr<const testing::IdempotencyStoreHooks> hooks);

    std::string state_directory_;
    std::string store_path_;
    std::string account_;
    uid_t expected_uid_ = 0;
    std::shared_ptr<const testing::IdempotencyStoreHooks> hooks_;
};

[[nodiscard]] std::variant<std::string, IdempotencyFailure>
serialize_idempotency_snapshot(const IdempotencySnapshot& snapshot, std::string_view account,
                               std::string_view store_path);
[[nodiscard]] nlohmann::json idempotency_unavailable_terminal(const IdempotencyFailure& failure);

} // namespace tgcli::daemon
