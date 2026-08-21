#include "cli/client.hpp"
#include "cli/routing.hpp"
#include "cli/schema_command.hpp"
#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "daemon/chats_commands.hpp"
#include "daemon/daemon_run.hpp"
#include "daemon/fetch_domain.hpp"
#include "daemon/local_selector.hpp"
#include "daemon/read_domain.hpp"
#include "daemon/resolver.hpp"
#include "daemon/saved_commands.hpp"
#include "proto/frame.hpp"
#include "proto/operation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

bool contains_reserved_full(int argc, char** argv) noexcept {
    constexpr std::string_view option = "--full";
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            continue;
        }
        const std::string_view argument(argv[index]);
        if (argument == option || argument.starts_with("--full=")) {
            return true;
        }
    }
    return false;
}

bool targets_schema_command(int argc, char** argv) noexcept {
    constexpr std::array<std::string_view, 4> value_options{"--account", "--idempotency-key",
                                                            "--timeout", "--cursor"};
    bool positional_only = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            continue;
        }
        const std::string_view argument(argv[index]);
        if (positional_only) {
            return argument == "schema";
        }
        if (argument == "--") {
            positional_only = true;
            continue;
        }

        bool consumes_value = false;
        bool attached_value = false;
        for (const auto option : value_options) {
            if (argument == option) {
                consumes_value = true;
                break;
            }
            if (argument.size() > option.size() && argument.starts_with(option) &&
                argument[option.size()] == '=') {
                attached_value = true;
                break;
            }
        }
        if (consumes_value) {
            ++index;
            continue;
        }
        if (attached_value || argument.starts_with('-')) {
            continue;
        }
        return argument == "schema";
    }
    return false;
}

