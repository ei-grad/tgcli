#pragma once

#include <functional>
#include <string>

namespace tgcli::daemon {

// Everything M0 command handlers need. Grows with the milestones (config,
// resolver, safety state); td_api types never appear here — handlers that
// need tdlib reach it through core/ in their own .cpp.
struct DaemonContext {
    std::string account;
    std::string binary_version;
    int protocol_version = 0;
    std::string tdlib_version;
    std::string socket_path; // empty in --no-daemon mode
    bool in_process = false; // true under --no-daemon
    // Wired by the daemon entrypoint; asks the server to shut down
    // gracefully (daemon stop). No-op in --no-daemon mode.
    std::function<void()> request_shutdown = [] {};
};

} // namespace tgcli::daemon
