#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::string_view kHelp = R"(Print curated JSON schemas

Usage:
  tgcli schema [OPTIONS] command...

Positionals:
  command TEXT ... REQUIRED    command path (for example: account list)

Options:
  -h,--help                    Print this help message and exit
  --all                        include every cataloged result, item, and error schema
)";

constexpr std::string_view kMissing =
    R"({"error":{"code":"USAGE","details":{"argument":null,"reason":"missing_argument"},"message":"required command argument is missing"}}
)";

struct ProcessOutcome {
    int exit_code = -1;
    std::string out;
    std::string err;
};

std::string read_file(const std::filesystem::path& filename) {
    std::ifstream input(filename, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& filename, std::string_view bytes) {
    std::ofstream output(filename, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

std::string schema_bytes(std::string_view filename) {
    return read_file(std::filesystem::path(TGCLI_SCHEMA_DIR) / filename);
}

std::string
all_schema_bytes(std::initializer_list<std::pair<std::string_view, std::string_view>> members) {
    std::string output{"{"};
    bool first = true;
    for (const auto& [kind, filename] : members) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        output.push_back('"');
        output.append(kind);
        output += "\":";
        auto bytes = schema_bytes(filename);
        REQUIRE(bytes.ends_with('\n'));
        bytes.pop_back();
        output += bytes;
    }
    output += "}\n";
    return output;
}

std::string unknown_error(std::string_view target) {
    return R"({"error":{"code":"USAGE","details":{"argument":")" + std::string(target) +
           R"(","reason":"unknown_command"},"message":"no curated schema is available for command"}})" +
           "\n";
}

std::string unsupported_error(std::string_view option) {
    return R"({"error":{"code":"USAGE","details":{"argument":")" + std::string(option) +
           R"(","reason":"unsupported_mode"},"message":")" + std::string(option) +
           R"( is not supported for this command"}})" + "\n";
}

class ProcessEnvironment {
  public:
    ProcessEnvironment() {
        std::string pattern = "/tmp/tgcli-schema-cli-XXXXXX";
        pattern.push_back('\0');
        const char* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        root_ = created;

        for (const auto* name : kManagedVariables) {
            const char* value = std::getenv(name);
            saved_.emplace_back(name, value == nullptr ? std::nullopt
                                                       : std::optional<std::string>(value));
        }
        set("HOME", root_ + "/home");
        set("XDG_CONFIG_HOME", config_home());
        set("XDG_DATA_HOME", data_home());
        set("XDG_STATE_HOME", state_home());
        set("XDG_RUNTIME_DIR", runtime_home());
        unset("TGCLI_ACCOUNT");
        unset("TGCLI_ALLOW_WRITE");
        unset("TGCLI_TEST_DC");
        unset("NO_COLOR");
        for (const auto& directory : {root_ + "/home", config_home(), data_home(), state_home(),
                                      runtime_home(), capture_home()}) {
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        }
    }

