#include "cli/client.hpp"
#include "cli/routing.hpp"
#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "daemon/daemon_run.hpp"
#include "proto/frame.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
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

void wipe_argument(char* value) noexcept {
    if (value == nullptr) {
        return;
    }
    const auto length = std::strlen(value);
    volatile char* bytes = value;
    for (std::size_t index = 0; index < length; ++index) {
        bytes[index] = '\0';
    }
}

bool consume_legacy_bot_token(int argc, char** argv) noexcept {
    constexpr std::string_view option = "--bot-token";
    bool found = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            continue;
        }
        const std::string_view argument(argv[index]);
        if (argument == option) {
            found = true;
            if (index + 1 < argc) {
                wipe_argument(argv[index + 1]);
                ++index;
            }
        } else if (argument.starts_with("--bot-token=")) {
            found = true;
            wipe_argument(argv[index] + option.size() + 1);
        }
    }
    return found;
}

int report_insecure_bot_token() {
    const nlohmann::json rendered{
        {"error",
         {{"code", "INSECURE_SECRET_INPUT"},
          {"message", "bot tokens are not accepted on the command line"},
          {"details", {{"argument", "--bot-token"}, {"replacement", "--bot"}}}}}};
    std::fputs((rendered.dump() + "\n").c_str(), stderr);
    return tgcli::kUsage;
}

std::optional<tgcli::proto::WriteAuthority> write_authority(bool allow_write) {
    if (const char* allow = std::getenv("TGCLI_ALLOW_WRITE"); allow != nullptr && *allow != '\0') {
        if (std::string_view(allow) == "0") {
            return tgcli::proto::WriteAuthority::Deny;
        }
        if (std::string_view(allow) == "1") {
            return tgcli::proto::WriteAuthority::Grant;
        }
        return std::nullopt;
    }
    return allow_write ? tgcli::proto::WriteAuthority::Grant : tgcli::proto::WriteAuthority::Unset;
}

