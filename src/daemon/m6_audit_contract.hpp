#pragma once

#include "proto/operation.hpp"

#include <string_view>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

bool valid_m6_audit_arguments(proto::M6Operation operation, const nlohmann::json& value);
bool valid_m6_audit_plan(proto::M6Operation operation, const nlohmann::json& value,
                         std::string_view account);
bool valid_m6_audit_result(proto::M6Operation operation, const nlohmann::json& value);
bool m6_result_matches_plan(proto::M6Operation operation, const nlohmann::json& result,
                            const nlohmann::json& plan);
bool m6_arguments_match_plan(proto::M6Operation operation, const nlohmann::json& arguments,
                             const nlohmann::json& plan);

} // namespace tgcli::daemon
