#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <variant>
#include <vector>

namespace tgcli::daemon {

struct FileSnapshot {
    std::string path;
    std::string name;
    std::uint64_t size{};
    std::string sha256;
    std::uint64_t device{};
    std::uint64_t inode{};
    std::int64_t mtime_ns{};
    std::int64_t ctime_ns{};

    friend bool operator==(const FileSnapshot&, const FileSnapshot&) = default;
};

struct SpoolRef {
    std::string relative_path;
    FileSnapshot file;

    friend bool operator==(const SpoolRef&, const SpoolRef&) = default;
};

struct FilesystemDiagnosticPath {
    std::string bytes_hex;

    friend bool operator==(const FilesystemDiagnosticPath&,
                           const FilesystemDiagnosticPath&) = default;
};

enum class SourceFileReason { Missing, Symlink, WrongType, Empty, Unreadable };

enum class DurabilityReason {
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

enum class FileSpoolErrorKind {
    InvalidInput,
    SourceUnavailable,
    InputChanged,
    DurabilityFailure,
    TimedOut,
    Cancelled,
    Contradiction,
};

struct FileSpoolError {
    FileSpoolErrorKind kind{FileSpoolErrorKind::DurabilityFailure};
    std::optional<SourceFileReason> source_reason;
    std::optional<DurabilityReason> durability_reason;
    std::optional<SpoolRef> cleanup_reference;
    std::optional<FilesystemDiagnosticPath> diagnostic_path;

    friend bool operator==(const FileSpoolError&, const FileSpoolError&) = default;
};

struct FileSpoolControl {
    FileSpoolControl() = default;
    FileSpoolControl(const std::optional<std::chrono::steady_clock::time_point>& deadline_value,
                     std::stop_token stop_token_value, std::function<bool()> cancelled_value = {})
        : deadline(deadline_value), stop_token(std::move(stop_token_value)),
          cancelled(std::move(cancelled_value)) {}

    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::stop_token stop_token;
    std::function<bool()> cancelled;
};

enum class FileSpoolStage {
    BeforeCwdCapture,
    AfterCwdCapture,
    BeforeSourceEntryStat,
    AfterSourceEntryStat,
    AfterSourceOpen,
    DuringPass1Read,
    DuringPass2Read,
    DuringDestinationWrite,
    AfterSourceRead,
    BeforeSourceRevalidate,
    BeforeAccountStateOpen,
    AfterAccountStateOpen,
    BeforeRootInspect,
    AfterRootEntryStat,
    AfterRootOpen,
    BeforeRootCreate,
    AfterRootCreate,
    BeforeAccountStateSync,
    BeforeRootEnumeration,
    DuringRootEnumeration,
    BeforeInvocationCreate,
    AfterInvocationCreate,
    BeforeRootSync,
    BeforeDestinationCreate,
    AfterDestinationCreate,
    BeforeDestinationRevalidate,
    BeforeFileSync,
    BeforeInvocationSync,
    BeforeCleanupOpen,
    BeforeCleanupUnlink,
    BeforeCleanupInvocationRemove,
    BeforeCleanupRootSync,
};

enum class FileSpoolIo { Pass1Read, Pass2Read, DestinationReadback, DestinationWrite };

enum class FileSpoolMetadata {
    CwdEntry,
    CwdDescriptor,
    DirectoryEntry,
    DirectoryDescriptor,
    SourceEntry,
    SourceBefore,
    SourceAfter,
    RootEntry,
    RootDescriptor,
    InvocationEntry,
    InvocationDescriptor,
    DestinationEntry,
    DestinationDescriptor,
};

namespace testing {

struct FileSpoolHooks {
    std::function<void(FileSpoolStage)> at_stage;
    std::function<bool(FileSpoolStage)> should_fail;
    std::function<ssize_t(FileSpoolIo, int, void*, std::size_t)> read;
    std::function<ssize_t(int, const void*, std::size_t)> write;
    std::function<int(FileSpoolStage, int)> sync;
    std::function<long(int)> name_max;
    std::function<void(FileSpoolMetadata, struct stat&)> mutate_metadata;
};

} // namespace testing

struct CreatedSpool;

class PreparedSource final {
  public:
    PreparedSource(PreparedSource&&) noexcept;
    PreparedSource& operator=(PreparedSource&&) noexcept;
    ~PreparedSource();

    PreparedSource(const PreparedSource&) = delete;
    PreparedSource& operator=(const PreparedSource&) = delete;

    [[nodiscard]] const FileSnapshot& snapshot() const;

