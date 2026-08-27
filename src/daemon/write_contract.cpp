#include "daemon/write_contract.hpp"

#include "common/paths.hpp"
#include "daemon/account_audit.hpp"

#include <utility>

namespace tgcli::daemon::write_contract {

namespace {

std::optional<AccountAuditOperation> audit_operation(WriteOperation operation) {
    return operation.audit();
}

} // namespace

Arguments::Arguments(WriteOperation operation, nlohmann::json value)
    : operation_(operation), value_(std::move(value)) {}

WriteOperation Arguments::operation() const noexcept {
    return operation_;
}

const nlohmann::json& Arguments::value() const noexcept {
    return value_;
}

Plan::Plan(WriteOperation operation, std::string account, nlohmann::json value)
    : operation_(operation), account_(std::move(account)), value_(std::move(value)) {}

WriteOperation Plan::operation() const noexcept {
    return operation_;
}

const std::string& Plan::account() const noexcept {
    return account_;
}

const nlohmann::json& Plan::value() const noexcept {
    return value_;
}

Result::Result(WriteOperation operation, nlohmann::json value)
    : operation_(operation), value_(std::move(value)) {}

WriteOperation Result::operation() const noexcept {
    return operation_;
}

const nlohmann::json& Result::value() const noexcept {
    return value_;
}

StoredTerminal::StoredTerminal(WriteOperation operation, nlohmann::json value)
    : operation_(operation), value_(std::move(value)) {}

WriteOperation StoredTerminal::operation() const noexcept {
    return operation_;
}

const nlohmann::json& StoredTerminal::value() const noexcept {
    return value_;
}

bool StoredTerminal::success() const noexcept {
    return value_.is_object() && value_.value("kind", std::string{}) == "result";
}

std::optional<Arguments> make_arguments(WriteOperation operation, nlohmann::json value,
                                        std::string& error) {
    const auto audit = audit_operation(operation);
    if (!audit || !validate_account_audit_persisted_arguments(*audit, value)) {
        error = "write arguments do not match their M3 operation";
        return std::nullopt;
    }
    error.clear();
    return Arguments(operation, std::move(value));
}

std::optional<Plan> make_plan(WriteOperation operation, std::string account, nlohmann::json value,
                              std::string& error) {
    const auto audit = audit_operation(operation);
    if (!audit || !paths::valid_account_name(account) ||
        !validate_account_audit_persisted_plan(*audit, value, account)) {
        error = "write plan does not match its M3 operation and account";
        return std::nullopt;
    }
    error.clear();
    return Plan(operation, std::move(account), std::move(value));
}

std::optional<Result> make_result(WriteOperation operation, nlohmann::json value,
                                  std::string& error) {
    const auto audit = audit_operation(operation);
    if (!audit || !validate_account_audit_persisted_result(*audit, value)) {
        error = "write result does not match its M3 operation";
        return std::nullopt;
    }
    error.clear();
    return Result(operation, std::move(value));
}

std::optional<StoredTerminal> make_stored_terminal(WriteOperation operation, nlohmann::json value,
                                                   std::string& error) {
    const auto audit = audit_operation(operation);
    if (!audit || !validate_account_audit_persisted_stored_terminal(*audit, value)) {
        error = "stored terminal does not match its M3 operation";
        return std::nullopt;
    }
    error.clear();
    return StoredTerminal(operation, std::move(value));
}

std::optional<StoredTerminal> make_result_terminal(const Result& result, std::string& error) {
    return make_stored_terminal(result.operation(), {{"kind", "result"}, {"data", result.value()}},
                                error);
}

std::optional<StoredTerminal> make_error_terminal(WriteOperation operation, std::string code,
                                                  std::string message, nlohmann::json details,
                                                  int exit_code, std::string& error) {
    return make_stored_terminal(operation,
                                {{"kind", "error"},
                                 {"code", std::move(code)},
                                 {"message", std::move(message)},
                                 {"details", std::move(details)},
                                 {"exit_code", exit_code}},
                                error);
}

bool terminal_matches_plan(const StoredTerminal& terminal, const Plan& plan) {
    const auto audit = audit_operation(plan.operation());
    return terminal.operation() == plan.operation() && audit &&
           validate_account_audit_persisted_terminal(*audit, terminal.value(), plan.value(),
                                                     plan.account());
}

} // namespace tgcli::daemon::write_contract
