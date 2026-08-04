#pragma once

#include "common/config.hpp"
#include "common/paths.hpp"
#include "daemon/dispatch.hpp"

#include <functional>

namespace tgcli::daemon {

struct ConfigGlobalContext {
    std::reference_wrapper<const config::Store> store;
    paths::Environment environment;
};

// Registers config-global account commands. These handlers operate on the
// current config file and never require an account daemon or TDLib client.
void register_account_commands(Dispatcher& dispatcher, const ConfigGlobalContext& context);

} // namespace tgcli::daemon