tgcli::proto::RequestContext make_request_context(bool json_output, bool yes, bool dry_run,
                                                  tgcli::proto::WriteAuthority authority) {
    tgcli::proto::RequestContext context;
    context.tty = ::isatty(STDIN_FILENO) != 0;
    context.json = json_output;
    context.yes = yes;
    context.dry_run = dry_run;
    context.write_authority = authority;
    if (std::array<char, 4096> cwd{}; ::getcwd(cwd.data(), cwd.size()) != nullptr) {
        context.cwd = cwd.data();
    }
    if (const char* media_dir = std::getenv("TGCLI_MEDIA_DIR");
        media_dir != nullptr && *media_dir != '\0') {
        context.media_dir = media_dir;
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

void report_verbose_diagnostic(const std::string& account, const std::vector<std::string>& command,
                               bool no_daemon, bool json_output) {
    std::string transport = "daemon";
    if (no_daemon) {
        transport = "in-process";
    } else if (command == std::vector<std::string>{"daemon", "run"}) {
        transport = "foreground-daemon";
    } else if (tgcli::cli::is_config_global_command(command)) {
        transport = "local";
    }
    if (json_output) {
        const nlohmann::json rendered{
            {"diagnostic", {{"account", account}, {"transport", transport}}}};
        std::fputs((rendered.dump() + "\n").c_str(), stderr);
        return;
    }
    std::fprintf(stderr, "diagnostic: account=%s transport=%s\n", account.c_str(),
                 transport.c_str());
}

void populate_config_global_args(nlohmann::json& args, const std::vector<std::string>& command,
                                 bool explicit_account, const std::string& add_account,
                                 const std::string& show_account, const std::string& use_account) {
    args["global_account_supplied"] = explicit_account;
    if (command == std::vector<std::string>{"account", "add"}) {
        args["account"] = add_account;
    } else if (command == std::vector<std::string>{"account", "show"}) {
        args["account"] = show_account;
    } else if (command == std::vector<std::string>{"account", "use"}) {
        args["account"] = use_account;
    }
}

std::optional<int> resolve_request_account(const std::vector<std::string>& command,
                                           bool explicit_account, std::string& account,
                                           nlohmann::json& args, bool& current_config_valid,
                                           std::optional<tgcli::cli::RoutingError>& route_error,
                                           const std::string& add_account,
                                           const std::string& show_account,
                                           const std::string& use_account) {
    if (tgcli::cli::is_config_global_command(command)) {
        account = "main";
        populate_config_global_args(args, command, explicit_account, add_account, show_account,
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
    route_error = routed.error;
    return std::nullopt;
}

int run(int argc, char** argv) {
    if (consume_legacy_bot_token(argc, argv)) {
        return report_insecure_bot_token();
    }
    CLI::App app{"tgcli — Telegram CLI"};
    app.require_subcommand(1);
    app.fallthrough();

    std::string account;
    bool json_output = false;
    bool no_daemon = false;
    bool verbose = false;
    bool allow_write = false;
    bool yes = false;
    bool dry_run = false;
    double timeout_seconds = 0.0;
    CLI::Option* account_option =
        app.add_option("--account", account, "account name (default from config / TGCLI_ACCOUNT)");
    app.add_flag("--json", json_output, "machine-readable JSON output");
    app.add_flag("-v,--verbose", verbose, "show tgcli diagnostics on stderr");
    app.add_flag("--allow-write", allow_write, "grant writes for this invocation");
    app.add_flag("--yes", yes, "approve destructive actions non-interactively");
    app.add_flag("--dry-run", dry_run, "validate and print a plan without mutation");
    app.add_flag("--no-daemon", no_daemon,
                 "run in-process without the daemon (debugging escape hatch)");
    CLI::Option* timeout_option =
        app.add_option("--timeout", timeout_seconds, "per-command deadline in seconds");

    app.add_subcommand("version", "print tgcli version");
    app.add_subcommand("doctor", "health/auth probe");
    bool login_qr = false;
    bool login_bot = false;
    std::string rejected_bot_token;
    CLI::App* login_cmd = app.add_subcommand("login", "authenticate the selected account");
    login_cmd->add_flag("--qr", login_qr, "authenticate by QR code");
    login_cmd->add_flag("--bot", login_bot, "authenticate a bot using a secure token source");
    CLI::Option* rejected_bot_token_option =
        login_cmd->add_option("--bot-token", rejected_bot_token, "rejected insecure legacy input");
    app.add_subcommand("me", "show the authenticated account identity");
    app.add_subcommand("logout", "log out the selected account");
    CLI::App* daemon_cmd = app.add_subcommand("daemon", "daemon management");
    daemon_cmd->require_subcommand(1);
    daemon_cmd->add_subcommand("run", "run the account daemon in the foreground");
    daemon_cmd->add_subcommand("status", "show the account daemon status");
    daemon_cmd->add_subcommand("stop", "stop the account daemon");
    daemon_cmd->add_subcommand("restart", "restart the account daemon");

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

    if (rejected_bot_token_option->count() != 0) {
        rejected_bot_token.assign(rejected_bot_token.size(), '\0');
        rejected_bot_token.clear();
        return report_insecure_bot_token();
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

    const auto folded_authority = write_authority(allow_write);
    if (!folded_authority) {
        const nlohmann::json rendered{
            {"error",
             {{"code", "USAGE"},
              {"message", "TGCLI_ALLOW_WRITE must be exactly 0 or 1 when set"},
              {"details",
               {{"argument", "TGCLI_ALLOW_WRITE"}, {"reason", "invalid_environment"}}}}}};
        std::fputs((rendered.dump() + "\n").c_str(), stderr);
        return tgcli::kUsage;
    }

    const bool supports_dry_run = command == std::vector<std::string>{"logout"} ||
                                  command == std::vector<std::string>{"account", "remove"};
    if (dry_run && !supports_dry_run) {
        const nlohmann::json rendered{
            {"error",
             {{"code", "USAGE"},
              {"message", "--dry-run is not supported for this command"},
              {"details", {{"argument", "--dry-run"}, {"reason", "unsupported_mode"}}}}}};
        std::fputs((rendered.dump() + "\n").c_str(), stderr);
        return tgcli::kUsage;
    }

    nlohmann::json request_args = nlohmann::json::object();
    auto request_context = make_request_context(json_output, yes, dry_run, *folded_authority);
    if (command == std::vector<std::string>{"login"}) {
        request_args = {{"qr", login_qr}, {"bot", login_bot}};
    }
    if (timeout_option->count() != 0) {
        request_context.timeout_seconds = timeout_seconds;
    }

    const bool explicit_account = account_option->count() != 0;
    bool current_config_valid = true;
    std::optional<tgcli::cli::RoutingError> unavailable_route_error;
    if (const auto route_exit = resolve_request_account(
            command, explicit_account, account, request_args, current_config_valid,
            unavailable_route_error, add_account, show_account, use_account);
        route_exit.has_value()) {
        return route_exit.value();
    }

    const std::string resolved_account = account;
    if (verbose) {
        report_verbose_diagnostic(resolved_account, command, no_daemon, json_output);
    }
    if (command == std::vector<std::string>{"daemon", "run"}) {
        return tgcli::daemon::run_daemon(resolved_account);
    }

    tgcli::proto::Request request(resolved_account);
    request.id = 1;
    request.command = command;
    request.args = std::move(request_args);
    request.context = std::move(request_context);

    tgcli::cli::RunOptions options;
    options.account = resolved_account;
    options.json = json_output;
    options.no_daemon = no_daemon;
    options.unavailable_route_error = std::move(unavailable_route_error);
    options.auto_spawn = command != std::vector<std::string>{"daemon", "status"} &&
                         command != std::vector<std::string>{"daemon", "stop"} && !dry_run &&
                         current_config_valid && !tgcli::cli::is_config_global_command(command);
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
