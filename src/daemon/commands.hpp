#pragma once

#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"

#include <span>
#include <string_view>

namespace tgcli::daemon {

enum class RecoveryPreflight { Removal, Logout };

[[nodiscard]] std::span<const RecoveryPreflight> recovery_preflight_order(std::string_view command);

// Registers every command implemented so far. The context must outlive the
// dispatcher.
void register_commands(Dispatcher& dispatcher, const DaemonContext& context);

} // namespace tgcli::daemon
