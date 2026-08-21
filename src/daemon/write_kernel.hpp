#pragma once

#include "common/config.hpp"
#include "common/deadline.hpp"
#include "common/invite_redaction.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/write_contract.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

enum class WriteKernelStatus {
    DryRunPlanned,
    Completed,
    Replayed,
    Pending,
    Conflict,
    Rejected,
    DurabilityFatal,
    AuditFatal,
};

struct WriteKernelResult {
    WriteKernelStatus status = WriteKernelStatus::Rejected;
    std::optional<nlohmann::json> terminal;
    std::optional<write_contract::Plan> plan;
};

struct WriteKernelRequest {
    proto::M3Operation operation = proto::M3Operation::Send;
    std::string account;
    std::optional<IdempotencyKeyHash> idempotency_key_hash;
    std::string invocation_id;
    std::string intent_timestamp;
    std::string config_path;
    std::string config_snapshot;
    AuthoritySource authority_source = AuthoritySource::Request;
    std::uint64_t request_source_bytes = 0;
    std::function<std::uint64_t()> sample_now;
    bool dry_run = false;
    RequestDeadline deadline;
    std::stop_token cancellation_token;
    std::function<bool()> cancelled;
};

struct WriteAdmission {
    write_contract::Arguments arguments;
    IdempotencyRequestFingerprint request_fingerprint;
    std::shared_ptr<PreparedSource> pass1_source;
    std::vector<redaction::CorrelatedInviteLink> invite_redactions;
};

using WriteAdmissionOutcome = std::variant<WriteAdmission, nlohmann::json>;
using WritePlanningOutcome = std::variant<write_contract::Plan, nlohmann::json>;

enum class WriteConfirmationStatus { ConfirmedYes, ConfirmedTty, Rejected, TimedOut, Cancelled };

struct WriteConfirmationOutcome {
    WriteConfirmationStatus status = WriteConfirmationStatus::Rejected;
    std::optional<nlohmann::json> terminal;
};

struct WritePostIntentPreparation {
    std::optional<SpoolRef> spool;
    std::optional<write_contract::StoredTerminal> terminal_without_dispatch;
};

struct WriteDispatchPreparation {
    nlohmann::json proof;
};

struct WriteDispatchStopped {};

using WriteDispatchAdmissionOutcome =
    std::variant<WriteDispatchPreparation, write_contract::StoredTerminal, WriteDispatchStopped>;

struct WriteDispatchOutcome {
    write_contract::StoredTerminal terminal;
    AccountAuditMutationState mutation_state = AccountAuditMutationState::None;
    bool mutation_confirmed = false;
};

class WriteDurableObservationSink final {
  public:
    ~WriteDurableObservationSink() = default;
    WriteDurableObservationSink(const WriteDurableObservationSink&) = delete;
    WriteDurableObservationSink& operator=(const WriteDurableObservationSink&) = delete;
    WriteDurableObservationSink(WriteDurableObservationSink&&) = delete;
    WriteDurableObservationSink& operator=(WriteDurableObservationSink&&) = delete;

    [[nodiscard]] bool temporary_message_ids(nlohmann::json ids) noexcept;
    [[nodiscard]] bool forward_progress(nlohmann::json items) noexcept;
    [[nodiscard]] bool durable() const noexcept;

  private:
    using Observer = std::function<bool(AccountAuditStage, nlohmann::json)>;
    explicit WriteDurableObservationSink(Observer observer) : observer_(std::move(observer)) {}

    [[nodiscard]] bool observe(AccountAuditStage stage, nlohmann::json value) noexcept;

    Observer observer_;
    bool durable_ = true;

    friend class WriteKernel;
};

struct WriteKernelHooks {
    std::function<WriteAdmissionOutcome()> admit;
    std::function<WritePlanningOutcome(const WriteAdmission&)> plan;
    std::function<WriteConfirmationOutcome(const write_contract::Plan&, bool replay)> confirm;
    std::function<config::GrantVerificationResult(std::string_view expected_identity,
                                                  std::string_view account,
                                                  const config::MutationControl&)>
        verify_config_grant;
    std::function<WritePostIntentPreparation(const write_contract::Plan&, const WriteAdmission&)>
        post_intent;
    std::function<void(const AccountAuditAppendReceipt&, const AccountAuditCoordinator::Guard&)>
        before_insert;
    std::function<AccountAuditSpoolCleanupCallResult(AccountAuditSpoolHold,
                                                     const AccountAuditCoordinator::Guard&)>
        cleanup_spool;
    std::function<WriteDispatchAdmissionOutcome(const write_contract::Plan&)>
        revalidate_auth_and_schedule;
    std::function<WriteDispatchOutcome(const write_contract::Plan&, const WriteDispatchPreparation&,
                                       WriteDurableObservationSink&)>
        dispatch;
    std::function<bool(const write_contract::Plan&, const WriteDispatchOutcome&)> cleanup;
    std::function<std::string()> timestamp;
    std::function<void()> audit_fatal_shutdown;
    std::shared_ptr<const testing::FileSpoolHooks> spool_hooks;
};

class WriteKernel final {
  public:
    explicit WriteKernel(std::shared_ptr<IdempotencyFoundation> foundation)
        : foundation_(std::move(foundation)) {}

    [[nodiscard]] WriteKernelResult run(const WriteKernelRequest& request,
                                        const WriteKernelHooks& hooks) const;

  private:
    std::shared_ptr<IdempotencyFoundation> foundation_;
};

} // namespace tgcli::daemon
