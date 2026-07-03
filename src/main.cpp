#include "cli/client.hpp"
#include "common/exit_codes.hpp"
#include "daemon/daemon_run.hpp"
#include "proto/frame.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include <CLI/CLI.hpp>

namespace {

// Last-resort reporter for main's catch blocks: must not itself throw, so it
// formats without allocating. `message` is not JSON-escaped — callers pass
// literals or exception messages, and this path only exists for bugs.
void report_fatal(const char* message) noexcept {
    std::fprintf(stderr, "{\"error\":{\"code\":\"INTERNAL\",\"message\":\"%s\",\"details\":{}}}\n",
                 message);
}

tgcli::proto::RequestContext make_request_context(bool json_output) {
    tgcli::proto::RequestContext context;
    context.tty = ::isatty(STDIN_FILENO) != 0;
    context.json = json_output;
    if (std::array<char, 4096> cwd{}; ::getcwd(cwd.data(), cwd.size()) != nullptr) {
        context.cwd = cwd.data();
    }
    if (const char* media_dir = std::getenv("TGCLI_MEDIA_DIR");
        media_dir != nullptr && *media_dir != '\0') {
        context.media_dir = media_dir;
    }
    // Fold the environment's write authority into the frame (DESIGN.md §10);
    // --allow-write joins in M3.
    if (const char* allow = std::getenv("TGCLI_ALLOW_WRITE"); allow != nullptr) {
        if (std::string_view(allow) == "0") {
            context.write_authority = tgcli::proto::WriteAuthority::Deny;
        } else if (std::string_view(allow) == "1") {
            context.write_authority = tgcli::proto::WriteAuthority::Grant;
        }
    }
    return context;
}

int run(int argc, char** argv) {
    CLI::App app{"tgcli — Telegram CLI"};
    app.require_subcommand(1);
    app.fallthrough();

    std::string account = "main";
    if (const char* env_account = std::getenv("TGCLI_ACCOUNT");
        env_account != nullptr && *env_account != '\0') {
        account = env_account;
    }
    bool json_output = false;
    bool no_daemon = false;
    app.add_option("--account", account, "account name (default: main, env TGCLI_ACCOUNT)");
    app.add_flag("--json", json_output, "machine-readable JSON output");
    app.add_flag("--no-daemon", no_daemon,
                 "run in-process without the daemon (debugging escape hatch)");

    app.add_subcommand("version", "print tgcli version");
    app.add_subcommand("doctor", "health/auth probe");
    CLI::App* daemon_cmd = app.add_subcommand("daemon", "daemon management");
    daemon_cmd->require_subcommand(1);
    daemon_cmd->add_subcommand("run", "run the account daemon in the foreground");
    daemon_cmd->add_subcommand("stop", "stop the account daemon");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // CLI11 already prints the message; only the exit code is ours.
        return app.exit(e) == 0 ? tgcli::kOk : tgcli::kUsage;
    }

    std::vector<std::string> command;
    for (const CLI::App* sub = &app; !sub->get_subcommands().empty();) {
        sub = sub->get_subcommands().front();
        command.push_back(sub->get_name());
    }

    if (command == std::vector<std::string>{"daemon", "run"}) {
        return tgcli::daemon::run_daemon(account);
    }

    tgcli::proto::Request request;
    request.id = 1;
    request.command = command;
    request.context = make_request_context(json_output);

    tgcli::cli::RunOptions options;
    options.account = account;
    options.json = json_output;
    options.no_daemon = no_daemon;
    options.auto_spawn = command != std::vector<std::string>{"daemon", "stop"};
    return tgcli::cli::run_command(request, options);
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        report_fatal(e.what());
        return tgcli::kGeneric;
    } catch (...) { // exit, not continue: anything unnamed here is a bug
        report_fatal("unhandled exception");
        return tgcli::kGeneric;
    }
}
