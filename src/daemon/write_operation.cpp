#include "daemon/write_operation.hpp"

#include "daemon/m6_write_policy.hpp"

namespace tgcli::daemon {

namespace {

std::optional<AccountAuditOperation> audit_from_name(std::string_view name) noexcept {
    return parse_account_audit_operation(name);
}

std::optional<AccountAuditOperation> audit_for(proto::M3Operation operation) noexcept {
    const auto* identity = proto::m3_operation_identity(operation);
    return identity == nullptr ? std::nullopt : audit_from_name(identity->canonical_name);
}

std::optional<AccountAuditOperation> audit_for(proto::M6Operation operation) noexcept {
    const auto* identity = proto::m6_operation_identity(operation);
    return identity == nullptr || !identity->mutation ? std::nullopt
                                                      : audit_from_name(identity->canonical_name);
}

std::optional<proto::M6Operation> m6(AccountAuditOperation operation) noexcept {
    const auto parsed = proto::parse_m6_operation(account_audit_operation_name(operation));
    return parsed && m6_write_policy(*parsed) != nullptr ? parsed : std::nullopt;
}

} // namespace

WriteOperation::WriteOperation() noexcept : operation_(audit_for(proto::M3Operation::Send)) {}

WriteOperation::WriteOperation(proto::M3Operation operation) noexcept
    : operation_(audit_for(operation)) {}

WriteOperation::WriteOperation(proto::M6Operation operation) noexcept
    : operation_(audit_for(operation)) {}

WriteOperation::WriteOperation(AccountAuditOperation operation) noexcept : operation_(operation) {}

std::optional<AccountAuditOperation> WriteOperation::audit() const noexcept {
    return operation_;
}

std::string_view WriteOperation::name() const noexcept {
    return operation_ ? account_audit_operation_name(*operation_) : std::string_view{};
}

bool WriteOperation::destructive() const noexcept {
    if (!operation_) {
        return false;
    }
    if (*operation_ == AccountAuditOperation::SessionTerminate ||
        *operation_ == AccountAuditOperation::MsgDelete ||
        *operation_ == AccountAuditOperation::ChatLeave) {
        return true;
    }
    const auto operation = m6(*operation_);
    const auto* identity = operation ? proto::m6_operation_identity(*operation) : nullptr;
    return identity != nullptr && identity->tier == proto::M6Tier::Destructive;
}

bool WriteOperation::idempotent() const noexcept {
    if (!operation_) {
        return false;
    }
    const auto operation = m6(*operation_);
    const auto* policy = operation ? m6_write_policy(*operation) : nullptr;
    return policy == nullptr ? *operation_ != AccountAuditOperation::SessionTerminate
                             : policy->idempotent;
}

bool WriteOperation::uses_photo_spool() const noexcept {
    if (!operation_) {
        return false;
    }
    const auto operation = m6(*operation_);
    const auto* policy = operation ? m6_write_policy(*operation) : nullptr;
    return policy != nullptr && policy->uses_photo_spool;
}

WriteOperation::operator bool() const noexcept {
    return operation_.has_value();
}

bool operator==(const WriteOperation& left, proto::M3Operation right) noexcept {
    return left.audit() == audit_for(right);
}

bool operator==(const WriteOperation& left, proto::M6Operation right) noexcept {
    return left.audit() == audit_for(right);
}

} // namespace tgcli::daemon
