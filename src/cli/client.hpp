#pragma once

#include "cli/routing.hpp"
#include "proto/frame.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tgcli::cli {

class ChallengePrompt;

} // namespace tgcli::cli

namespace tgcli::daemon {
class Dispatcher;
}

namespace tgcli::core {
class TdClient;
}

namespace tgcli::cli {

struct RunOptions {
    std::string account = "main";
    bool json = false;
    bool no_daemon = false;
    bool auto_spawn = true;
    // Preserved invalid-current-config error for an explicit route. A running
    // daemon may still serve its last-good snapshot; an absent daemon cannot.
    std::optional<RoutingError> unavailable_route_error;
    bool restart_on_mismatch = true;
    std::chrono::milliseconds restart_timeout{60000};
    // Empty uses the running executable. Tests may supply the built tgcli
    // binary because their process image is the unit-test runner.
    std::string daemon_executable;
    // Optional injectable prompt for tests/frontends. Null uses the process
    // terminal (stdin for input, stderr for prompts).
    ChallengePrompt* prompt = nullptr;
    // Optional in-process dispatcher injection for frontends and end-to-end
    // transport tests. The normal --no-daemon path builds the production table.
    const daemon::Dispatcher* in_process_dispatcher = nullptr;
    // Shared fake-boundary seam for --no-daemon contract tests. Null creates
    // the production TDLib client; the caller retains ownership when set.
    core::TdClient* in_process_td_client = nullptr;
    // Shared fake-boundary seam for request-admission wall-clock tests.
    std::function<std::chrono::system_clock::time_point()> in_process_request_wall_clock;
};

// Only the two M1 dry-runs bypass daemon routing. M3/M4 dry-runs are
// auth-bound and therefore remain eligible for normal daemon autospawn.
bool uses_client_local_dry_run(const std::vector<std::string>& command, bool dry_run);

// Runs one command end to end: connect to (or spawn) the account daemon —
// or dispatch in-process under --no-daemon — then render response frames
// and map the terminal frame to an exit code. `doctor` degrades to local
// diagnostics when the daemon is unreachable (DESIGN.md §4).
int run_command(const proto::Request& request, const RunOptions& options);

} // namespace tgcli::cli