    ~ProcessEnvironment() {
        for (const auto& [name, value] : saved_) {
            if (value) {
                ::setenv(name.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(name.c_str());
            }
        }
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ProcessEnvironment(const ProcessEnvironment&) = delete;
    ProcessEnvironment& operator=(const ProcessEnvironment&) = delete;
    ProcessEnvironment(ProcessEnvironment&&) = delete;
    ProcessEnvironment& operator=(ProcessEnvironment&&) = delete;

    [[nodiscard]] const std::string& root() const {
        return root_;
    }

    [[nodiscard]] std::string config_home() const {
        return root_ + "/config";
    }

    [[nodiscard]] std::string data_home() const {
        return root_ + "/data";
    }

    [[nodiscard]] std::string state_home() const {
        return root_ + "/state";
    }

    [[nodiscard]] std::string runtime_home() const {
        return root_ + "/run";
    }

    [[nodiscard]] std::string capture_home() const {
        return root_ + "/capture";
    }

    static void set_variable(const char* name, const std::string& value) {
        set(name, value);
    }

    static void unset_variable(const char* name) {
        unset(name);
    }

    ProcessOutcome run(const std::vector<std::string>& arguments) {
        const std::string stem = capture_home() + "/" + std::to_string(sequence_++);
        const std::string output_path = stem + ".out";
        const std::string error_path = stem + ".err";
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            const int output = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            const int errors = ::open(error_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (output < 0 || errors < 0) {
                ::_exit(126);
            }
            ::dup2(output, STDOUT_FILENO);
            ::dup2(errors, STDERR_FILENO);
            ::close(output);
            ::close(errors);

            std::vector<std::string> owned{TGCLI_TEST_BINARY};
            owned.insert(owned.end(), arguments.begin(), arguments.end());
            std::vector<char*> argv;
            argv.reserve(owned.size() + 1);
            for (auto& argument : owned) {
                argv.push_back(argument.data());
            }
            argv.push_back(nullptr);
            ::execv(TGCLI_TEST_BINARY, argv.data());
            ::_exit(127);
        }

        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        return {WEXITSTATUS(status), read_file(output_path), read_file(error_path)};
    }

  private:
    static constexpr std::array<const char*, 9> kManagedVariables{
        "HOME",          "XDG_CONFIG_HOME",   "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR",
        "TGCLI_ACCOUNT", "TGCLI_ALLOW_WRITE", "TGCLI_TEST_DC", "NO_COLOR"};

    static void set(const char* name, const std::string& value) {
        REQUIRE(::setenv(name, value.c_str(), 1) == 0);
    }

    static void unset(const char* name) {
        REQUIRE(::unsetenv(name) == 0);
    }

    std::string root_;
    std::size_t sequence_ = 0;
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

void check_success(const ProcessOutcome& outcome, std::string_view expected) {
    CHECK(outcome.exit_code == 0);
    CHECK(outcome.out == expected);
    CHECK(outcome.err.empty());
}

void check_usage(const ProcessOutcome& outcome, std::string_view expected) {
    CHECK(outcome.exit_code == 2);
    CHECK(outcome.out.empty());
    CHECK(outcome.err == expected);
}

} // namespace

TEST_CASE("schema CLI returns exact embedded primary and all-kind bytes",
          "[schema][cli][process]") {
    ProcessEnvironment environment;
    const auto version = schema_bytes("version.result.schema.json");
    const auto account_list = schema_bytes("account-list.result.schema.json");
    const auto read = schema_bytes("read.result.schema.json");

    check_success(environment.run({"schema", "version"}), version);
    check_success(environment.run({"--json", "--no-daemon", "--no-color", "schema", "version"}),
                  version);
    check_success(environment.run({"schema", "version", "--json", "--no-daemon", "--no-color"}),
                  version);

    ProcessEnvironment::set_variable("NO_COLOR", "1");
    check_success(environment.run({"schema", "version"}), version);
    ProcessEnvironment::unset_variable("NO_COLOR");

    check_success(environment.run({"schema", "account list"}), account_list);
    check_success(environment.run({"schema", " account\t", "\nlist\r\f\v"}), account_list);
    check_success(environment.run({"schema", "history"}), read);
    check_success(environment.run({"schema", "listen"}), schema_bytes("listen.item.schema.json"));
    check_success(environment.run({"schema", "wait-for"}),
                  schema_bytes("wait-for.result.schema.json"));

    check_success(environment.run({"schema", "listen", "--all"}),
                  all_schema_bytes({{"item", "listen.item.schema.json"},
                                    {"error", "stream.error.schema.json"}}));
    check_success(environment.run({"schema", "--all", "account", "remove"}),
                  all_schema_bytes({{"result", "account-remove.result.schema.json"},
                                    {"error", "account-remove.error.schema.json"}}));
}

TEST_CASE("schema CLI preserves target bytes and recognizes only the history alias",
          "[schema][cli][process]") {
    ProcessEnvironment environment;

    check_usage(environment.run({"schema"}), kMissing);
    check_usage(environment.run({"schema", " \t\n\r\f\v"}), kMissing);
    check_usage(environment.run({"schema", "schema"}), unknown_error("schema"));
    check_usage(environment.run({"schema", "daemon", "run"}), unknown_error("daemon run"));
    check_usage(environment.run({"schema", "History"}), unknown_error("History"));

    const std::string unicode_space = "account\xC2\xA0list";
    check_usage(environment.run({"schema", unicode_space}), unknown_error(unicode_space));
}

TEST_CASE("schema CLI freezes help and parser unsupported lookup precedence",
          "[schema][cli][process]") {
    ProcessEnvironment environment;

    check_success(environment.run({"schema", "--help"}), kHelp);
    ProcessEnvironment::set_variable("NO_COLOR", "anything");
    check_success(environment.run({"--json", "--no-daemon", "--no-color", "schema", "--help"}),
                  kHelp);
    ProcessEnvironment::unset_variable("NO_COLOR");
    check_success(environment.run({"schema", "-h", "--account", "bad/name", "--full", "unknown"}),
                  kHelp);

    constexpr std::string_view invalid_argument =
        R"({"error":{"code":"USAGE","details":{"argument":null,"reason":"invalid_argument"},"message":"invalid command argument"}}
)";
    constexpr std::string_view unknown_argument =
        R"({"error":{"code":"USAGE","details":{"argument":null,"reason":"unknown_command"},"message":"unknown command or argument"}}
)";
    check_usage(environment.run({"schema", "--timeout", "not-a-number", "--help", "version"}),
                invalid_argument);
    check_usage(environment.run({"schema", "version", "--unknown", "--help"}), unknown_argument);

