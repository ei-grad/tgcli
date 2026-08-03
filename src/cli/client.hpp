#pragma once

#include "proto/frame.hpp"

#include <chrono>
#include <string>

namespace tgcli::cli {

struct RunOptions {
    std::string account = "main";
    bool json = false;
    bool no_daemon = false;
    bool auto_spawn = true; // `daemon stop` must not resurrect the daemon
    std::chrono::milliseconds restart_timeout{10000};
    // Empty uses the running executable. Tests may supply the built tgcli
    // binary because their process image is the unit-test runner.
    std::string daemon_executable;
};

// Runs one command end to end: connect to (or spawn) the account daemon —
// or dispatch in-process under --no-daemon — then render response frames
// and map the terminal frame to an exit code. `doctor` degrades to local
// diagnostics when the daemon is unreachable (DESIGN.md §4).
int run_command(const proto::Request& request, const RunOptions& options);

} // namespace tgcli::cli
