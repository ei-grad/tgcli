#include "cli/client.hpp"
#include "cli/routing.hpp"
#include "cli/schema_command.hpp"
#include "common/exit_codes.hpp"
#include "common/invite_link.hpp"
#include "common/paths.hpp"
#include "common/secure_wipe.hpp"
#include "daemon/chats_commands.hpp"
#include "daemon/daemon_run.hpp"
#include "daemon/fetch_domain.hpp"
#include "daemon/file_spool.hpp"
#include "daemon/local_selector.hpp"
#include "daemon/m6_domain.hpp"
#include "daemon/read_domain.hpp"
#include "daemon/request_fingerprint.hpp"
#include "daemon/resolver.hpp"
#include "daemon/saved_commands.hpp"
#include "daemon/stream_commands.hpp"
#include "daemon/write_domain.hpp"
#include "proto/frame.hpp"
#include "proto/operation.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
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

bool parse_signed_decimal(std::string_view raw, std::int64_t& value) noexcept {
    if (raw.empty()) {
        return false;
    }
    auto digits = raw;
    if (digits.front() == '+' || digits.front() == '-') {
        digits.remove_prefix(1);
    }
    if (digits.empty() || !std::ranges::all_of(digits, [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return false;
    }
    if (raw.front() == '+') {
        raw.remove_prefix(1);
    }
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return error == std::errc{} && end == raw.data() + raw.size();
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
                                                  tgcli::proto::WriteAuthority authority,
                                                  std::string frozen_cwd) {
    tgcli::proto::RequestContext context;
    context.tty = ::isatty(STDIN_FILENO) != 0;
    context.json = json_output;
    context.yes = yes;
    context.dry_run = dry_run;
    context.write_authority = authority;
    context.cwd = std::move(frozen_cwd);
    if (const char* media_dir = std::getenv("TGCLI_MEDIA_DIR");
        media_dir != nullptr && *media_dir != '\0') {
        context.media_dir = media_dir;
    }
    return context;
}

std::optional<std::string> capture_request_cwd() {
    constexpr std::size_t maximum_cwd_bytes = 4096;
    std::vector<char> buffer(256);
    for (;;) {
        errno = 0;
        if (::getcwd(buffer.data(), buffer.size()) != nullptr) {
            return std::string(buffer.data());
        }
        if (errno != ERANGE || buffer.size() == maximum_cwd_bytes + 1) {
            return std::nullopt;
        }
        buffer.resize(std::min(buffer.size() * 2, maximum_cwd_bytes + 1));
    }
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
    const auto message = argument == "--full" ? "--full is reserved through v1"
                                              : std::string(argument) + " is reserved until M7";
    const nlohmann::json rendered{
        {"error",
         {{"code", "USAGE"},
          {"message", message},
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
    const CLI::Option& idempotency_key, const std::vector<std::string>& schema_target,
    bool schema_all, bool schema_help, bool verbose) {
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
    const auto m6 = tgcli::proto::m6_operation_for_command(command);
    const auto* m6_identity = m6 ? tgcli::proto::m6_operation_identity(*m6) : nullptr;
    if (!tgcli::proto::m3_operation_for_command(command) &&
        (m6_identity == nullptr || !m6_identity->idempotent)) {
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
    std::string attach_message_id_raw;
    std::int64_t attach_message_id = 0;
    bool attach_message_id_valid = true;
    std::string attach_path;
    std::string attach_caption;
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
    std::string delete_chat;
    std::vector<std::int64_t> delete_ids;
    bool delete_for_all = false;
    bool delete_has_duplicate = false;
    std::string forward_from;
    std::vector<std::string> forward_positionals;
    std::vector<std::int64_t> forward_ids;
    std::string forward_to;
    bool forward_drop_author = false;
    bool forward_ids_valid = true;
    std::string edit_chat;
    std::int64_t edit_id = 0;
    std::string edit_text;
    std::string react_chat;
    std::int64_t react_id = 0;
    std::string reaction;
    bool react_remove = false;
    bool react_big = false;
    std::string pin_chat;
    std::int64_t pin_id = 0;
    std::string unpin_chat;
    std::int64_t unpin_id = 0;
};

struct SendCliArguments {
    std::string chat;
    std::string text;
    std::int64_t reply_to = 0;
    std::string topic;
    std::string schedule;
    bool markdown = false;
    bool html = false;
    bool silent = false;
    CLI::Option* reply_option = nullptr;
    CLI::Option* topic_option = nullptr;
    CLI::Option* schedule_option = nullptr;
};

struct ChatCliArguments {
    std::string mark_read;
    std::string mute;
    std::string mute_for;
    std::string unmute;
    std::string pin;
    std::string unpin;
    std::string archive;
    std::string unarchive;
    std::string join;
    std::string leave;
    CLI::Option* mute_for_option = nullptr;
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

struct StreamCliArguments {
    std::vector<std::string> listen_chats;
    std::string listen_types;
    std::string listen_count_raw;
    std::optional<std::uint64_t> listen_count;
    std::vector<std::string> listen_type_values;
    std::string wait_chat;
    std::string wait_from;
    std::string wait_regex;
    std::string wait_after_raw;
    std::optional<std::int64_t> wait_after;
    CLI::Option* listen_types_option = nullptr;
    CLI::Option* listen_count_option = nullptr;
    CLI::Option* wait_chat_option = nullptr;
    CLI::Option* wait_from_option = nullptr;
    CLI::Option* wait_regex_option = nullptr;
    CLI::Option* wait_after_option = nullptr;
};

struct M6CliArguments {
    std::string contact_query;
    std::string contact_user;
    std::string folder_id_raw;
    std::int32_t folder_id = 0;
    std::string folder_name;
    std::vector<std::string> folder_chats;
    std::string folder_chat;
    std::string folder_icon;
    std::int64_t folder_color = -1;
    CLI::Option* folder_name_option = nullptr;
    CLI::Option* folder_create_icon_option = nullptr;
    CLI::Option* folder_create_color_option = nullptr;
    CLI::Option* folder_edit_icon_option = nullptr;
    CLI::Option* folder_edit_color_option = nullptr;
    std::string topic_chat;
    std::string topic_id_raw;
    std::int32_t topic_id = 0;
    std::string topic_name;
    std::string topic_icon = "blue";
    std::string admin_chat;
    std::string admin_title;
    std::string photo_path;
    bool photo_delete = false;
    CLI::Option* photo_path_option = nullptr;
    CLI::Option* photo_delete_option = nullptr;
    std::string admin_description;
    std::string invite_revoke;
    CLI::Option* invite_revoke_option = nullptr;
    std::string admin_user;
    std::string admin_rights;
    std::vector<std::string> normalized_admin_rights;
    std::string admin_permissions;
    std::vector<std::string> normalized_permissions;
    std::string session_id;
};

std::optional<std::vector<std::string>>
parse_m6_closed_list(std::string_view raw, std::span<const std::string_view> allowed,
                     bool allow_none) {
    if (allow_none && raw == "none") {
        return std::vector<std::string>{};
    }
    if (raw.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> values;
    for (std::size_t begin = 0;;) {
        const auto end = raw.find(',', begin);
        const auto value = raw.substr(begin, end - begin);
        if (value.empty() || value == "none" ||
            std::find(allowed.begin(), allowed.end(), value) == allowed.end() ||
            std::ranges::find(values, value) != values.end()) {
            return std::nullopt;
        }
        values.emplace_back(value);
        if (end == std::string_view::npos) {
            return values;
        }
        begin = end + 1;
    }
}

bool parse_canonical_int64(std::string_view value, std::int64_t& parsed) noexcept {
    if (value.empty() || value == "-0" || value.front() == '+' ||
        (value.size() > 1 && value.front() == '0') ||
        (value.size() > 2 && value.starts_with("-0"))) {
        return false;
    }
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() &&
           std::to_string(parsed) == value;
}

bool parse_canonical_positive_decimal(std::string_view raw, std::uint64_t maximum,
                                      std::uint64_t& value) noexcept {
    if (raw.empty() || raw.front() == '0' || !std::ranges::all_of(raw, [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return false;
    }
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return error == std::errc{} && end == raw.data() + raw.size() && value <= maximum;
}

std::vector<std::string> split_stream_types(std::string_view raw) {
    std::vector<std::string> values;
    for (std::size_t begin = 0;;) {
        const auto end = raw.find(',', begin);
        values.emplace_back(raw.substr(begin, end - begin));
        if (end == std::string_view::npos) {
            return values;
        }
        begin = end + 1;
    }
}

nlohmann::json stream_request_args(const std::vector<std::string>& command,
                                   const StreamCliArguments& stream) {
    if (command == std::vector<std::string>{"listen"}) {
        return {{"chats", stream.listen_chats},
                {"types", stream.listen_types_option->count() != 0
                              ? nlohmann::json(stream.listen_type_values)
                              : nlohmann::json(nullptr)},
                {"count", stream.listen_count ? nlohmann::json(*stream.listen_count)
                                              : nlohmann::json(nullptr)}};
    }
    return {{"chat", stream.wait_chat_option->count() != 0 ? nlohmann::json(stream.wait_chat)
                                                           : nlohmann::json(nullptr)},
            {"from", stream.wait_from_option->count() != 0 ? nlohmann::json(stream.wait_from)
                                                           : nlohmann::json(nullptr)},
            {"regex", stream.wait_regex_option->count() != 0 ? nlohmann::json(stream.wait_regex)
                                                             : nlohmann::json(nullptr)},
            {"after",
             stream.wait_after ? nlohmann::json(*stream.wait_after) : nlohmann::json(nullptr)}};
}

std::optional<int> normalize_stream_arguments(const std::vector<std::string>& command,
                                              StreamCliArguments& stream,
                                              std::optional<double> timeout_seconds) {
    const bool listen = command == std::vector<std::string>{"listen"};
    const bool wait_for = command == std::vector<std::string>{"wait-for"};
    if (!listen && !wait_for) {
        return std::nullopt;
    }
    if (const auto error = tgcli::daemon::validate_stream_timeout(timeout_seconds)) {
        return report_usage(error->message, error->argument, error->reason);
    }
    if (listen) {
        if (stream.listen_count_option->count() != 0) {
            std::uint64_t count = 0;
            if (!parse_canonical_positive_decimal(stream.listen_count_raw,
                                                  tgcli::daemon::kMaximumStreamCount, count)) {
                return report_usage("listen count must be a canonical integer from 1 to 1000000",
                                    "--count");
            }
            stream.listen_count = count;
        }
        if (stream.listen_types_option->count() != 0) {
            stream.listen_type_values = split_stream_types(stream.listen_types);
        }
        const auto parsed =
            tgcli::daemon::parse_listen_arguments(stream_request_args(command, stream));
        if (const auto* error = std::get_if<tgcli::daemon::StreamArgumentError>(&parsed)) {
            return report_usage(error->message, error->argument, error->reason);
        }
        return std::nullopt;
    }
    if (stream.wait_after_option->count() != 0) {
        std::uint64_t after = 0;
        if (!parse_canonical_positive_decimal(stream.wait_after_raw,
                                              tgcli::daemon::kMaximumStreamMessageId, after)) {
            return report_usage("wait-for after must be a canonical positive int53", "--after");
        }
        stream.wait_after = static_cast<std::int64_t>(after);
    }
    const auto parsed =
        tgcli::daemon::parse_wait_for_arguments(stream_request_args(command, stream));
    if (const auto* error = std::get_if<tgcli::daemon::StreamArgumentError>(&parsed)) {
        return report_usage(error->message, error->argument, error->reason);
    }
    if (stream.wait_regex_option->count() != 0) {
        const auto compiled = tgcli::daemon::compile_stream_regex(stream.wait_regex);
        if (const auto* error = std::get_if<tgcli::daemon::StreamArgumentError>(&compiled)) {
            return report_usage(error->message, error->argument, error->reason);
        }
    }
    return std::nullopt;
}

bool is_saved_tags(const std::vector<std::string>& command) {
    return command == std::vector<std::string>{"saved", "tags"};
}

bool is_saved_search(const std::vector<std::string>& command) {
    return command == std::vector<std::string>{"saved", "search"};
}

bool is_saved_attach(const std::vector<std::string>& command) {
    return command == std::vector<std::string>{"saved", "attach"};
}

std::optional<int> validate_saved_attach_arguments(const SavedCliArguments& saved,
                                                   std::string_view frozen_cwd) {
    constexpr std::int64_t maximum_int53 = 9007199254740991LL;
    if (!saved.attach_message_id_valid || saved.attach_message_id == 0 ||
        saved.attach_message_id < -maximum_int53 || saved.attach_message_id > maximum_int53) {
        return report_usage("saved attach message id must be a nonzero int53 value", "message-id");
    }
    if (!tgcli::daemon::canonical_source_display_path(saved.attach_path, frozen_cwd)) {
        return report_usage("saved attach path is invalid", "PATH");
    }
    if (!tgcli::daemon::valid_saved_attach_caption(saved.attach_caption)) {
        return report_usage("saved attach caption must contain at most 1024 Unicode scalars",
                            "--caption");
    }
    return std::nullopt;
}

std::optional<int> validate_saved_arguments(const std::vector<std::string>& command,
                                            const SavedCliArguments& saved,
                                            std::string_view frozen_cwd) {
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
        if (!is_saved_attach(command)) {
            return std::nullopt;
        }
        return validate_saved_attach_arguments(saved, frozen_cwd);
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed 30-command grammar table.
std::optional<int> validate_m6_arguments(const std::vector<std::string>& command,
                                         M6CliArguments& m6, std::string_view frozen_cwd) {
    const auto operation = tgcli::proto::m6_operation_for_command(command);
    if (!operation) {
        if (command == std::vector<std::string>{"session", "terminate"}) {
            std::int64_t ignored = 0;
            if (!parse_canonical_int64(m6.session_id, ignored)) {
                return report_usage("session id must use canonical signed int64 spelling",
                                    "session-id");
            }
        }
        return std::nullopt;
    }
    using O = tgcli::proto::M6Operation;
    const auto exact_selector = [](std::string_view value) {
        return tgcli::daemon::valid_m6_exact_selector(value);
    };
    switch (*operation) {
    case O::ContactList:
        return std::nullopt;
    case O::ContactSearch:
        return tgcli::daemon::valid_m6_contact_query(m6.contact_query)
                   ? std::nullopt
                   : std::optional<int>{
                         report_usage("contact query must contain 1..256 UTF-8 bytes", "query")};
    case O::ContactAdd:
    case O::ContactRemove:
    case O::ContactBlock:
    case O::ContactUnblock:
        return exact_selector(m6.contact_user)
                   ? std::nullopt
                   : std::optional<int>{
                         report_usage("contact mutation requires an exact user selector", "user")};
    case O::FolderList:
        return std::nullopt;
    case O::FolderShow:
    case O::FolderDelete:
    case O::FolderAddChat:
    case O::FolderRemoveChat: {
        const auto folder_id = tgcli::daemon::parse_m6_positive_int32(m6.folder_id_raw);
        if (!folder_id) {
            return report_usage("folder id must be a canonical positive int32", "folder-id");
        }
        m6.folder_id = *folder_id;
        if ((*operation == O::FolderAddChat || *operation == O::FolderRemoveChat) &&
            !exact_selector(m6.folder_chat)) {
            return report_usage("folder membership requires an exact chat selector", "chat");
        }
        return std::nullopt;
    }
    case O::FolderCreate:
        if (!tgcli::daemon::valid_m6_canonical_text(tgcli::daemon::M6TextKind::FolderName,
                                                    m6.folder_name)) {
            return report_usage("folder name is not canonical", "name");
        }
        if (m6.folder_chats.empty() || m6.folder_chats.size() > 100 ||
            !std::ranges::all_of(m6.folder_chats, exact_selector)) {
            return report_usage("folder create requires 1..100 exact chat selectors", "--chat");
        }
        if (m6.folder_create_icon_option->count() != 0 &&
            !tgcli::daemon::parse_m6_folder_icon(m6.folder_icon)) {
            return report_usage("folder icon is invalid", "--icon");
        }
        if (m6.folder_color < -1 || m6.folder_color > 6) {
            return report_usage("folder color must be between -1 and 6", "--color");
        }
        return std::nullopt;
    case O::FolderEdit: {
        const auto folder_id = tgcli::daemon::parse_m6_positive_int32(m6.folder_id_raw);
        if (!folder_id) {
            return report_usage("folder id must be a canonical positive int32", "folder-id");
        }
        m6.folder_id = *folder_id;
        if (m6.folder_name_option->count() == 0 && m6.folder_edit_icon_option->count() == 0 &&
            m6.folder_edit_color_option->count() == 0) {
            return report_usage("folder edit requires a changed field", "folder");
        }
        if (m6.folder_name_option->count() != 0 &&
            !tgcli::daemon::valid_m6_canonical_text(tgcli::daemon::M6TextKind::FolderName,
                                                    m6.folder_name)) {
            return report_usage("folder name is not canonical", "--name");
        }
        if (m6.folder_edit_icon_option->count() != 0 && m6.folder_icon != "default" &&
            !tgcli::daemon::parse_m6_folder_icon(m6.folder_icon)) {
            return report_usage("folder icon is invalid", "--icon");
        }
        if (m6.folder_edit_color_option->count() != 0 &&
            (m6.folder_color < -1 || m6.folder_color > 6)) {
            return report_usage("folder color must be between -1 and 6", "--color");
        }
        return std::nullopt;
    }
    case O::TopicList:
        return exact_selector(m6.topic_chat)
                   ? std::nullopt
                   : std::optional<int>{
                         report_usage("topic list requires an exact chat selector", "chat")};
    case O::TopicCreate:
        if (!exact_selector(m6.topic_chat) ||
            !tgcli::daemon::valid_m6_canonical_text(tgcli::daemon::M6TextKind::TopicName,
                                                    m6.topic_name) ||
            !tgcli::daemon::parse_m6_topic_color(m6.topic_icon)) {
            return report_usage("topic create arguments are invalid", "topic");
        }
        return std::nullopt;
    case O::TopicEdit:
    case O::TopicClose:
    case O::TopicReopen: {
        const auto topic_id = tgcli::daemon::parse_m6_positive_int32(m6.topic_id_raw);
        if (!exact_selector(m6.topic_chat) || !topic_id) {
            return report_usage("topic target is invalid", "topic-id");
        }
        m6.topic_id = *topic_id;
        if (*operation == O::TopicEdit &&
            !tgcli::daemon::valid_m6_canonical_text(tgcli::daemon::M6TextKind::TopicName,
                                                    m6.topic_name)) {
            return report_usage("topic name is not canonical", "name");
        }
        return std::nullopt;
    }
    case O::ChatSetTitle:
        return exact_selector(m6.admin_chat) &&
                       tgcli::daemon::valid_m6_canonical_text(tgcli::daemon::M6TextKind::ChatTitle,
                                                              m6.admin_title)
                   ? std::nullopt
                   : std::optional<int>{report_usage("chat title arguments are invalid", "title")};
    case O::ChatSetPhoto:
        if (!exact_selector(m6.admin_chat) ||
            (m6.photo_path_option->count() == 0) == !m6.photo_delete) {
            return report_usage("set-photo requires exactly one PATH or --delete", "photo",
                                "mutually_exclusive");
        }
        if (!m6.photo_delete &&
            !tgcli::daemon::canonical_source_display_path(m6.photo_path, frozen_cwd)) {
            return report_usage("set-photo path is invalid", "PATH");
        }
        return std::nullopt;
    case O::ChatSetDescription:
        return exact_selector(m6.admin_chat) &&
                       tgcli::daemon::valid_m6_canonical_text(
                           tgcli::daemon::M6TextKind::ChatDescription, m6.admin_description)
                   ? std::nullopt
                   : std::optional<int>{
                         report_usage("chat description arguments are invalid", "description")};
    case O::ChatInviteLink:
        if (!exact_selector(m6.admin_chat) ||
            (m6.invite_revoke_option->count() != 0 &&
             !tgcli::daemon::valid_m6_invite_link(m6.invite_revoke))) {
            return report_usage("chat invite-link arguments are invalid", "invite-link");
        }
        return std::nullopt;
    case O::ChatPromote: {
        const auto rights =
            parse_m6_closed_list(m6.admin_rights, tgcli::daemon::m6_admin_right_names(), false);
        if (!exact_selector(m6.admin_chat) || !exact_selector(m6.admin_user) || !rights) {
            return report_usage("chat promote arguments are invalid", "--rights");
        }
        m6.normalized_admin_rights = *rights;
        return std::nullopt;
    }
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick:
        return exact_selector(m6.admin_chat) && exact_selector(m6.admin_user)
                   ? std::nullopt
                   : std::optional<int>{report_usage("chat member target is invalid", "user")};
    case O::ChatSetPermissions: {
        const auto permissions = parse_m6_closed_list(
            m6.admin_permissions, tgcli::daemon::m6_chat_permission_names(), true);
        if (!exact_selector(m6.admin_chat) || !permissions) {
            return report_usage("chat permissions are invalid", "--permissions");
        }
        m6.normalized_permissions = *permissions;
        return std::nullopt;
    }
    case O::StorageStats:
    case O::StorageOptimize:
        return std::nullopt;
    }
    return report_usage("M6 command is invalid", "command");
}

// Each command's closed CLI contract is validated before common request construction.
// NOLINTBEGIN(readability-function-cognitive-complexity)
std::optional<int>
validate_command_arguments(const std::vector<std::string>& command, const SavedCliArguments& saved,
                           const ChatsCliArguments& chats, const MessageCliArguments& messages,
                           const SendCliArguments& send, const ChatCliArguments& chat,
                           const ReadCliArguments& read, const FetchCliArguments& fetch,
                           std::string_view resolve_selector, M6CliArguments& m6,
                           std::string_view frozen_cwd) {
    if (const auto m6_exit = validate_m6_arguments(command, m6, frozen_cwd); m6_exit) {
        return m6_exit;
    }
    if (const auto saved_exit = validate_saved_arguments(command, saved, frozen_cwd); saved_exit) {
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
    if (command == std::vector<std::string>{"send"}) {
        if (tgcli::daemon::classify_exact_write_selector(send.chat) ==
            tgcli::daemon::ExactWriteSelectorStatus::Invalid) {
            return report_usage("send chat selector is invalid", "chat");
        }
        if (!tgcli::daemon::valid_send_text(send.text)) {
            return report_usage("send text must contain between 1 and 4096 Unicode scalars",
                                "TEXT");
        }
        if (send.reply_option->count() != 0 && !valid_message_id(send.reply_to)) {
            return report_usage("--reply-to must be a nonzero int53 message id", "--reply-to");
        }
        if (send.topic_option->count() != 0 && !tgcli::daemon::parse_send_topic(send.topic)) {
            const char* const reason =
                send.topic.find(':') != std::string::npos && !send.topic.starts_with("forum:")
                    ? "unsupported_topic_kind"
                    : "invalid_argument";
            return report_usage("invalid send topic", "--topic", reason);
        }
        if (send.schedule_option->count() != 0 &&
            !tgcli::daemon::parse_send_schedule(send.schedule)) {
            return report_usage("invalid send schedule", "--schedule");
        }
    }
    if (command == std::vector<std::string>{"msg", "delete"}) {
        if (tgcli::daemon::classify_exact_write_selector(messages.delete_chat) ==
            tgcli::daemon::ExactWriteSelectorStatus::Invalid) {
            return report_usage("msg delete chat selector is invalid", "chat");
        }
        if (messages.delete_ids.empty() || messages.delete_ids.size() > 100 ||
            messages.delete_has_duplicate ||
            !std::ranges::all_of(messages.delete_ids, valid_message_id)) {
            return report_usage("msg delete requires 1 to 100 unique nonzero int53 message ids",
                                "id");
        }
    }
    if (command == std::vector<std::string>{"msg", "forward"}) {
        if (tgcli::daemon::classify_exact_write_selector(messages.forward_from) ==
                tgcli::daemon::ExactWriteSelectorStatus::Invalid ||
            tgcli::daemon::classify_exact_write_selector(messages.forward_to) ==
                tgcli::daemon::ExactWriteSelectorStatus::Invalid) {
            return report_usage("msg forward chat selector is invalid", "from|to");
        }
        if (!messages.forward_ids_valid || messages.forward_ids.empty() ||
            messages.forward_ids.size() > 100 ||
            !std::ranges::all_of(messages.forward_ids, valid_message_id) ||
            std::adjacent_find(messages.forward_ids.begin(), messages.forward_ids.end(),
                               std::greater_equal<>{}) != messages.forward_ids.end()) {
            return report_usage(
                "msg forward requires 1 to 100 strictly increasing nonzero int53 message ids",
                "id");
        }
    }
    if (command == std::vector<std::string>{"msg", "edit"}) {
        if (tgcli::daemon::classify_exact_write_selector(messages.edit_chat) ==
            tgcli::daemon::ExactWriteSelectorStatus::Invalid) {
            return report_usage("msg edit chat selector is invalid", "chat");
        }
        if (!valid_message_id(messages.edit_id)) {
            return report_usage("message id must be a nonzero int53 value", "id");
        }
        if (!tgcli::daemon::valid_send_text(messages.edit_text)) {
            return report_usage("msg edit text must contain between 1 and 4096 Unicode scalars",
                                "TEXT");
        }
    }
    if (command == std::vector<std::string>{"msg", "react"}) {
        if (tgcli::daemon::classify_exact_write_selector(messages.react_chat) ==
            tgcli::daemon::ExactWriteSelectorStatus::Invalid) {
            return report_usage("msg react chat selector is invalid", "chat");
        }
        if (!valid_message_id(messages.react_id)) {
            return report_usage("message id must be a nonzero int53 value", "id");
        }
        if (!tgcli::daemon::valid_message_reaction(messages.reaction)) {
            return report_usage("msg react emoji must be valid UTF-8 between 1 and 64 bytes",
                                "emoji");
        }
        if (messages.react_remove && messages.react_big) {
            return report_usage("--remove and --big are mutually exclusive", "--remove/--big",
                                "mutually_exclusive");
        }
    }
    if (command == std::vector<std::string>{"msg", "pin"} ||
        command == std::vector<std::string>{"msg", "unpin"}) {
        const bool pin = command.back() == "pin";
        const auto& chat = pin ? messages.pin_chat : messages.unpin_chat;
        const auto id = pin ? messages.pin_id : messages.unpin_id;
        if (tgcli::daemon::classify_exact_write_selector(chat) ==
            tgcli::daemon::ExactWriteSelectorStatus::Invalid) {
            return report_usage("message pin chat selector is invalid", "chat");
        }
        if (!valid_message_id(id)) {
            return report_usage("message id must be a nonzero int53 value", "id");
        }
    }
    const auto* const chat_target = [&]() -> const std::string* {
        if (command == std::vector<std::string>{"chat", "mark-read"}) {
            return &chat.mark_read;
        }
        if (command == std::vector<std::string>{"chat", "mute"}) {
            return &chat.mute;
        }
        if (command == std::vector<std::string>{"chat", "unmute"}) {
            return &chat.unmute;
        }
        if (command == std::vector<std::string>{"chat", "pin"}) {
            return &chat.pin;
        }
        if (command == std::vector<std::string>{"chat", "unpin"}) {
            return &chat.unpin;
        }
        if (command == std::vector<std::string>{"chat", "archive"}) {
            return &chat.archive;
        }
        if (command == std::vector<std::string>{"chat", "unarchive"}) {
            return &chat.unarchive;
        }
        if (command == std::vector<std::string>{"chat", "leave"}) {
            return &chat.leave;
        }
        return nullptr;
    }();
    if (chat_target != nullptr && tgcli::daemon::classify_exact_write_selector(*chat_target) ==
                                      tgcli::daemon::ExactWriteSelectorStatus::Invalid) {
        return report_usage("chat selector is invalid", "chat");
    }
    if (command == std::vector<std::string>{"chat", "mute"} && chat.mute_for_option->count() != 0 &&
        !tgcli::daemon::parse_mute_duration(chat.mute_for)) {
        return report_usage("chat mute duration is invalid", "--for");
    }
    if (command == std::vector<std::string>{"chat", "join"}) {
        const auto canonical = tgcli::daemon::canonical_write_selector(chat.join);
        if (!canonical || (chat.join.starts_with('@') ? *canonical != chat.join
                                                      : !canonical->starts_with("sha256:"))) {
            return report_usage("chat join requires an invite link or @username",
                                "invite-link|@username");
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

nlohmann::json m6_request_args(const std::vector<std::string>& command, const M6CliArguments& m6) {
    const auto operation = tgcli::proto::m6_operation_for_command(command);
    if (!operation) {
        return command == std::vector<std::string>{"session", "terminate"}
                   ? nlohmann::json{{"session_id", m6.session_id}}
                   : nlohmann::json::object();
    }
    using O = tgcli::proto::M6Operation;
    switch (*operation) {
    case O::ContactList:
    case O::FolderList:
    case O::StorageStats:
    case O::StorageOptimize:
        return nlohmann::json::object();
    case O::ContactSearch:
        return {{"query", m6.contact_query}};
    case O::ContactAdd:
    case O::ContactRemove:
    case O::ContactBlock:
    case O::ContactUnblock:
        return {{"user", m6.contact_user}};
    case O::FolderShow:
    case O::FolderDelete:
        return {{"folder_id", m6.folder_id}};
    case O::FolderCreate:
        return {{"name", m6.folder_name},
                {"chats", m6.folder_chats},
                {"icon", m6.folder_create_icon_option->count() != 0 ? nlohmann::json(m6.folder_icon)
                                                                    : nlohmann::json(nullptr)},
                {"color_id", static_cast<std::int32_t>(m6.folder_color)}};
    case O::FolderEdit:
        return {{"folder_id", m6.folder_id},
                {"name", m6.folder_name_option->count() != 0 ? nlohmann::json(m6.folder_name)
                                                             : nlohmann::json(nullptr)},
                {"icon", m6.folder_edit_icon_option->count() != 0 && m6.folder_icon != "default"
                             ? nlohmann::json(m6.folder_icon)
                             : nlohmann::json(nullptr)},
                {"use_default_icon",
                 m6.folder_edit_icon_option->count() != 0 && m6.folder_icon == "default"},
                {"color_id", m6.folder_edit_color_option->count() != 0
                                 ? nlohmann::json(static_cast<std::int32_t>(m6.folder_color))
                                 : nlohmann::json(nullptr)}};
    case O::FolderAddChat:
    case O::FolderRemoveChat:
        return {{"folder_id", m6.folder_id}, {"chat", m6.folder_chat}};
    case O::TopicList:
        return {{"chat", m6.topic_chat}};
    case O::TopicCreate:
        return {{"chat", m6.topic_chat}, {"name", m6.topic_name}, {"icon", m6.topic_icon}};
    case O::TopicEdit:
        return {{"chat", m6.topic_chat}, {"topic_id", m6.topic_id}, {"name", m6.topic_name}};
    case O::TopicClose:
    case O::TopicReopen:
        return {{"chat", m6.topic_chat}, {"topic_id", m6.topic_id}};
    case O::ChatSetTitle:
        return {{"chat", m6.admin_chat}, {"title", m6.admin_title}};
    case O::ChatSetPhoto:
        return m6.photo_delete ? nlohmann::json{{"chat", m6.admin_chat}, {"delete", true}}
                               : nlohmann::json{{"chat", m6.admin_chat}, {"path", m6.photo_path}};
    case O::ChatSetDescription:
        return {{"chat", m6.admin_chat}, {"description", m6.admin_description}};
    case O::ChatInviteLink:
        return {{"chat", m6.admin_chat},
                {"revoke", m6.invite_revoke_option->count() != 0 ? nlohmann::json(m6.invite_revoke)
                                                                 : nlohmann::json(nullptr)}};
    case O::ChatPromote:
        return {{"chat", m6.admin_chat},
                {"user", m6.admin_user},
                {"rights", m6.normalized_admin_rights}};
    case O::ChatDemote:
    case O::ChatBan:
    case O::ChatUnban:
    case O::ChatKick:
        return {{"chat", m6.admin_chat}, {"user", m6.admin_user}};
    case O::ChatSetPermissions:
        return {{"chat", m6.admin_chat}, {"permissions", m6.normalized_permissions}};
    }
    return nlohmann::json::object();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed CLI request shape table.
nlohmann::json command_request_args(const std::vector<std::string>& command, bool login_qr,
                                    bool login_bot, std::string_view resolve_selector,
                                    const ChatsCliArguments& chats, const SavedCliArguments& saved,
                                    const MessageCliArguments& messages,
                                    const SendCliArguments& send, const ChatCliArguments& chat,
                                    const ReadCliArguments& read, const FetchCliArguments& fetch,
                                    const StreamCliArguments& stream, const M6CliArguments& m6) {
    if (tgcli::proto::m6_operation_for_command(command) ||
        command == std::vector<std::string>{"session", "list"} ||
        command == std::vector<std::string>{"session", "terminate"}) {
        return m6_request_args(command, m6);
    }
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
    if (command == std::vector<std::string>{"listen"} ||
        command == std::vector<std::string>{"wait-for"}) {
        return stream_request_args(command, stream);
    }
    if (is_saved_search(command)) {
        return saved_search_request_args(saved);
    }
    if (is_saved_attach(command)) {
        return {{"message_id", saved.attach_message_id},
                {"path", saved.attach_path},
                {"caption", saved.attach_caption}};
    }
    if (command == std::vector<std::string>{"msg", "get"}) {
        return {{"chat", messages.get_chat}, {"message_ids", messages.get_ids}};
    }
    if (command == std::vector<std::string>{"msg", "link"}) {
        return {{"chat", messages.link_chat}, {"message_id", messages.link_id}};
    }
    if (command == std::vector<std::string>{"send"}) {
        const auto topic = send.topic_option->count() != 0
                               ? tgcli::daemon::parse_send_topic(send.topic)
                               : std::nullopt;
        const auto schedule = send.schedule_option->count() != 0
                                  ? tgcli::daemon::parse_send_schedule(send.schedule)
                                  : std::nullopt;
        nlohmann::json schedule_value = nullptr;
        if (schedule) {
            schedule_value =
                schedule->kind == tgcli::daemon::SendScheduleKind::Online
                    ? nlohmann::json{{"kind", "online"}}
                    : nlohmann::json{{"kind", "at"}, {"send_date", schedule->send_date}};
        }
        const char* parse_mode = "plain";
        if (send.markdown) {
            parse_mode = "markdown_v2";
        } else if (send.html) {
            parse_mode = "html";
        }
        return {{"chat", send.chat},
                {"text", send.text},
                {"parse_mode", parse_mode},
                {"reply_to", send.reply_option->count() != 0 ? nlohmann::json(send.reply_to)
                                                             : nlohmann::json(nullptr)},
                {"topic", topic ? tgcli::daemon::topic_ref_json(*topic) : nlohmann::json(nullptr)},
                {"silent", send.silent},
                {"schedule", std::move(schedule_value)}};
    }
    if (command == std::vector<std::string>{"msg", "delete"}) {
        return {{"chat", messages.delete_chat},
                {"message_ids", messages.delete_ids},
                {"for_all", messages.delete_for_all}};
    }
    if (command == std::vector<std::string>{"msg", "forward"}) {
        return {{"from", messages.forward_from},
                {"to", messages.forward_to},
                {"message_ids", messages.forward_ids},
                {"drop_author", messages.forward_drop_author}};
    }
    if (command == std::vector<std::string>{"msg", "edit"}) {
        return {{"chat", messages.edit_chat},
                {"message_id", messages.edit_id},
                {"text", messages.edit_text}};
    }
    if (command == std::vector<std::string>{"msg", "react"}) {
        return {{"chat", messages.react_chat},
                {"message_id", messages.react_id},
                {"reaction", messages.reaction},
                {"remove", messages.react_remove},
                {"big", messages.react_big}};
    }
    if (command == std::vector<std::string>{"msg", "pin"}) {
        return {{"chat", messages.pin_chat}, {"message_id", messages.pin_id}};
    }
    if (command == std::vector<std::string>{"msg", "unpin"}) {
        return {{"chat", messages.unpin_chat}, {"message_id", messages.unpin_id}};
    }
    if (command == std::vector<std::string>{"chat", "mark-read"}) {
        return {{"chat", chat.mark_read}};
    }
    if (command == std::vector<std::string>{"chat", "mute"}) {
        const auto duration = chat.mute_for_option->count() != 0
                                  ? tgcli::daemon::parse_mute_duration(chat.mute_for)
                                        .value_or(std::numeric_limits<std::int32_t>::max())
                                  : std::numeric_limits<std::int32_t>::max();
        return {{"chat", chat.mute}, {"duration_seconds", duration}};
    }
    if (command == std::vector<std::string>{"chat", "unmute"}) {
        return {{"chat", chat.unmute}, {"duration_seconds", 0}};
    }
    if (command == std::vector<std::string>{"chat", "pin"}) {
        return {{"chat", chat.pin}};
    }
    if (command == std::vector<std::string>{"chat", "unpin"}) {
        return {{"chat", chat.unpin}};
    }
    if (command == std::vector<std::string>{"chat", "archive"}) {
        return {{"chat", chat.archive}};
    }
    if (command == std::vector<std::string>{"chat", "unarchive"}) {
        return {{"chat", chat.unarchive}};
    }
    if (command == std::vector<std::string>{"chat", "join"}) {
        return {{"target", chat.join}};
    }
    if (command == std::vector<std::string>{"chat", "leave"}) {
        return {{"chat", chat.leave}};
    }
    return nlohmann::json::object();
}

std::optional<int> read_send_stdin(SendCliArguments& send) {
    if (send.text != "-") {
        return std::nullopt;
    }
    constexpr std::size_t maximum = static_cast<std::size_t>(1024) * 1024;
    std::string value;
    std::array<char, 65'536> buffer{};
    for (;;) {
        const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return report_usage("cannot read send text from stdin", "TEXT");
        }
        if (count == 0) {
            break;
        }
        const auto added = static_cast<std::size_t>(count);
        if (added > maximum - value.size()) {
            return report_usage("send stdin exceeds 1 MiB", "TEXT");
        }
        value.append(buffer.data(), added);
    }
    send.text = std::move(value);
    return std::nullopt;
}

std::optional<int> read_edit_stdin(MessageCliArguments& messages) {
    if (messages.edit_text != "-") {
        return std::nullopt;
    }
    constexpr std::size_t maximum = static_cast<std::size_t>(1024) * 1024;
    std::string value;
    std::array<char, 65'536> buffer{};
    for (;;) {
        const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return report_usage("cannot read msg edit text from stdin", "TEXT");
        }
        if (count == 0) {
            break;
        }
        const auto added = static_cast<std::size_t>(count);
        if (added > maximum - value.size()) {
            return report_usage("msg edit stdin exceeds 1 MiB", "TEXT");
        }
        value.append(buffer.data(), added);
    }
    messages.edit_text = std::move(value);
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed CLI grammar and routing table.
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
    SendCliArguments send;
    ChatCliArguments chat;
    ReadCliArguments read;
    ReadCliArguments history;
    FetchCliArguments fetch;
    StreamCliArguments stream;
    M6CliArguments m6;
    CLI::Option* account_option =
        app.add_option("--account", account, "account name (default from config / TGCLI_ACCOUNT)");
    app.add_flag("--json", json_output, "machine-readable JSON output");
    CLI::Option* full_option = app.add_flag("--full", full, "reserved through v1");
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
    app.add_flag("--no-color", no_color, "disable colored output");
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
    CLI::App* send_cmd = app.add_subcommand("send", "send a text message");
    send_cmd->add_option("chat", send.chat, "exact chat selector")->required();
    send_cmd->add_option("TEXT", send.text, "message text or - for stdin")->required();
    auto* markdown_option = send_cmd->add_flag("--md", send.markdown, "parse as Markdown v2");
    auto* html_option = send_cmd->add_flag("--html", send.html, "parse as HTML");
    markdown_option->excludes(html_option);
    html_option->excludes(markdown_option);
    send.reply_option = send_cmd->add_option("--reply-to", send.reply_to, "message id to reply to");
    send.topic_option = send_cmd->add_option("--topic", send.topic, "bare forum id or forum:<id>");
    send_cmd->add_flag("--silent", send.silent, "disable recipient notification");
    send.schedule_option =
        send_cmd->add_option("--schedule", send.schedule, "RFC3339 instant or online");
    CLI::App* fetch_cmd = app.add_subcommand("fetch", "warm local history for one chat");
    fetch_cmd->add_option("chat", fetch.chat, "chat selector")->required();
    fetch.limit_option =
        fetch_cmd->add_option("--limit", fetch.limit, "target history depth (1-1000000)");
    fetch.all_option = fetch_cmd->add_flag("--all", fetch.all, "fetch without a numeric limit");
    fetch.since_option =
        fetch_cmd->add_option("--since", fetch.since, "inclusive timestamp lower bound");
    CLI::App* listen_cmd = app.add_subcommand("listen", "stream Telegram updates");
    listen_cmd->add_option("--chat", stream.listen_chats, "chat selector")->expected(0, 64);
    stream.listen_types_option =
        listen_cmd->add_option("--types", stream.listen_types, "comma-separated event types");
    stream.listen_count_option =
        listen_cmd->add_option("--count", stream.listen_count_raw, "stop after N items");
    CLI::App* wait_for_cmd = app.add_subcommand("wait-for", "wait for a matching message");
    stream.wait_chat_option = wait_for_cmd->add_option("--chat", stream.wait_chat, "chat selector");
    stream.wait_from_option =
        wait_for_cmd->add_option("--from", stream.wait_from, "sender user selector");
    stream.wait_regex_option =
        wait_for_cmd->add_option("--regex", stream.wait_regex, "message text RE2 pattern");
    stream.wait_after_option =
        wait_for_cmd->add_option("--after", stream.wait_after_raw, "local history anchor");
    CLI::App* contact_cmd = app.add_subcommand("contact", "curated contacts");
    contact_cmd->require_subcommand(1);
    contact_cmd->add_subcommand("list", "list contacts");
    contact_cmd->add_subcommand("search", "search contacts")
        ->add_option("query", m6.contact_query)
        ->required();
    for (const auto* action : {"add", "remove", "block", "unblock"}) {
        contact_cmd->add_subcommand(action, std::string(action) + " a contact")
            ->add_option("user", m6.contact_user)
            ->required();
    }
    CLI::App* folder_cmd = app.add_subcommand("folder", "chat folders");
    folder_cmd->require_subcommand(1);
    folder_cmd->add_subcommand("list", "list chat folders");
    folder_cmd->add_subcommand("show", "show a chat folder")
        ->add_option("folder-id", m6.folder_id_raw)
        ->required();
    CLI::App* folder_create_cmd = folder_cmd->add_subcommand("create", "create a chat folder");
    folder_create_cmd->add_option("name", m6.folder_name)->required();
    folder_create_cmd->add_option("--chat", m6.folder_chats)->required()->expected(1, 100);
    m6.folder_create_icon_option =
        folder_create_cmd->add_option("--icon", m6.folder_icon, "folder icon");
    m6.folder_create_color_option =
        folder_create_cmd->add_option("--color", m6.folder_color, "folder color -1..6");
    CLI::App* folder_edit_cmd = folder_cmd->add_subcommand("edit", "edit folder metadata");
    folder_edit_cmd->add_option("folder-id", m6.folder_id_raw)->required();
    m6.folder_name_option = folder_edit_cmd->add_option("--name", m6.folder_name, "folder name");
    m6.folder_edit_icon_option =
        folder_edit_cmd->add_option("--icon", m6.folder_icon, "folder icon or default");
    m6.folder_edit_color_option =
        folder_edit_cmd->add_option("--color", m6.folder_color, "folder color -1..6");
    folder_cmd->add_subcommand("delete", "delete a chat folder")
        ->add_option("folder-id", m6.folder_id_raw)
        ->required();
    for (const auto* action : {"add-chat", "remove-chat"}) {
        auto* membership = folder_cmd->add_subcommand(action, "change folder membership");
        membership->add_option("folder-id", m6.folder_id_raw)->required();
        membership->add_option("chat", m6.folder_chat)->required();
    }
    CLI::App* topic_cmd = app.add_subcommand("topic", "forum topics");
    topic_cmd->require_subcommand(1);
    topic_cmd->add_subcommand("list", "list topics")->add_option("chat", m6.topic_chat)->required();
    CLI::App* topic_create_cmd = topic_cmd->add_subcommand("create", "create a topic");
    topic_create_cmd->add_option("chat", m6.topic_chat)->required();
    topic_create_cmd->add_option("name", m6.topic_name)->required();
    topic_create_cmd->add_option("--icon", m6.topic_icon, "blue|yellow|purple|green|pink|red");
    CLI::App* topic_edit_cmd = topic_cmd->add_subcommand("edit", "edit a topic");
    topic_edit_cmd->add_option("chat", m6.topic_chat)->required();
    topic_edit_cmd->add_option("topic-id", m6.topic_id_raw)->required();
    topic_edit_cmd->add_option("name", m6.topic_name)->required();
    for (const auto* action : {"close", "reopen"}) {
        auto* state = topic_cmd->add_subcommand(action, "change topic state");
        state->add_option("chat", m6.topic_chat)->required();
        state->add_option("topic-id", m6.topic_id_raw)->required();
    }
    CLI::App* storage_cmd = app.add_subcommand("storage", "TDLib storage");
    storage_cmd->require_subcommand(1);
    storage_cmd->add_subcommand("stats", "show storage statistics");
    storage_cmd->add_subcommand("optimize", "optimize storage");
    CLI::App* session_cmd = app.add_subcommand("session", "active sessions");
    session_cmd->require_subcommand(1);
    session_cmd->add_subcommand("list", "list active sessions");
    session_cmd->add_subcommand("terminate", "terminate a session")
        ->add_option("session-id", m6.session_id)
        ->required();
    CLI::App* msg_cmd = app.add_subcommand("msg", "message reads and mutations");
    msg_cmd->require_subcommand(1);
    CLI::App* msg_get_cmd = msg_cmd->add_subcommand("get", "get messages by id");
    msg_get_cmd->add_option("chat", messages.get_chat)->required();
    msg_get_cmd->add_option("id", messages.get_ids)->required()->expected(1, 100);
    CLI::App* msg_link_cmd = msg_cmd->add_subcommand("link", "get a message link");
    msg_link_cmd->add_option("chat", messages.link_chat)->required();
    msg_link_cmd->add_option("id", messages.link_id)->required();
    CLI::App* msg_delete_cmd = msg_cmd->add_subcommand("delete", "delete messages");
    msg_delete_cmd->add_option("chat", messages.delete_chat)->required();
    msg_delete_cmd->add_option("id", messages.delete_ids)->required()->expected(1, 100);
    msg_delete_cmd->add_flag("--for-all", messages.delete_for_all, "delete for all participants");
    CLI::App* msg_forward_cmd = msg_cmd->add_subcommand("forward", "forward messages");
    msg_forward_cmd
        ->add_option("arg", messages.forward_positionals,
                     "source, ordered message ids, and destination")
        ->required()
        ->expected(3, 102);
    msg_forward_cmd->add_flag("--drop-author", messages.forward_drop_author,
                              "send copies without author attribution");
    CLI::App* msg_edit_cmd = msg_cmd->add_subcommand("edit", "edit a text message");
    msg_edit_cmd->add_option("chat", messages.edit_chat)->required();
    msg_edit_cmd->add_option("id", messages.edit_id)->required();
    msg_edit_cmd->add_option("TEXT", messages.edit_text, "message text or - for stdin")->required();
    CLI::App* msg_react_cmd = msg_cmd->add_subcommand("react", "add or remove a reaction");
    msg_react_cmd->add_option("chat", messages.react_chat)->required();
    msg_react_cmd->add_option("id", messages.react_id)->required();
    msg_react_cmd->add_option("emoji", messages.reaction)->required();
    auto* react_remove_option =
        msg_react_cmd->add_flag("--remove", messages.react_remove, "remove the reaction");
    auto* react_big_option =
        msg_react_cmd->add_flag("--big", messages.react_big, "use a big reaction animation");
    react_remove_option->excludes(react_big_option);
    react_big_option->excludes(react_remove_option);
    CLI::App* msg_pin_cmd = msg_cmd->add_subcommand("pin", "pin a message");
    msg_pin_cmd->add_option("chat", messages.pin_chat)->required();
    msg_pin_cmd->add_option("id", messages.pin_id)->required();
    CLI::App* msg_unpin_cmd = msg_cmd->add_subcommand("unpin", "unpin a message");
    msg_unpin_cmd->add_option("chat", messages.unpin_chat)->required();
    msg_unpin_cmd->add_option("id", messages.unpin_id)->required();
    CLI::App* chat_cmd = app.add_subcommand("chat", "chat mutations");
    chat_cmd->require_subcommand(1);
    chat_cmd->add_subcommand("mark-read", "mark a chat as read")
        ->add_option("chat", chat.mark_read)
        ->required();
    CLI::App* chat_mute_cmd = chat_cmd->add_subcommand("mute", "mute a chat");
    chat_mute_cmd->add_option("chat", chat.mute)->required();
    chat.mute_for_option =
        chat_mute_cmd->add_option("--for", chat.mute_for, "duration such as 30m or 1d");
    chat_cmd->add_subcommand("unmute", "unmute a chat")
        ->add_option("chat", chat.unmute)
        ->required();
    chat_cmd->add_subcommand("pin", "pin a chat")->add_option("chat", chat.pin)->required();
    chat_cmd->add_subcommand("unpin", "unpin a chat")->add_option("chat", chat.unpin)->required();
    chat_cmd->add_subcommand("archive", "archive a chat")
        ->add_option("chat", chat.archive)
        ->required();
    chat_cmd->add_subcommand("unarchive", "unarchive a chat")
        ->add_option("chat", chat.unarchive)
        ->required();
    chat_cmd->add_subcommand("join", "join a chat")
        ->add_option("invite-link|@username", chat.join)
        ->required();
    chat_cmd->add_subcommand("leave", "leave a chat")->add_option("chat", chat.leave)->required();
    CLI::App* set_title_cmd = chat_cmd->add_subcommand("set-title", "set chat title");
    set_title_cmd->add_option("chat", m6.admin_chat)->required();
    set_title_cmd->add_option("title", m6.admin_title)->required();
    CLI::App* set_photo_cmd = chat_cmd->add_subcommand("set-photo", "set or delete chat photo");
    set_photo_cmd->add_option("chat", m6.admin_chat)->required();
    m6.photo_path_option = set_photo_cmd->add_option("PATH", m6.photo_path);
    m6.photo_path_option->expected(0, 1);
    m6.photo_delete_option = set_photo_cmd->add_flag("--delete", m6.photo_delete);
    CLI::App* set_description_cmd =
        chat_cmd->add_subcommand("set-description", "set chat description");
    set_description_cmd->add_option("chat", m6.admin_chat)->required();
    set_description_cmd->add_option("description", m6.admin_description)->required();
    CLI::App* invite_cmd = chat_cmd->add_subcommand("invite-link", "create or revoke invite link");
    invite_cmd->add_option("chat", m6.admin_chat)->required();
    m6.invite_revoke_option =
        invite_cmd->add_option("--revoke", m6.invite_revoke, "invite link to revoke");
    CLI::App* promote_cmd = chat_cmd->add_subcommand("promote", "promote a chat member");
    promote_cmd->add_option("chat", m6.admin_chat)->required();
    promote_cmd->add_option("user", m6.admin_user)->required();
    promote_cmd->add_option("--rights", m6.admin_rights)->required();
    for (const auto* action : {"demote", "ban", "unban", "kick"}) {
        auto* member = chat_cmd->add_subcommand(action, "change a chat member");
        member->add_option("chat", m6.admin_chat)->required();
        member->add_option("user", m6.admin_user)->required();
    }
    CLI::App* permissions_cmd =
        chat_cmd->add_subcommand("set-permissions", "set default chat permissions");
    permissions_cmd->add_option("chat", m6.admin_chat)->required();
    permissions_cmd->add_option("--permissions", m6.admin_permissions)->required();
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

    CLI::App* saved_cmd = app.add_subcommand("saved", "Saved Messages operations");
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
    CLI::App* saved_attach_cmd =
        saved_cmd->add_subcommand("attach", "send a file as a Saved Messages reply");
    saved_attach_cmd->add_option("message-id", saved.attach_message_id_raw)->required();
    saved_attach_cmd->add_option("PATH", saved.attach_path)->required();
    saved_attach_cmd->add_option("--caption", saved.attach_caption, "plain attachment caption");

    std::function<void(CLI::App&)> expose_no_color;
    expose_no_color = [&no_color, &expose_no_color](CLI::App& command) {
        for (auto* child : command.get_subcommands([](CLI::App*) { return true; })) {
            child->add_flag("--no-color", no_color, "disable colored output");
            expose_no_color(*child);
        }
    };
    expose_no_color(app);

    if (const auto parse_exit = parse_arguments(app, argc, argv); parse_exit.has_value()) {
        return parse_exit.value();
    }

    if (rejected_bot_token_option->count() != 0) {
        rejected_bot_token.assign(rejected_bot_token.size(), '\0');
        rejected_bot_token.clear();
        return report_insecure_bot_token();
    }

    auto command = selected_command(app);
    std::unique_ptr<tgcli::secure::StringWiper> join_invite_wiper;
    std::unique_ptr<tgcli::secure::StringWiper> revoke_invite_wiper;
    if (command == std::vector<std::string>{"chat", "join"} &&
        tgcli::common::is_exact_telegram_invite_link(chat.join)) {
        join_invite_wiper = std::make_unique<tgcli::secure::StringWiper>(
            chat.join, tgcli::secure::WipeObserver{}, "cli_join_argument");
        for (int index = 1; index < argc; ++index) {
            if (argv[index] != nullptr && std::string_view(argv[index]) == chat.join) {
                wipe_argument(argv[index]);
            }
        }
    }
    if (command == std::vector<std::string>{"chat", "invite-link"} &&
        m6.invite_revoke_option->count() != 0) {
        revoke_invite_wiper = std::make_unique<tgcli::secure::StringWiper>(
            m6.invite_revoke, tgcli::secure::WipeObserver{}, "cli_m6_invite_argument");
        for (int index = 1; index < argc; ++index) {
            if (argv[index] != nullptr && std::string_view(argv[index]) == m6.invite_revoke) {
                wipe_argument(argv[index]);
            }
        }
    }
    if (command == std::vector<std::string>{"send"}) {
        if (const auto stdin_exit = read_send_stdin(send); stdin_exit) {
            return *stdin_exit;
        }
    }
    if (command == std::vector<std::string>{"msg", "edit"}) {
        if (const auto stdin_exit = read_edit_stdin(messages); stdin_exit) {
            return *stdin_exit;
        }
    }
    if (command == std::vector<std::string>{"msg", "delete"}) {
        std::ranges::sort(messages.delete_ids);
        messages.delete_has_duplicate =
            std::adjacent_find(messages.delete_ids.begin(), messages.delete_ids.end()) !=
            messages.delete_ids.end();
    }
    if (command == std::vector<std::string>{"msg", "forward"}) {
        messages.forward_from = messages.forward_positionals.front();
        messages.forward_to = messages.forward_positionals.back();
        for (const auto& raw : std::span(messages.forward_positionals)
                                   .subspan(1, messages.forward_positionals.size() - 2)) {
            std::int64_t id = 0;
            if (!parse_signed_decimal(raw, id)) {
                messages.forward_ids_valid = false;
                break;
            }
            messages.forward_ids.push_back(id);
        }
    }
    if (is_saved_attach(command) &&
        !parse_signed_decimal(saved.attach_message_id_raw, saved.attach_message_id)) {
        saved.attach_message_id_valid = false;
    }
    const ReadCliArguments* selected_read = &read;
    if (command == std::vector<std::string>{"history"}) {
        command = {"read"};
        selected_read = &history;
    }
    const bool stream_command = command == std::vector<std::string>{"listen"} ||
                                command == std::vector<std::string>{"wait-for"};
    if (stream_command) {
        if (saved.cursor_option->count() != 0) {
            return report_usage("--cursor is not supported for stream commands", "--cursor",
                                "unsupported_mode");
        }
        for (const auto& [option, argument] :
             {std::pair{allow_write_option, std::string_view{"--allow-write"}},
              std::pair{yes_option, std::string_view{"--yes"}},
              std::pair{dry_run_option, std::string_view{"--dry-run"}},
              std::pair{idempotency_key_option, std::string_view{"--idempotency-key"}}}) {
            if (option->count() != 0) {
                return report_usage(std::string(argument) + " is not supported for stream commands",
                                    argument, "unsupported_mode");
            }
        }
        if (const auto stream_exit = normalize_stream_arguments(
                command, stream,
                timeout_option->count() != 0 ? std::optional<double>{timeout_seconds}
                                             : std::nullopt);
            stream_exit) {
            return *stream_exit;
        }
    }
    if (const auto operation = tgcli::proto::m6_operation_for_command(command)) {
        const auto* identity = tgcli::proto::m6_operation_identity(*operation);
        if (saved.cursor_option->count() != 0) {
            return report_usage("--cursor is not supported for M6 commands", "--cursor",
                                "unsupported_mode");
        }
        if (!identity->mutation) {
            for (const auto& [option, argument] :
                 {std::pair{allow_write_option, std::string_view{"--allow-write"}},
                  std::pair{yes_option, std::string_view{"--yes"}},
                  std::pair{dry_run_option, std::string_view{"--dry-run"}},
                  std::pair{idempotency_key_option, std::string_view{"--idempotency-key"}}}) {
                if (option->count() != 0) {
                    return report_usage(std::string(argument) + " is unsupported for M6 reads",
                                        argument, "unsupported_mode");
                }
            }
        } else if (yes_option->count() != 0 &&
                   identity->tier != tgcli::proto::M6Tier::Destructive) {
            return report_usage("--yes is supported only for destructive M6 commands", "--yes",
                                "unsupported_mode");
        }
    } else if (command == std::vector<std::string>{"session", "list"}) {
        if (saved.cursor_option->count() != 0 || allow_write_option->count() != 0 ||
            yes_option->count() != 0 || dry_run_option->count() != 0 ||
            idempotency_key_option->count() != 0) {
            return report_usage("session list does not accept write or pagination options",
                                "session", "unsupported_mode");
        }
    } else if (command == std::vector<std::string>{"session", "terminate"}) {
        if (saved.cursor_option->count() != 0 || idempotency_key_option->count() != 0) {
            return report_usage("session terminate does not accept cursor or idempotency",
                                "session", "unsupported_mode");
        }
    }
    if (const auto pre_routing_exit = handle_client_local_or_reserved_command(
            command, *account_option, *full_option, *allow_write_option, *yes_option,
            *dry_run_option, *timeout_option, timeout_seconds, *saved.cursor_option,
            *idempotency_key_option, schema_target, schema_all, schema_help, verbose);
        pre_routing_exit) {
        return *pre_routing_exit;
    }
    if (command.empty()) {
        return report_missing_command();
    }
    auto captured_cwd = capture_request_cwd();
    std::string frozen_cwd = captured_cwd ? std::move(*captured_cwd) : std::string{};
    if (const auto argument_exit =
            validate_command_arguments(command, saved, chats, messages, send, chat, *selected_read,
                                       fetch, resolve_selector, m6, frozen_cwd);
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

    const auto m6_operation = tgcli::proto::m6_operation_for_command(command);
    const auto* m6_identity =
        m6_operation ? tgcli::proto::m6_operation_identity(*m6_operation) : nullptr;
    const bool supports_dry_run = command == std::vector<std::string>{"logout"} ||
                                  command == std::vector<std::string>{"account", "remove"} ||
                                  command == std::vector<std::string>{"session", "terminate"} ||
                                  tgcli::proto::m3_operation_for_command(command).has_value() ||
                                  (m6_identity != nullptr && m6_identity->mutation);
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
                             send, chat, *selected_read, fetch, stream, m6);
    auto request_context =
        make_request_context(json_output, yes, dry_run, *folded_authority, std::move(frozen_cwd));
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
    if (join_invite_wiper) {
        tgcli::secure::wipe(chat.join, {}, "cli_join_argument");
    }
    if (revoke_invite_wiper) {
        tgcli::secure::wipe(m6.invite_revoke, {}, "cli_m6_invite_argument");
    }

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