    const std::vector<std::pair<std::vector<std::string>, std::string_view>> unsupported{
        {{"schema", "--account", "main", "unknown"}, "--account"},
        {{"schema", "--full", "unknown"}, "--full"},
        {{"schema", "--allow-write", "unknown"}, "--allow-write"},
        {{"schema", "--yes", "unknown"}, "--yes"},
        {{"schema", "--dry-run", "unknown"}, "--dry-run"},
        {{"schema", "--timeout", "1", "unknown"}, "--timeout"},
        {{"schema", "--cursor", "cursor", "unknown"}, "--cursor"},
        {{"schema", "--idempotency-key", "key", "unknown"}, "--idempotency-key"},
    };
    for (const auto& [arguments, option] : unsupported) {
        CAPTURE(option);
        check_usage(environment.run(arguments), unsupported_error(option));
    }

    check_usage(environment.run({"schema", "--idempotency-key", "key", "--cursor", "cursor",
                                 "--timeout", "1", "--dry-run", "--yes", "--allow-write", "--full",
                                 "--account", "main", "unknown"}),
                unsupported_error("--account"));
    check_usage(environment.run({"schema", "--cursor", "cursor"}), unsupported_error("--cursor"));
    check_usage(environment.run({"--no-color", "schema", "unknown"}), unknown_error("unknown"));
}

TEST_CASE("schema CLI verbose output is success-only and option-invariant",
          "[schema][cli][process]") {
    ProcessEnvironment environment;
    const auto expected = schema_bytes("version.result.schema.json");

    const auto verbose = environment.run({"-v", "schema", "version"});
    CHECK(verbose.exit_code == 0);
    CHECK(verbose.out == expected);
    CHECK(verbose.err == "diagnostic: transport=local\n");

    ProcessEnvironment::set_variable("NO_COLOR", "anything");
    const auto noops =
        environment.run({"--json", "--no-daemon", "--no-color", "--verbose", "schema", "version"});
    CHECK(noops.exit_code == 0);
    CHECK(noops.out == verbose.out);
    CHECK(noops.err == verbose.err);

    const auto unknown = environment.run({"-v", "schema", "unknown"});
    check_usage(unknown, unknown_error("unknown"));
    const auto help = environment.run({"-v", "schema", "--help"});
    check_success(help, kHelp);
}

TEST_CASE("schema CLI retains redacted legacy secret rejection before parsing",
          "[schema][cli][process]") {
    ProcessEnvironment environment;
    constexpr std::string_view secret = "schema-secret-sentinel";
    const auto rejected =
        environment.run({"schema", "--unknown", "--bot-token", std::string(secret), "version"});

    check_usage(
        rejected,
        R"({"error":{"code":"INSECURE_SECRET_INPUT","details":{"argument":"--bot-token","replacement":"--bot"},"message":"bot tokens are not accepted on the command line"}}
)");
    CHECK(rejected.err.find(secret) == std::string::npos);
}

