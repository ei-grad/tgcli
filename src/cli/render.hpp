#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace tgcli::cli {

// Human rendering of a command's curated result data (client-side, DESIGN.md
// §7). The same data the --json mode prints; no information asymmetry.
std::string render_human(const std::string& command_key, const nlohmann::json& data);

} // namespace tgcli::cli