  private:
    struct Impl;
    explicit PreparedSource(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;

    friend std::variant<PreparedSource, FileSpoolError>
    prepare_spool_source(std::string_view caller_path, std::string_view frozen_cwd,
                         const FileSpoolControl& control,
                         const std::shared_ptr<const testing::FileSpoolHooks>& hooks);
    friend std::variant<CreatedSpool, FileSpoolError>
    create_spool_file(PreparedSource& source, std::string account_state,
                      std::string_view invocation_id, uid_t expected_uid,
                      const FileSpoolControl& control,
                      const std::shared_ptr<const testing::FileSpoolHooks>& hooks);
};

using PrepareSpoolSourceResult = std::variant<PreparedSource, FileSpoolError>;

[[nodiscard]] PrepareSpoolSourceResult
prepare_spool_source(std::string_view caller_path, std::string_view frozen_cwd,
                     const FileSpoolControl& control = {},
                     const std::shared_ptr<const testing::FileSpoolHooks>& hooks = {});

[[nodiscard]] std::optional<std::string> canonical_source_display_path(std::string_view caller_path,
                                                                       std::string_view frozen_cwd);

struct CreatedSpool {
    SpoolRef reference;
    std::string local_path;

    friend bool operator==(const CreatedSpool&, const CreatedSpool&) = default;
};

using CreateSpoolFileResult = std::variant<CreatedSpool, FileSpoolError>;

[[nodiscard]] CreateSpoolFileResult
create_spool_file(PreparedSource& source, std::string account_state, std::string_view invocation_id,
                  uid_t expected_uid, const FileSpoolControl& control = {},
                  const std::shared_ptr<const testing::FileSpoolHooks>& hooks = {});

enum class SpoolRootState { Absent, Safe, Unsafe, IoFailure };

struct SpoolRootInspection {
    SpoolRootState state{SpoolRootState::IoFailure};
    std::optional<DurabilityReason> reason;

    friend bool operator==(const SpoolRootInspection&, const SpoolRootInspection&) = default;
};

using SpoolRootInspectionResult = std::variant<SpoolRootInspection, FileSpoolError>;

[[nodiscard]] SpoolRootInspectionResult
inspect_spool_root(std::string account_state, uid_t expected_uid,
                   const FileSpoolControl& control = {},
                   const std::shared_ptr<const testing::FileSpoolHooks>& hooks = {});

struct SpoolInvocationObservation {
    struct File {
        std::string name;
        FilesystemDiagnosticPath path;

        friend bool operator==(const File&, const File&) = default;
    };

    std::string invocation_id;
    std::optional<std::string> file_name;
    FilesystemDiagnosticPath directory_path;
    std::optional<FilesystemDiagnosticPath> file_path;
    std::vector<File> files;

    friend bool operator==(const SpoolInvocationObservation&,
                           const SpoolInvocationObservation&) = default;
};

struct SpoolInventory {
    bool root_absent{false};
    std::vector<SpoolInvocationObservation> invocations;
    std::optional<FilesystemDiagnosticPath> contradiction;

    friend bool operator==(const SpoolInventory&, const SpoolInventory&) = default;
};

using SpoolInventoryResult = std::variant<SpoolInventory, FileSpoolError>;

[[nodiscard]] SpoolInventoryResult
enumerate_spool(std::string account_state, uid_t expected_uid, const FileSpoolControl& control = {},
                const std::shared_ptr<const testing::FileSpoolHooks>& hooks = {});

struct ExpectedSpoolObject {
    std::string invocation_id;
    std::string file_name;

    friend bool operator==(const ExpectedSpoolObject&, const ExpectedSpoolObject&) = default;
};

struct SpoolReconciliation {
    std::vector<std::string> ready_invocations;
    std::vector<std::string> incomplete_invocations;
    std::vector<ExpectedSpoolObject> missing;
    std::optional<FilesystemDiagnosticPath> contradiction;

    friend bool operator==(const SpoolReconciliation&, const SpoolReconciliation&) = default;
};

using SpoolReconciliationResult = std::variant<SpoolReconciliation, FileSpoolError>;

[[nodiscard]] SpoolReconciliationResult
reconcile_spool_inventory(const SpoolInventory& inventory,
                          std::vector<ExpectedSpoolObject> expected);

struct SpoolCleanupResult {
    bool removed{false};
    bool root_synced{false};

    friend bool operator==(const SpoolCleanupResult&, const SpoolCleanupResult&) = default;
};

using SpoolCleanupCallResult = std::variant<SpoolCleanupResult, FileSpoolError>;

[[nodiscard]] SpoolCleanupCallResult
cleanup_spool_file(std::string_view account_state, const SpoolRef& reference, uid_t expected_uid,
                   const FileSpoolControl& control = {},
                   const std::shared_ptr<const testing::FileSpoolHooks>& hooks = {});

[[nodiscard]] std::optional<FilesystemDiagnosticPath>
encode_filesystem_diagnostic_path(std::string_view absolute_path_bytes);

[[nodiscard]] bool valid_filesystem_diagnostic_path(const FilesystemDiagnosticPath& value);
[[nodiscard]] bool valid_spool_reference(const SpoolRef& reference,
                                         std::string_view invocation_id = {});

} // namespace tgcli::daemon