TEST_CASE("schema CLI ignores invalid environment and inaccessible configuration roots",
          "[schema][cli][process]") {
    ProcessEnvironment environment;
    const auto expected = environment.run({"schema", "version"});
    check_success(expected, schema_bytes("version.result.schema.json"));

    constexpr std::string_view sentinel = "must-not-change\n";
    const std::array<std::pair<const char*, std::string>, 4> invalid_roots{{
        {"XDG_CONFIG_HOME", environment.root() + "/invalid-config"},
        {"XDG_DATA_HOME", environment.root() + "/invalid-data"},
        {"XDG_STATE_HOME", environment.root() + "/invalid-state"},
        {"XDG_RUNTIME_DIR", environment.root() + "/invalid-runtime"},
    }};
    for (const auto& [variable, filename] : invalid_roots) {
        write_file(filename, sentinel);
        REQUIRE(::chmod(filename.c_str(), 0000) == 0);
        ProcessEnvironment::set_variable(variable, filename);
    }
    ProcessEnvironment::set_variable("TGCLI_ACCOUNT", "bad/name");
    ProcessEnvironment::set_variable("TGCLI_ALLOW_WRITE", "invalid");
    ProcessEnvironment::set_variable("TGCLI_TEST_DC", "invalid");

    const auto isolated = environment.run({"--json", "schema", "version"});
    CHECK(isolated.exit_code == expected.exit_code);
    CHECK(isolated.out == expected.out);
    CHECK(isolated.err == expected.err);
    for (const auto& [variable, filename] : invalid_roots) {
        CAPTURE(variable);
        REQUIRE(::chmod(filename.c_str(), 0600) == 0);
        CHECK(read_file(filename) == sentinel);
    }
    CHECK_FALSE(std::filesystem::exists(environment.config_home() + "/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.data_home() + "/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.state_home() + "/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.runtime_home() + "/tgcli"));
}

TEST_CASE("schema CLI never connects to an available account socket or creates local state",
          "[schema][cli][process]") {
    ProcessEnvironment environment;
    const std::string socket_directory = environment.runtime_home() + "/tgcli";
    REQUIRE(std::filesystem::create_directory(socket_directory));
    REQUIRE(::chmod(socket_directory.c_str(), 0700) == 0);
    const std::string socket_path = socket_directory + "/main.sock";

    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    REQUIRE(socket_path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    REQUIRE(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::chmod(socket_path.c_str(), 0600) == 0);
    REQUIRE(::listen(listener, 4) == 0);

    std::array<int, 2> control{};
    REQUIRE(::pipe(control.data()) == 0);
    const pid_t watcher = ::fork();
    REQUIRE(watcher >= 0);
    if (watcher == 0) {
        ::close(control[1]);
        bool connected = false;
        for (;;) {
            std::array<pollfd, 2> descriptors{{
                {listener, POLLIN, 0},
                {control[0], POLLIN, 0},
            }};
            int ready = -1;
            do {
                ready = ::poll(descriptors.data(), descriptors.size(), -1);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0) {
                ::_exit(125);
            }
            if ((descriptors[0].revents & POLLIN) != 0) {
                const int accepted = ::accept(listener, nullptr, nullptr);
                if (accepted < 0) {
                    ::_exit(125);
                }
                connected = true;
                ::close(accepted);
            }
            if ((descriptors[1].revents & POLLIN) != 0) {
                char stop = '\0';
                if (::read(control[0], &stop, sizeof(stop)) != sizeof(stop)) {
                    ::_exit(125);
                }
                ::_exit(connected ? 1 : 0);
            }
        }
    }
    REQUIRE(::close(control[0]) == 0);

    check_success(environment.run({"schema", "version"}),
                  schema_bytes("version.result.schema.json"));
    constexpr char stop = 'x';
    REQUIRE(::write(control[1], &stop, sizeof(stop)) == sizeof(stop));
    REQUIRE(::close(control[1]) == 0);
    int watcher_status = 0;
    REQUIRE(::waitpid(watcher, &watcher_status, 0) == watcher);
    REQUIRE(WIFEXITED(watcher_status));
    CHECK(WEXITSTATUS(watcher_status) == 0);
    REQUIRE(::close(listener) == 0);

    CHECK_FALSE(std::filesystem::exists(environment.config_home() + "/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.data_home() + "/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.state_home() + "/tgcli"));
    CHECK_FALSE(std::filesystem::exists(socket_directory + "/main.control.sock"));
    CHECK_FALSE(std::filesystem::exists(socket_directory + "/main.lock"));
}
