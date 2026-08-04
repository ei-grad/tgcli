#include "cli/client.hpp"
#include "cli/routing.hpp"
#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "daemon/daemon_run.hpp"
#include "proto/frame.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
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

void report_invalid_test_dc() noexcept {
    std::fputs(
        "{\"error\":{\"code\":\"USAGE\",\"message\":\"TGCLI_TEST_DC must be exactly 1 when "
        "set\",\"details\":{\"argument\":\"TGCLI_TEST_DC\",\"reason\":\"invalid_environment\"}}}\n",
        stderr);
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

struct ParseErrorDescription {
    const char* message;
    const char* reason;
};

ParseErrorDescription describe_parse_error(const CLI::ParseError& error) {
    if (dynamic_cast<const CLI::RequiredError*>(&error) != nullptr ||
        dynamic_cast<const CLI::ArgumentMismatch*>(&error) != nullptr) {
        return {"required command argument is missing", "missing_argument"};
    }
    if (dynamic_cast<const CLI::ExtrasError*>(&error) != nullptr) {
        return {"unknown command or argument", "unknown_command"};
    }
    return {"invalid command argument", "invalid_argument"};
}

std::optional<int> parse_arguments(CLI::App& app, int argc, char** argv) {
    try {
        app.parse(argc, argv);
        return std::nullopt;
    } catch (const CLI::ParseError& error) {
        if (error.get_exit_code() == 0) {
            return app.exit(error);
        }
        const auto description = describe_parse_error(error);
        const nlohmann::json rendered{
            {"error",
             {{"code", "USAGE"},
              {"message", description.message},
              {"details", {{"argument", nullptr}, {"reason", description.reason}}}}}};
        std::fputs((rendered.dump() + "\n").c_str(), stderr);
        return tgcli::kUsage;
    }
}

std::vector<std::string> selected_command(const CLI::App& app) {
    std::vector<std::string> command;
    for (const CLI::App* sub = &app; !sub->get_subcommands().empty();) {
        sub = sub->get_subcommands().front();
        command.push_back(sub->get_name());
    }
    return command;
}

bool is_daemon_lifecycle(const std::vector<std::string>& command) {
    return command == std::vector<std::string>{"daemon", "run"} ||
           command == std::vector<std::string>{"daemon", "status"} ||
           command == std::vector<std::string>{"daemon", "stop"} ||
           command == std::vector<std::string>{"daemon", "restart"};
}

void populate_config_global_args(tgcli::proto::Request& request,
                                 const std::vector<std::string>& command, bool explicit_account,
                                 const std::string& add_account, const std::string& show_account,
                                 const std::string& use_account) {
    request.args["global_account_supplied"] = explicit_account;
    if (command == std::vector<std::string>{"account", "add"}) {
        request.args["account"] = add_account;
    } else if (command == std::vector<std::string>{"account", "show"}) {
        request.args["account"] = show_account;
    } else if (command == std::vector<std::string>{"account", "use"}) {
        request.args["account"] = use_account;
    }
}

std::optional<int>
resolve_request_account(const std::vector<std::string>& command, bool explicit_account,
                        std::string& account, tgcli::proto::Request& request,
                        bool& current_config_valid, const std::string& add_account,
                        const std::string& show_account, const std::string& use_account) {
    if (tgcli::cli::is_config_global_command(command)) {
        populate_config_global_args(request, command, explicit_account, add_account, show_account,
                                    use_account);
        return std::nullopt;
    }

    std::optional<std::string> environment_account;
    if (const char* value = std::getenv("TGCLI_ACCOUNT"); value != nullptr && *value != '\0') {
        environment_account = value;
    }
    const auto environment = tgcli::paths::real_environment();
    const auto routed = tgcli::cli::resolve_account_route(
        command, environment, explicit_account ? std::optional<std::string>{account} : std::nullopt,
        environment_account);
    if (!routed.selection) {
        if (!routed.error.has_value()) {
            report_fatal("account routing failed without an error");
            return tgcli::kGeneric;
        }
        const auto& route_error = routed.error.value();
        const nlohmann::json rendered{{"error",
                                       {{"code", route_error.code},
                                        {"message", route_error.message},
                                        {"details", route_error.details}}}};
        std::fputs((rendered.dump() + "\n").c_str(), stderr);
        return route_error.exit_code;
    }
    account = routed.selection->name;
    current_config_valid = routed.current_config_valid;
    return std::nullopt;
}

int run(int argc, char** argv) {
    CLI::App app{"tgcli — Telegram CLI"};
    app.require_subcommand(1);
    app.fallthrough();

    std::string account;
    bool json_output = false;
    bool no_daemon = false;
    double timeout_seconds = 0.0;
    CLI::Option* account_option =
        app.add_option("--account", account, "account name (default from config / TGCLI_ACCOUNT)");
    app.add_flag("--json", json_output, "machine-readable JSON output");
    app.add_flag("--no-daemon", no_daemon,
                 "run in-process without the daemon (debugging escape hatch)");
    CLI::Option* timeout_option =
        app.add_option("--timeout", timeout_seconds, "per-command deadline in seconds");

    app.add_subcommand("version", "print tgcli version");
    app.add_subcommand("doctor", "health/auth probe");
    CLI::App* daemon_cmd = app.add_subcommand("daemon", "daemon management");
    daemon_cmd->require_subcommand(1);
    daemon_cmd->add_subcommand("run", "run the account daemon in the foreground");
    daemon_cmd->add_subcommand("stop", "stop the account daemon");

    CLI::App* account_cmd = app.add_subcommand("account", "account configuration");
    account_cmd->require_subcommand(1);
    std::string add_account;
    std::string show_account;
    std::string use_account;
    account_cmd->add_subcommand("add", "add an account")
        ->add_option("name", add_account)
        ->required();
    account_cmd->add_subcommand("list", "list configured accounts");
    account_cmd->add_subcommand("show", "show account configuration")
        ->add_option("name", show_account)
        ->required();
    account_cmd->add_subcommand("use", "set the default account")
        ->add_option("name", use_account)
        ->required();

    if (const auto parse_exit = parse_arguments(app, argc, argv); parse_exit.has_value()) {
        return parse_exit.value();
    }

    const auto command = selected_command(app);
    if (no_daemon && is_daemon_lifecycle(command)) {
        const nlohmann::json rendered{
            {"error",
             {{"code", "USAGE"},
              {"message", "daemon lifecycle commands do not support --no-daemon"},
              {"details", nlohmann::json::object()}}}};
        std::fputs((rendered.dump() + "\n").c_str(), stderr);
        return tgcli::kUsage;
    }

    tgcli::proto::Request request;
    request.id = 1;
    request.command = command;
    request.context = make_request_context(json_output);
    if (timeout_option->count() != 0) {
        request.context.timeout_seconds = timeout_seconds;
    }

    const bool explicit_account = account_option->count() != 0;
    bool current_config_valid = true;
    if (const auto route_exit =
            resolve_request_account(command, explicit_account, account, request,
                                    current_config_valid, add_account, show_account, use_account);
        route_exit.has_value()) {
        return route_exit.value();
    }

    if (command == std::vector<std::string>{"daemon", "run"}) {
        return tgcli::daemon::run_daemon(account);
    }

    tgcli::cli::RunOptions options;
    options.account = account.empty() ? "main" : account;
    options.json = json_output;
    options.no_daemon = no_daemon;
    options.auto_spawn = command != std::vector<std::string>{"daemon", "stop"} &&
                         !tgcli::cli::is_config_global_command(command) &&
                         (command != std::vector<std::string>{"doctor"} || current_config_valid);
    return tgcli::cli::run_command(request, options);
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const tgcli::paths::InvalidTestDcEnvironment&) {
        report_invalid_test_dc();
        return tgcli::kUsage;
    } catch (const std::exception& e) {
        report_fatal(e.what());
        return tgcli::kGeneric;
    } catch (...) { // exit, not continue: anything unnamed here is a bug
        report_fatal("unhandled exception");
        return tgcli::kGeneric;
    }
}
