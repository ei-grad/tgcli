#pragma once

#include "proto/destructive_plan.hpp"
#include "proto/frame.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

enum class AuthoritySource { Request, Config };
enum class WriteDenialReason { ExplicitDeny, NoGrant, InvalidConfigGrant };

struct DryRunAuthority {
    friend bool operator==(const DryRunAuthority&, const DryRunAuthority&) = default;
};

struct GrantedAuthority {
    AuthoritySource source;
    friend bool operator==(const GrantedAuthority&, const GrantedAuthority&) = default;
};

struct DeniedAuthority {
    WriteDenialReason reason;
    friend bool operator==(const DeniedAuthority&, const DeniedAuthority&) = default;
};

using AuthorityDecision = std::variant<DryRunAuthority, GrantedAuthority, DeniedAuthority>;

struct ConfigWriteAuthority {
    bool grant_valid = false;
    bool allow_write = false;
};

AuthorityDecision evaluate_destructive_authority(const proto::RequestContext& request,
                                                 ConfigWriteAuthority config);
std::string_view authority_source_name(AuthoritySource source);
std::string_view write_denial_reason_name(WriteDenialReason reason);

enum class DestructiveCommand { Logout, AccountRemove };
enum class ConfirmationSource { Yes, Tty };
enum class MutationState { None, Possible, Confirmed };
enum class AccountRemoveRemoteResult { Confirmed, NotPresent, Kept };

enum class AuditStage {
    Planned,
    IntentSynced,
    LogoutSendStarted,
    LogoutClosedConfirmed,
    RemoteLogoutSendStarted,
    RemoteConfirmed,
    RemoteNotPresent,
    RemoteKept,
    ClientCloseStarted,
    ClientClosed,
    ConfigRemoveStarted,
    ConfigRemoved,
    DataRemoveStarted,
    DataRemoved,
    StateRemoveStarted,
    StateRemoved,
    OutcomeSynced,
};

std::string_view destructive_command_name(DestructiveCommand command);
std::string_view confirmation_source_name(ConfirmationSource source);
std::string_view mutation_state_name(MutationState state);
std::string_view account_remove_remote_result_name(AccountRemoveRemoteResult result);
std::string_view audit_stage_name(AuditStage stage);
std::optional<AuditStage> parse_audit_stage(std::string_view name);

bool validate_audit_stage_prefix(DestructiveCommand command,
                                 const std::vector<AuditStage>& completed_stages,
                                 std::string& error);
std::optional<MutationState> derive_mutation_state(DestructiveCommand command,
                                                   const std::vector<AuditStage>& completed_stages,
                                                   std::string& error);

struct AuditRecordIdentity {
    std::string invocation_id;
    std::string timestamp;
};

class StructuredOutcomeError final {
  public:
    [[nodiscard]] const std::string& code() const;
    [[nodiscard]] const nlohmann::json& details() const;

  private:
    StructuredOutcomeError(std::string code, nlohmann::json details);

    std::string code_;
    nlohmann::json details_;

    friend std::optional<StructuredOutcomeError>
    parse_structured_outcome_error(const nlohmann::json& value, std::string& error);
};

std::optional<StructuredOutcomeError> parse_structured_outcome_error(const nlohmann::json& value,
                                                                     std::string& error);
nlohmann::json serialize(const StructuredOutcomeError& error);

namespace detail {
struct DestructiveContractAccess;
}

class AuditIntent final {
  private:
    explicit AuditIntent(nlohmann::json document);
    nlohmann::json document_;

    friend struct detail::DestructiveContractAccess;
    friend nlohmann::json serialize(const AuditIntent& record);
};

class AuditCheckpoint final {
  private:
    explicit AuditCheckpoint(nlohmann::json document);
    nlohmann::json document_;

    friend struct detail::DestructiveContractAccess;
    friend nlohmann::json serialize(const AuditCheckpoint& record);
};

class AuditOutcome final {
  private:
    explicit AuditOutcome(nlohmann::json document);
    nlohmann::json document_;

    friend struct detail::DestructiveContractAccess;
    friend nlohmann::json serialize(const AuditOutcome& record);
};

std::optional<AuditIntent>
make_logout_audit_intent(AuditRecordIdentity identity, const proto::LogoutPlan& plan,
                         std::string config_snapshot, AuthoritySource authority_source,
                         ConfirmationSource confirmation_source, std::string& error);
std::optional<AuditIntent> make_account_remove_audit_intent(AuditRecordIdentity identity,
                                                            const proto::AccountRemovePlan& plan,
                                                            AuthoritySource authority_source,
                                                            ConfirmationSource confirmation_source,
                                                            std::string& error);

std::optional<AuditCheckpoint> make_logout_audit_checkpoint(AuditRecordIdentity identity,
                                                            const proto::LogoutPlan& plan,
                                                            AuditStage stage, std::string& error);

std::optional<AuditOutcome>
make_logout_success_audit_outcome(AuditRecordIdentity identity, const proto::LogoutPlan& plan,
                                  const std::vector<AuditStage>& completed_stages,
                                  std::string& error);
std::optional<AuditOutcome> make_account_remove_success_audit_outcome(
    AuditRecordIdentity identity, const proto::AccountRemovePlan& plan,
    AccountRemoveRemoteResult remote_result, const std::vector<AuditStage>& completed_stages,
    std::string& error);
std::optional<AuditOutcome>
make_failure_audit_outcome(AuditRecordIdentity identity, const proto::DestructivePlan& plan,
                           const std::vector<AuditStage>& completed_stages,
                           const StructuredOutcomeError& outcome_error, std::string& error);

nlohmann::json serialize(const AuditIntent& record);
nlohmann::json serialize(const AuditCheckpoint& record);
nlohmann::json serialize(const AuditOutcome& record);

} // namespace tgcli::daemon