void configure_schema_parent_help(CLI::App& app, bool schema_invocation, bool& schema_help) {
    if (schema_invocation) {
        app.set_help_flag();
        app.add_flag("-h,--help", schema_help, "Print this help message and exit");
    }
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

int report_missing_command() {
    const nlohmann::json rendered{
        {"error",
         {{"code", "USAGE"},
          {"message", "required command argument is missing"},
          {"details", {{"argument", nullptr}, {"reason", "missing_argument"}}}}}};
    std::fputs((rendered.dump() + "\n").c_str(), stderr);
    return tgcli::kUsage;
}

int report_usage(std::string_view message, const nlohmann::json& argument,
                 std::string_view reason = "invalid_argument") {
    const nlohmann::json rendered{{"error",
                                   {{"code", "USAGE"},
                                    {"message", message},
                                    {"details", {{"argument", argument}, {"reason", reason}}}}}};
    std::fputs((rendered.dump() + "\n").c_str(), stderr);
    return tgcli::kUsage;
}

int report_unsupported_mode(std::string_view argument) {
    const nlohmann::json rendered{
        {"error",
         {{"code", "USAGE"},
          {"message", std::string(argument) + " is reserved until M7"},
          {"details", {{"argument", argument}, {"reason", "unsupported_mode"}}}}}};
    std::fputs((rendered.dump() + "\n").c_str(), stderr);
    return tgcli::kUsage;
}

std::string_view schema_unsupported_option(const CLI::Option& account, const CLI::Option& full,
                                           const CLI::Option& allow_write, const CLI::Option& yes,
                                           const CLI::Option& dry_run, const CLI::Option& timeout,
                                           const CLI::Option& cursor,
                                           const CLI::Option& idempotency_key) {
    if (account.count() != 0) {
        return "--account";
    }
    if (full.count() != 0) {
        return "--full";
    }
    if (allow_write.count() != 0) {
        return "--allow-write";
    }
    if (yes.count() != 0) {
        return "--yes";
    }
    if (dry_run.count() != 0) {
        return "--dry-run";
    }
    if (timeout.count() != 0) {
        return "--timeout";
    }
    if (cursor.count() != 0) {
        return "--cursor";
    }
    if (idempotency_key.count() != 0) {
        return "--idempotency-key";
    }
    return {};
}

std::optional<int> handle_client_local_or_reserved_command(
    const std::vector<std::string>& command, const CLI::Option& account, const CLI::Option& full,
    const CLI::Option& allow_write, const CLI::Option& yes, const CLI::Option& dry_run,
    const CLI::Option& timeout, double timeout_seconds, const CLI::Option& cursor,
    const CLI::Option& idempotency_key, const CLI::Option& no_color,
    const std::vector<std::string>& schema_target, bool schema_all, bool schema_help,
    bool verbose) {
    if (command == std::vector<std::string>{"schema"}) {
        if (timeout.count() != 0 &&
            !tgcli::request_deadline(timeout_seconds, tgcli::DeadlineDefault::Default60)) {
            return report_usage("invalid request timeout", "--timeout");
        }
        const auto unsupported_option = schema_unsupported_option(
            account, full, allow_write, yes, dry_run, timeout, cursor, idempotency_key);
        return tgcli::cli::run_schema_command(
            {{schema_target}, unsupported_option, schema_all, schema_help, verbose});
    }
    if (full.count() != 0) {
        return report_unsupported_mode("--full");
    }
    if (no_color.count() != 0) {
        return report_usage("unknown command or argument", nullptr, "unknown_command");
    }
    if (command == std::vector<std::string>{"raw"}) {
        return report_unsupported_mode("raw");
    }
    return std::nullopt;
}

std::optional<int> validate_idempotency_option(const CLI::Option& option,
                                               const std::vector<std::string>& command,
                                               std::string_view key, bool dry_run) {
    if (option.count() == 0) {
        return std::nullopt;
    }
    if (!tgcli::proto::valid_idempotency_key(key)) {
        return report_usage("--idempotency-key has invalid syntax", "--idempotency-key");
    }
    if (dry_run) {
        return report_usage("--idempotency-key cannot be combined with --dry-run",
                            "--idempotency-key", "mutually_exclusive");
    }
    if (!tgcli::proto::m3_operation_for_command(command)) {
        return report_usage("--idempotency-key is not supported for this command",
                            "--idempotency-key", "unsupported_mode");
    }
    return std::nullopt;
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

std::optional<int>
resolve_request_account(const std::vector<std::string>& command, bool explicit_account,
                        std::string& account, nlohmann::json& args, bool& current_config_valid,
                        std::optional<tgcli::cli::RoutingError>& route_error,
                        const std::string& add_account, const std::string& show_account,
                        const std::string& use_account, const std::string& remove_account,
                        bool keep_session, const std::string& reassign_default,
                        bool reassign_default_supplied) {
    if (command == std::vector<std::string>{"account", "remove"}) {
        if (explicit_account) {
            const nlohmann::json rendered{
                {"error",
                 {{"code", "USAGE"},
                  {"message", "--account cannot target an account subcommand"},
                  {"details", {{"argument", "--account"}, {"reason", "mutually_exclusive"}}}}}};
            std::fputs((rendered.dump() + "\n").c_str(), stderr);
            return tgcli::kUsage;
        }
        std::string selection_error;
        const auto selection = tgcli::paths::select_account(
            {std::optional<std::string>{remove_account}, std::nullopt, std::nullopt},
            selection_error);
        if (!selection) {
            const nlohmann::json rendered{
                {"error",
                 {{"code", "USAGE"},
                  {"message", selection_error},
                  {"details", {{"argument", "name"}, {"reason", "invalid_argument"}}}}}};
            std::fputs((rendered.dump() + "\n").c_str(), stderr);
            return tgcli::kUsage;
        }
        account = selection->name;
        args = {{"account", account},
                {"global_account_supplied", false},
                {"keep_session", keep_session},
                {"reassign_default", reassign_default_supplied ? nlohmann::json(reassign_default)
                                                               : nlohmann::json(nullptr)}};
        return std::nullopt;
    }
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

struct SavedCliArguments {
    std::string query;
    std::string tag;
    std::string cursor;
    int limit = tgcli::daemon::kDefaultSavedSearchLimit;
    CLI::Option* query_option = nullptr;
    CLI::Option* tag_option = nullptr;
    CLI::Option* limit_option = nullptr;
    CLI::Option* cursor_option = nullptr;
};

struct ChatsCliArguments {
    std::int64_t folder = 0;
    int limit = tgcli::daemon::kDefaultChatsLimit;
    bool archived = false;
    bool unread = false;
    CLI::Option* folder_option = nullptr;
    CLI::Option* limit_option = nullptr;
    CLI::Option* archived_option = nullptr;
    CLI::Option* unread_option = nullptr;
};

struct MessageCliArguments {
    std::string get_chat;
    std::vector<std::int64_t> get_ids;
    std::string link_chat;
    std::int64_t link_id = 0;
};

struct ReadCliArguments {
    std::string chat;
    std::int64_t before = 0;
    std::string since;
    std::string until;
    std::string topic;
    int limit = tgcli::daemon::kDefaultReadLimit;
    bool local = false;
    CLI::Option* chat_option = nullptr;
    CLI::Option* before_option = nullptr;
    CLI::Option* since_option = nullptr;
    CLI::Option* until_option = nullptr;
    CLI::Option* topic_option = nullptr;
    CLI::Option* limit_option = nullptr;
    CLI::Option* local_option = nullptr;
};

struct FetchCliArguments {
    std::string chat;
    std::string since;
    int limit = tgcli::daemon::kDefaultFetchLimit;
    bool all = false;
    CLI::Option* limit_option = nullptr;
    CLI::Option* since_option = nullptr;
    CLI::Option* all_option = nullptr;
};

bool is_saved_tags(const std::vector<std::string>& command) {
    return command == std::vector<std::string>{"saved", "tags"};
}

bool is_saved_search(const std::vector<std::string>& command) {
    return command == std::vector<std::string>{"saved", "search"};
}

std::optional<int> validate_saved_arguments(const std::vector<std::string>& command,
                                            const SavedCliArguments& saved) {
    if (saved.cursor_option->count() != 0 && is_saved_tags(command)) {
        return report_usage("saved tags does not accept --cursor", "--cursor");
    }
    if (saved.cursor_option->count() != 0 && !is_saved_search(command) &&
        command != std::vector<std::string>{"chats"} &&
        command != std::vector<std::string>{"read"}) {
        return report_usage("--cursor is not supported for this command", "--cursor",
                            "unsupported_mode");
    }
    if (!is_saved_search(command)) {
        return std::nullopt;
    }
    if (saved.cursor_option->count() != 0 && saved.limit_option->count() != 0) {
        return report_usage("-n is not accepted with a continuation cursor", "-n");
    }
    if (saved.cursor_option->count() == 0 && saved.tag_option->count() == 0) {
        return report_usage("saved search requires --tag on the first page", "--tag",
                            "missing_argument");
    }
    if (saved.limit_option->count() != 0 &&
        (saved.limit < 1 || saved.limit > tgcli::daemon::kMaximumSavedSearchLimit)) {
        return report_usage("saved search limit must be between 1 and 100", "-n");
    }
    if (saved.tag_option->count() != 0 &&
        !tgcli::daemon::parse_saved_reaction_selector(saved.tag)) {
        return report_usage("invalid Saved Messages reaction selector", "--tag");
    }
    if (saved.query_option->count() != 0 && !tgcli::daemon::valid_utf8(saved.query)) {
        return report_usage("saved search query must be valid UTF-8", "query");
    }
    if (saved.cursor_option->count() != 0 &&
        !tgcli::daemon::decode_saved_search_cursor(saved.cursor)) {
        return report_usage("invalid Saved Messages search cursor", "--cursor");
    }
    return std::nullopt;
}

std::optional<int> validate_chats_arguments(const std::vector<std::string>& command,
                                            const ChatsCliArguments& chats,
                                            const SavedCliArguments& pagination);

// Each command's closed CLI contract is validated before common request construction.
// NOLINTBEGIN(readability-function-cognitive-complexity)
std::optional<int>
validate_command_arguments(const std::vector<std::string>& command, const SavedCliArguments& saved,
                           const ChatsCliArguments& chats, const MessageCliArguments& messages,
                           const ReadCliArguments& read, const FetchCliArguments& fetch,
                           std::string_view resolve_selector) {
    if (const auto saved_exit = validate_saved_arguments(command, saved); saved_exit) {
        return saved_exit;
    }
    if (const auto chats_exit = validate_chats_arguments(command, chats, saved); chats_exit) {
        return chats_exit;
    }
    if (command == std::vector<std::string>{"read"}) {
        const bool cursor = saved.cursor_option->count() != 0;
        if (cursor) {
            if (read.chat_option->count() != 0 || read.before_option->count() != 0 ||
                read.since_option->count() != 0 || read.until_option->count() != 0 ||
                read.topic_option->count() != 0 || read.limit_option->count() != 0 ||
                read.local_option->count() != 0) {
                return report_usage("read cursor cannot be combined with first-page arguments",
                                    "--cursor", "mutually_exclusive");
            }
            if (!tgcli::daemon::decode_read_cursor(saved.cursor)) {
                return report_usage("invalid read cursor", "--cursor", "invalid_cursor");
            }
        } else {
            if (read.chat_option->count() == 0) {
                return report_usage("read requires a chat selector", "chat", "missing_argument");
            }
            if (read.local) {
                const auto selector = tgcli::daemon::classify_local_selector(read.chat);
                if (!selector || selector->kind == tgcli::daemon::LocalSelectorKind::InvalidLink) {
                    return report_usage("read selector is invalid", "selector");
                }
                if (selector->kind == tgcli::daemon::LocalSelectorKind::UnsupportedLink) {
                    return report_usage("read selector has an unsupported link type", "selector",
                                        "unsupported_link_type");
                }
            } else if (!tgcli::daemon::valid_resolve_selector(read.chat)) {
                return report_usage("read selector is invalid", "selector");
            }
            constexpr std::int64_t maximum_int53 = 9007199254740991LL;
            if (read.before_option->count() != 0 &&
                (read.before == 0 || read.before < -maximum_int53 || read.before > maximum_int53)) {
                return report_usage("--before must be a nonzero int53 message id", "--before");
            }
            if (read.limit_option->count() != 0 &&
                (read.limit < 1 || read.limit > tgcli::daemon::kMaximumReadLimit)) {
                return report_usage("read limit must be between 1 and 100", "-n");
            }
            if (read.topic_option->count() != 0 && !tgcli::daemon::parse_read_topic(read.topic)) {
                return report_usage("invalid read topic", "--topic");
            }
            const auto request_start = std::chrono::system_clock::now();
            std::optional<std::int32_t> since;
            std::optional<std::int32_t> until;
            if (read.since_option->count() != 0) {
                since = tgcli::daemon::parse_read_timestamp(
                    read.since, tgcli::daemon::ReadTimestampBound::Since, request_start);
                if (!since) {
                    return report_usage("invalid --since timestamp", "--since");
                }
            }
            if (read.until_option->count() != 0) {
                until = tgcli::daemon::parse_read_timestamp(
                    read.until, tgcli::daemon::ReadTimestampBound::Until, request_start);
                if (!until) {
                    return report_usage("invalid --until timestamp", "--until");
                }
            }
            if (since && until && *since > *until) {
                return report_usage("--since must not be later than --until", "--since/--until");
            }
        }
    }
    if (command == std::vector<std::string>{"resolve"} &&
        !tgcli::daemon::valid_resolve_selector(resolve_selector)) {
        return report_usage("resolve selector is invalid", "selector");
    }
    if (command == std::vector<std::string>{"fetch"}) {
        if (!tgcli::daemon::valid_resolve_selector(fetch.chat)) {
            return report_usage("fetch selector is invalid", "selector");
        }
        if (fetch.limit_option->count() != 0 &&
            (fetch.limit < 1 || fetch.limit > tgcli::daemon::kMaximumFetchLimit)) {
            return report_usage("fetch limit must be between 1 and 1000000", "--limit");
        }
        if (fetch.limit_option->count() != 0 && fetch.all_option->count() != 0) {
            return report_usage("--limit and --all are mutually exclusive", "--limit/--all",
                                "mutually_exclusive");
        }
        if (fetch.since_option->count() != 0 &&
            !tgcli::daemon::parse_read_timestamp(fetch.since,
                                                 tgcli::daemon::ReadTimestampBound::Since,
                                                 std::chrono::system_clock::now())) {
            return report_usage("invalid --since timestamp", "--since");
        }
    }
    constexpr std::int64_t maximum_int53 = 9007199254740991LL;
    const auto valid_message_id = [](std::int64_t id) {
        return id != 0 && id >= -maximum_int53 && id <= maximum_int53;
    };
    if (command == std::vector<std::string>{"msg", "get"}) {
        if (!tgcli::daemon::valid_resolve_selector(messages.get_chat)) {
            return report_usage("msg get chat selector is invalid", "chat");
        }
        if (messages.get_ids.empty() || messages.get_ids.size() > 100) {
            return report_usage("msg get requires between 1 and 100 message ids", "id",
                                "invalid_argument");
        }
        if (!std::ranges::all_of(messages.get_ids, valid_message_id)) {
            return report_usage("message ids must be nonzero int53 values", "id");
        }
    }
    if (command == std::vector<std::string>{"msg", "link"}) {
        if (!tgcli::daemon::valid_resolve_selector(messages.link_chat)) {
            return report_usage("msg link chat selector is invalid", "chat");
        }
        if (!valid_message_id(messages.link_id)) {
            return report_usage("message id must be a nonzero int53 value", "id");
        }
    }
    return std::nullopt;
}
// NOLINTEND(readability-function-cognitive-complexity)

std::optional<int> validate_chats_arguments(const std::vector<std::string>& command,
                                            const ChatsCliArguments& chats,
                                            const SavedCliArguments& pagination) {
    if (command != std::vector<std::string>{"chats"}) {
        return std::nullopt;
    }
    const bool cursor = pagination.cursor_option->count() != 0;
    if (cursor && chats.folder_option->count() != 0) {
        return report_usage("--folder is not accepted with a continuation cursor", "--folder");
    }
    if (cursor && chats.archived_option->count() != 0) {
        return report_usage("--archived is not accepted with a continuation cursor", "--archived");
    }
    if (cursor && chats.unread_option->count() != 0) {
        return report_usage("--unread is not accepted with a continuation cursor", "--unread");
    }
    if (cursor && chats.limit_option->count() != 0) {
        return report_usage("-n is not accepted with a continuation cursor", "-n");
    }
    if (chats.archived_option->count() != 0 && chats.folder_option->count() != 0) {
        return report_usage("--archived and --folder are mutually exclusive", "--archived/--folder",
                            "mutually_exclusive");
    }
    if (chats.folder_option->count() != 0 &&
        (chats.folder <= 0 || chats.folder > std::numeric_limits<std::int32_t>::max())) {
        return report_usage("chat folder id must be a positive int32", "--folder");
    }
    if (chats.limit_option->count() != 0 &&
        (chats.limit < 1 || chats.limit > tgcli::daemon::kMaximumChatsLimit)) {
        return report_usage("chats limit must be between 1 and 100", "-n");
    }
    if (cursor && !tgcli::daemon::decode_chats_cursor(pagination.cursor)) {
        return report_usage("invalid chats cursor", "--cursor", "invalid_cursor");
    }
    return std::nullopt;
}

nlohmann::json chats_request_args(const ChatsCliArguments& chats,
                                  const SavedCliArguments& pagination) {
    return {
        {"archived", chats.archived},
        {"cursor", pagination.cursor_option->count() != 0 ? nlohmann::json(pagination.cursor)
                                                          : nlohmann::json(nullptr)},
        {"folder", chats.folder_option->count() != 0 ? nlohmann::json(chats.folder)
                                                     : nlohmann::json(nullptr)},
        {"limit",
         chats.limit_option->count() != 0 ? nlohmann::json(chats.limit) : nlohmann::json(nullptr)},
        {"unread", chats.unread},
    };
}

nlohmann::json saved_search_request_args(const SavedCliArguments& saved) {
    return {
        {"query",
         saved.query_option->count() != 0 ? nlohmann::json(saved.query) : nlohmann::json(nullptr)},
        {"tag",
         saved.tag_option->count() != 0 ? nlohmann::json(saved.tag) : nlohmann::json(nullptr)},
        {"limit",
         saved.limit_option->count() != 0 ? nlohmann::json(saved.limit) : nlohmann::json(nullptr)},
        {"cursor", saved.cursor_option->count() != 0 ? nlohmann::json(saved.cursor)
                                                     : nlohmann::json(nullptr)},
    };
}

nlohmann::json read_request_args(const ReadCliArguments& read,
                                 const SavedCliArguments& pagination) {
    return {
        {"chat",
         read.chat_option->count() != 0 ? nlohmann::json(read.chat) : nlohmann::json(nullptr)},
        {"before",
         read.before_option->count() != 0 ? nlohmann::json(read.before) : nlohmann::json(nullptr)},
        {"since",
         read.since_option->count() != 0 ? nlohmann::json(read.since) : nlohmann::json(nullptr)},
        {"until",
         read.until_option->count() != 0 ? nlohmann::json(read.until) : nlohmann::json(nullptr)},
        {"topic",
         read.topic_option->count() != 0 ? nlohmann::json(read.topic) : nlohmann::json(nullptr)},
        {"local", read.local},
        {"limit",
         read.limit_option->count() != 0 ? nlohmann::json(read.limit) : nlohmann::json(nullptr)},
        {"cursor", pagination.cursor_option->count() != 0 ? nlohmann::json(pagination.cursor)
                                                          : nlohmann::json(nullptr)},
    };
}

nlohmann::json command_request_args(const std::vector<std::string>& command, bool login_qr,
                                    bool login_bot, std::string_view resolve_selector,
                                    const ChatsCliArguments& chats, const SavedCliArguments& saved,
                                    const MessageCliArguments& messages,
                                    const ReadCliArguments& read, const FetchCliArguments& fetch) {
    if (command == std::vector<std::string>{"login"}) {
        return {{"qr", login_qr}, {"bot", login_bot}};
    }
    if (command == std::vector<std::string>{"resolve"}) {
        return {{"selector", resolve_selector}};
    }
    if (command == std::vector<std::string>{"chats"}) {
        return chats_request_args(chats, saved);
    }
    if (command == std::vector<std::string>{"read"}) {
        return read_request_args(read, saved);
    }
    if (command == std::vector<std::string>{"fetch"}) {
        return {{"chat", fetch.chat},
                {"limit", fetch.limit_option->count() != 0 ? nlohmann::json(fetch.limit)
                                                           : nlohmann::json(nullptr)},
                {"all", fetch.all},
                {"since", fetch.since_option->count() != 0 ? nlohmann::json(fetch.since)
                                                           : nlohmann::json(nullptr)}};
    }
    if (is_saved_search(command)) {
        return saved_search_request_args(saved);
    }
    if (command == std::vector<std::string>{"msg", "get"}) {
        return {{"chat", messages.get_chat}, {"message_ids", messages.get_ids}};
    }
    if (command == std::vector<std::string>{"msg", "link"}) {
        return {{"chat", messages.link_chat}, {"message_id", messages.link_id}};
    }
    return nlohmann::json::object();
}

int run(int argc, char** argv) {
    if (consume_legacy_bot_token(argc, argv)) {
        return report_insecure_bot_token();
    }
    const bool schema_invocation = targets_schema_command(argc, argv);
    if (!schema_invocation && contains_reserved_full(argc, argv)) {
        return report_unsupported_mode("--full");
    }
    bool schema_help = false;
    CLI::App app{"tgcli — Telegram CLI"};
    app.require_subcommand(0, 1);
    app.fallthrough();
    configure_schema_parent_help(app, schema_invocation, schema_help);

    std::string account;
    bool json_output = false;
    bool full = false;
    bool no_daemon = false;
    bool no_color = false;
    bool verbose = false;
    bool allow_write = false;
    bool yes = false;
    bool dry_run = false;
    std::string idempotency_key;
    double timeout_seconds = 0.0;
    std::string resolve_selector;
    SavedCliArguments saved;
    ChatsCliArguments chats;
    MessageCliArguments messages;
    ReadCliArguments read;
    ReadCliArguments history;
    FetchCliArguments fetch;
    CLI::Option* account_option =
        app.add_option("--account", account, "account name (default from config / TGCLI_ACCOUNT)");
    app.add_flag("--json", json_output, "machine-readable JSON output");
    CLI::Option* full_option = app.add_flag("--full", full, "reserved until M7");
    app.add_flag("-v,--verbose", verbose, "show tgcli diagnostics on stderr");
    CLI::Option* allow_write_option =
        app.add_flag("--allow-write", allow_write, "grant writes for this invocation");
    CLI::Option* yes_option =
        app.add_flag("--yes", yes, "approve destructive actions non-interactively");
    CLI::Option* dry_run_option =
        app.add_flag("--dry-run", dry_run, "validate and print a plan without mutation");
    CLI::Option* idempotency_key_option = app.add_option("--idempotency-key", idempotency_key,
                                                         "deduplicate an M3/M4 write invocation");
    app.add_flag("--no-daemon", no_daemon,
                 "run in-process without the daemon (debugging escape hatch)");
    CLI::Option* no_color_option = app.add_flag("--no-color", no_color, "disable colored output");
    no_color_option->group("");
    CLI::Option* timeout_option =
        app.add_option("--timeout", timeout_seconds, "per-command deadline in seconds");
    saved.cursor_option =
        app.add_option("--cursor", saved.cursor, "resume pagination from an opaque cursor");

    app.add_subcommand("version", "print tgcli version");
    app.add_subcommand("doctor", "health/auth probe");
    std::vector<std::string> schema_target;
    bool schema_all = false;
    CLI::App* schema_cmd = app.add_subcommand("schema", "Print curated JSON schemas");
    schema_cmd->set_help_flag();
    schema_cmd->add_flag("-h,--help", schema_help, "Print this help message and exit");
    schema_cmd->add_flag("--all", schema_all,
                         "include every cataloged result, item, and error schema");
    schema_cmd->add_option("command", schema_target, "command path (for example: account list)");
    CLI::App* raw_cmd = app.add_subcommand("raw", "reserved until M7");
    raw_cmd->set_help_flag();
    raw_cmd->fallthrough(false)->prefix_command();
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
    app.add_subcommand("resolve", "resolve a chat, username, or t.me link")
        ->add_option("selector", resolve_selector)
        ->required();
    CLI::App* chats_cmd = app.add_subcommand("chats", "list chats");
    chats.archived_option =
        chats_cmd->add_flag("--archived", chats.archived, "list archived chats");
    chats.folder_option =
        chats_cmd->add_option("--folder", chats.folder, "list chats in a folder id");
    chats.unread_option =
        chats_cmd->add_flag("--unread", chats.unread, "only include unread chats");
    chats.limit_option = chats_cmd->add_option("-n", chats.limit, "page size (1-100; default 20)");
    const auto add_read_options = [](CLI::App& command, ReadCliArguments& arguments) {
        arguments.chat_option = command.add_option("chat", arguments.chat, "chat selector");
        arguments.chat_option->expected(0, 1);
        arguments.before_option =
            command.add_option("--before", arguments.before, "exclusive message-id anchor");
        arguments.since_option =
            command.add_option("--since", arguments.since, "inclusive timestamp lower bound");
        arguments.until_option =
            command.add_option("--until", arguments.until, "inclusive timestamp upper bound");
        arguments.topic_option =
            command.add_option("--topic", arguments.topic, "topic kind:id selector");
        arguments.local_option =
            command.add_flag("--local", arguments.local, "use only local TDLib history");
        arguments.limit_option =
            command.add_option("-n", arguments.limit, "page size (1-100; default 20)");
    };
    CLI::App* read_cmd = app.add_subcommand("read", "read chat history");
    add_read_options(*read_cmd, read);
    CLI::App* history_cmd = app.add_subcommand("history", "alias for read");
    add_read_options(*history_cmd, history);
    app.add_subcommand("unread", "list chats with unread activity");
    CLI::App* fetch_cmd = app.add_subcommand("fetch", "warm local history for one chat");
    fetch_cmd->add_option("chat", fetch.chat, "chat selector")->required();
    fetch.limit_option =
        fetch_cmd->add_option("--limit", fetch.limit, "target history depth (1-1000000)");
    fetch.all_option = fetch_cmd->add_flag("--all", fetch.all, "fetch without a numeric limit");
    fetch.since_option =
        fetch_cmd->add_option("--since", fetch.since, "inclusive timestamp lower bound");
    CLI::App* msg_cmd = app.add_subcommand("msg", "message reads and mutations");
    msg_cmd->require_subcommand(1);
    CLI::App* msg_get_cmd = msg_cmd->add_subcommand("get", "get messages by id");
    msg_get_cmd->add_option("chat", messages.get_chat)->required();
    msg_get_cmd->add_option("id", messages.get_ids)->required()->expected(1, 100);
    CLI::App* msg_link_cmd = msg_cmd->add_subcommand("link", "get a message link");
    msg_link_cmd->add_option("chat", messages.link_chat)->required();
    msg_link_cmd->add_option("id", messages.link_id)->required();
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
    std::string remove_account;
    std::string reassign_default;
    bool keep_session = false;
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
    CLI::App* remove_cmd = account_cmd->add_subcommand("remove", "remove an account");
    remove_cmd->add_option("name", remove_account)->required();
    remove_cmd->add_flag("--keep-session", keep_session,
                         "remove local state without revoking the Telegram session");
    CLI::Option* reassign_default_option =
        remove_cmd->add_option("--reassign-default", reassign_default,
                               "new default when removing the current default account");

    CLI::App* saved_cmd = app.add_subcommand("saved", "Saved Messages reads");
    saved_cmd->require_subcommand(1);
    saved_cmd->add_subcommand("tags", "list Saved Messages reaction tags");
    CLI::App* saved_search_cmd =
        saved_cmd->add_subcommand("search", "search Saved Messages by reaction tag");
    saved.query_option =
        saved_search_cmd->add_option("query", saved.query, "optional exact text query");
    saved.query_option->expected(0, 1);
    saved.tag_option =
        saved_search_cmd->add_option("--tag", saved.tag, "exact emoji or custom:<id> tag");
    saved.limit_option =
        saved_search_cmd->add_option("-n", saved.limit, "page size (1-100; default 20)");

    if (const auto parse_exit = parse_arguments(app, argc, argv); parse_exit.has_value()) {
        return parse_exit.value();
    }

    if (rejected_bot_token_option->count() != 0) {
        rejected_bot_token.assign(rejected_bot_token.size(), '\0');
        rejected_bot_token.clear();
        return report_insecure_bot_token();
    }

    auto command = selected_command(app);
    const ReadCliArguments* selected_read = &read;
    if (command == std::vector<std::string>{"history"}) {
        command = {"read"};
        selected_read = &history;
    }
    if (const auto pre_routing_exit = handle_client_local_or_reserved_command(
            command, *account_option, *full_option, *allow_write_option, *yes_option,
            *dry_run_option, *timeout_option, timeout_seconds, *saved.cursor_option,
            *idempotency_key_option, *no_color_option, schema_target, schema_all, schema_help,
            verbose);
        pre_routing_exit) {
        return *pre_routing_exit;
    }
    if (command.empty()) {
        return report_missing_command();
    }
    if (const auto argument_exit = validate_command_arguments(
            command, saved, chats, messages, *selected_read, fetch, resolve_selector);
        argument_exit) {
        return *argument_exit;
    }
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

    if (const auto idempotency_exit =
            validate_idempotency_option(*idempotency_key_option, command, idempotency_key, dry_run);
        idempotency_exit) {
        return *idempotency_exit;
    }

    const bool supports_dry_run = command == std::vector<std::string>{"logout"} ||
                                  command == std::vector<std::string>{"account", "remove"} ||
                                  tgcli::proto::m3_operation_for_command(command).has_value();
    if (dry_run && !supports_dry_run) {
        const nlohmann::json rendered{
            {"error",
             {{"code", "USAGE"},
              {"message", "--dry-run is not supported for this command"},
              {"details", {{"argument", "--dry-run"}, {"reason", "unsupported_mode"}}}}}};
        std::fputs((rendered.dump() + "\n").c_str(), stderr);
        return tgcli::kUsage;
    }

    nlohmann::json request_args =
        command_request_args(command, login_qr, login_bot, resolve_selector, chats, saved, messages,
                             *selected_read, fetch);
    auto request_context = make_request_context(json_output, yes, dry_run, *folded_authority);
    if (idempotency_key_option->count() != 0) {
        request_context.idempotency_key = std::move(idempotency_key);
    }
    if (timeout_option->count() != 0) {
        request_context.timeout_seconds = timeout_seconds;
        if (!tgcli::request_deadline(request_context.timeout_seconds,
                                     tgcli::DeadlineDefault::Default60)) {
            return report_usage("invalid request timeout", "--timeout");
        }
    }

    const bool explicit_account = account_option->count() != 0;
    bool current_config_valid = true;
    std::optional<tgcli::cli::RoutingError> unavailable_route_error;
    if (const auto route_exit = resolve_request_account(
            command, explicit_account, account, request_args, current_config_valid,
            unavailable_route_error, add_account, show_account, use_account, remove_account,
            keep_session, reassign_default, reassign_default_option->count() != 0);
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
    const bool client_local_dry_run = tgcli::cli::uses_client_local_dry_run(command, dry_run);
    options.auto_spawn =
        command != std::vector<std::string>{"daemon", "status"} &&
        command != std::vector<std::string>{"daemon", "stop"} && !client_local_dry_run &&
        current_config_valid && !tgcli::cli::is_config_global_command(command) &&
        (command != std::vector<std::string>{"account", "remove"} || !keep_session);
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
