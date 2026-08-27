#pragma once

#include "daemon/account_audit.hpp"
#include "proto/operation.hpp"

#include <optional>
#include <string_view>

namespace tgcli::daemon {

class WriteOperation final {
  public:
    WriteOperation() noexcept;
    WriteOperation(proto::M3Operation operation) noexcept;
    WriteOperation(proto::M6Operation operation) noexcept;
    explicit WriteOperation(AccountAuditOperation operation) noexcept;

    [[nodiscard]] std::optional<AccountAuditOperation> audit() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] bool destructive() const noexcept;
    [[nodiscard]] bool idempotent() const noexcept;
    [[nodiscard]] bool uses_photo_spool() const noexcept;
    explicit operator bool() const noexcept;

    friend bool operator==(const WriteOperation&, const WriteOperation&) = default;
    friend bool operator==(const WriteOperation& left, proto::M3Operation right) noexcept;
    friend bool operator==(const WriteOperation& left, proto::M6Operation right) noexcept;

  private:
    std::optional<AccountAuditOperation> operation_;
};

} // namespace tgcli::daemon
