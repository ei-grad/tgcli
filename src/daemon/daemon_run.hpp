#pragma once

#include "daemon/dispatch.hpp"
#include "proto/frame.hpp"

#include <chrono>
#include <string>

namespace tgcli::daemon {

// Foreground daemon entrypoint (`tgcli daemon run`): binds the account
// socket, serves until `daemon stop` or SIGTERM/SIGINT, closes tdlib
// cleanly. Returns a process exit code.
int run_daemon(const std::string& account);

// --no-daemon: the same dispatch path minus the socket. Returns false with
// a reason if it cannot run (invalid account, or a daemon holds the account
// lock). Command results/errors flow through the sink.
bool run_no_daemon(const proto::Request& request, ResponseSink& sink, const std::string& account,
                   std::string& error, const Dispatcher* dispatcher_override = nullptr);

// Reconciles an unmatched account-local logout audit without binding sockets or
// starting a background daemon. The caller should isolate this bounded DB-open
// operation in a child process so a tardy TDLib close cannot extend its deadline.
bool reconcile_logout_audit_offline(const std::string& account,
                                    std::chrono::steady_clock::time_point deadline);

} // namespace tgcli::daemon
