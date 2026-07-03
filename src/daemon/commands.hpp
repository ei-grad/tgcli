#pragma once

#include "daemon/context.hpp"
#include "daemon/dispatch.hpp"

namespace tgcli::daemon {

// Registers every command implemented so far. The context must outlive the
// dispatcher.
void register_commands(Dispatcher& dispatcher, const DaemonContext& context);

} // namespace tgcli::daemon
