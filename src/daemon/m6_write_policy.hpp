#pragma once

#include "proto/operation.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace tgcli::daemon {

struct M6WritePolicy {
    proto::M6Operation operation;
    std::string_view audit_name;
    std::array<std::string_view, 2> tdlib_functions;
    std::size_t tdlib_function_count;
    bool idempotent;
    bool uses_photo_spool;
};

std::span<const M6WritePolicy> m6_write_policies() noexcept;
const M6WritePolicy* m6_write_policy(proto::M6Operation operation) noexcept;
std::optional<proto::M6Operation> parse_m6_write_operation(std::string_view audit_name) noexcept;
bool valid_m6_tdlib_function(proto::M6Operation operation, std::string_view function) noexcept;

} // namespace tgcli::daemon
