#pragma once

#include "common/secret_hook.hpp"

#include <functional>
#include <sys/types.h>

namespace tgcli::secret_hook::testing {

struct RunHooks {
    std::function<void(pid_t)> on_spawn;
    std::function<void()> before_accept;
};

HookResult run(const HookRequest& request, const RunHooks& hooks);

} // namespace tgcli::secret_hook::testing
