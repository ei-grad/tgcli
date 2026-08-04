#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

class ProcessEnvironment {
  public:
    ProcessEnvironment() {
        std::string pattern = "/tmp/tgcli-account-cli-XXXXXX";
        pattern.push_back('\0');
        root_ = ::mkdtemp(pattern.data());
        REQUIRE_FALSE(root_.empty());

        for (const auto* name : kManagedVariables) {
            const char* value = std::getenv(name);
            saved_.emplace_back(name, value == nullptr ? std::nullopt
                                                       : std::optional<std::string>(value));
        }
        set("HOME", root_);
        set("XDG_CONFIG_HOME", root_ + "/config");
        set("XDG_DATA_HOME", root_ + "/data");
        set("XDG_STATE_HOME", root_ + "/state");
        set("XDG_RUNTIME_DIR", root_ + "/run");
        unsetenv("TGCLI_ACCOUNT");
        unsetenv("TGCLI_TEST_DC");
        for (const auto& directory :
             {config_home(), root_ + "/data", root_ + "/state", root_ + "/run"}) {
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

    static void set_account(std::optional<std::string> value) {
        set_optional("TGCLI_ACCOUNT", std::move(value));
    }

    static void set_test_dc(bool enabled) {
        set_optional("TGCLI_TEST_DC", enabled ? std::optional<std::string>{"1"} : std::nullopt);
    }

    static void set_variable(const char* name, const std::string& value) {
        set(name, value);
    }

    [[nodiscard]] const std::string& root() const {
        return root_;
    }

    [[nodiscard]] std::string config_home() const {
        return root_ + "/config";
    }

    [[nodiscard]] std::string production_config() const {
        return config_home() + "/tgcli/config.toml";
    }

    [[nodiscard]] std::string test_config() const {
        return config_home() + "/tgcli-test/config.toml";
    }

  private:
    static constexpr std::array<const char*, 7> kManagedVariables{
        "HOME",          "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR",
        "TGCLI_ACCOUNT", "TGCLI_TEST_DC"};

    static void set(const char* name, const std::string& value) {
        REQUIRE(::setenv(name, value.c_str(), 1) == 0);
    }

    static void set_optional(const char* name, std::optional<std::string> value) {
        if (value) {
            REQUIRE(::setenv(name, value->c_str(), 1) == 0);
        } else {
            REQUIRE(::unsetenv(name) == 0);
        }
    }

    std::string root_;
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

struct ProcessOutcome {
    int exit_code = -1;
    std::string out;
    std::string err;
};

std::string read_file(const std::string& filename) {
    const std::ifstream input(filename);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

ProcessOutcome run_cli(const ProcessEnvironment& environment,
                       const std::vector<std::string>& arguments) {
    static std::size_t sequence = 0;
    const std::string output_path =
        environment.root() + "/process-" + std::to_string(sequence) + ".out";
    const std::string error_path =
        environment.root() + "/process-" + std::to_string(sequence++) + ".err";
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

        std::vector<std::string> owned{TGCLI_TEST_BINARY, "--json"};
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

bool wait_for_exists(const std::string& filename) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::access(filename.c_str(), F_OK) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

class ChildGuard {
  public:
    explicit ChildGuard(pid_t pid) : pid_(pid) {}
    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;
    ChildGuard(ChildGuard&&) = delete;
    ChildGuard& operator=(ChildGuard&&) = delete;
    ~ChildGuard() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    int wait() {
        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(pid_, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != pid_) {
            return -1;
        }
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

  private:
    pid_t pid_;
};

} // namespace

TEST_CASE("real CLI account globals never spawn or create account roots",
          "[account][cli][process][schema]") {
    const ProcessEnvironment environment;

    const auto empty = run_cli(environment, {"account", "list"});
    REQUIRE(empty.exit_code == kOk);
    CHECK(empty.err.empty());
    CHECK(json::parse(empty.out) == json{{"items", json::array()}, {"next", nullptr}});
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));

    const auto added = run_cli(environment, {"account", "add", "main"});
    REQUIRE(added.exit_code == kOk);
    CHECK(added.err.empty());
    const auto added_json = json::parse(added.out);
    CHECK(added_json == json{{"account", "main"}, {"created", true}, {"default", true}});
    CHECK_THAT(added_json, test::matches_json_schema("account-add.result.schema.json"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));

    const auto no_daemon = run_cli(environment, {"--no-daemon", "account", "list"});
    REQUIRE(no_daemon.exit_code == kOk);
    CHECK(no_daemon.err.empty());
    CHECK(json::parse(no_daemon.out)["items"].size() == 1);
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
}

TEST_CASE("daemon lifecycle --no-daemon fails before account routing", "[account][cli][process]") {
    const ProcessEnvironment environment;

    const auto outcome =
        run_cli(environment, {"--no-daemon", "--account", "missing", "daemon", "stop"});
    REQUIRE(outcome.exit_code == kUsage);
    CHECK(outcome.out.empty());
    const auto error = json::parse(outcome.err);
    CHECK(error["error"]["code"] == "USAGE");
    CHECK(error["error"]["details"] == json::object());
}

TEST_CASE("real CLI account targets ignore environment/default and reject global --account",
          "[account][cli][process][schema]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    ProcessEnvironment::set_account("bad.name");

    const auto listed = run_cli(environment, {"account", "list"});
    REQUIRE(listed.exit_code == kOk);
    CHECK(listed.err.empty());

    const auto competing = run_cli(environment, {"--account", "other", "account", "show", "main"});
    REQUIRE(competing.exit_code == kUsage);
    CHECK(competing.out.empty());
    const auto error = json::parse(competing.err);
    CHECK(error["error"]["details"] ==
          json{{"argument", "--account"}, {"reason", "mutually_exclusive"}});
    CHECK_THAT(error, test::matches_json_schema("account.error.schema.json"));

    const auto missing_name = run_cli(environment, {"account", "show"});
    REQUIRE(missing_name.exit_code == kUsage);
    CHECK(missing_name.out.empty());
    const auto missing_error = json::parse(missing_name.err);
    CHECK(missing_error["error"]["details"] ==
          json{{"argument", nullptr}, {"reason", "missing_argument"}});
    CHECK_THAT(missing_error, test::matches_json_schema("account.error.schema.json"));
}

TEST_CASE("real CLI returns exact duplicate, missing, and current-file config errors",
          "[account][cli][process][schema]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);

    const auto duplicate = run_cli(environment, {"account", "add", "main"});
    REQUIRE(duplicate.exit_code == kUsage);
    CHECK(duplicate.out.empty());
    auto error = json::parse(duplicate.err);
    CHECK(error["error"]["code"] == "ACCOUNT_EXISTS");
    CHECK(error["error"]["details"] == json{{"account", "main"}});
    CHECK_THAT(error, test::matches_json_schema("account.error.schema.json"));

    const auto missing = run_cli(environment, {"account", "show", "missing"});
    REQUIRE(missing.exit_code == kNotFound);
    CHECK(missing.out.empty());
    error = json::parse(missing.err);
    CHECK(error["error"]["code"] == "ACCOUNT_NOT_FOUND");
    CHECK(error["error"]["details"] == json{{"account", "missing"}});

    {
        const int fd =
            ::open(environment.production_config().c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
        REQUIRE(fd >= 0);
        constexpr std::string_view invalid = "[accounts.main\n";
        REQUIRE(::write(fd, invalid.data(), invalid.size()) ==
                static_cast<ssize_t>(invalid.size()));
        REQUIRE(::close(fd) == 0);
    }
    const auto invalid = run_cli(environment, {"account", "list"});
    REQUIRE(invalid.exit_code == kGeneric);
    CHECK(invalid.out.empty());
    error = json::parse(invalid.err);
    CHECK(error["error"]["code"] == "CONFIG_INVALID");
    CHECK(error["error"]["details"] ==
          json{{"path", environment.production_config()}, {"reason", "parse_error"}});
    CHECK_THAT(error, test::matches_json_schema("account.error.schema.json"));

    const auto doctor = run_cli(environment, {"--no-daemon", "doctor"});
    REQUIRE(doctor.exit_code == kOk);
    CHECK(doctor.err.empty());
    CHECK(json::parse(doctor.out)["account"] == "main");
}

TEST_CASE("account show rejects relative XDG-derived result paths", "[account][cli][process]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    ProcessEnvironment::set_variable("XDG_DATA_HOME", "relative-data");

    const auto outcome = run_cli(environment, {"account", "show", "main"});
    REQUIRE(outcome.exit_code == kGeneric);
    CHECK(outcome.out.empty());
    const auto error = json::parse(outcome.err);
    CHECK(error["error"]["code"] == "CONFIG_INVALID");
    CHECK(error["error"]["details"] ==
          json{{"path", environment.production_config()}, {"reason", "path_invalid"}});
    CHECK_THAT(error, test::matches_json_schema("account.error.schema.json"));
}

TEST_CASE("real CLI keeps production and TGCLI_TEST_DC account configs isolated",
          "[account][cli][process]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "prod"}).exit_code == kOk);
    REQUIRE(std::filesystem::exists(environment.production_config()));
    CHECK_FALSE(std::filesystem::exists(environment.test_config()));

    ProcessEnvironment::set_test_dc(true);
    const auto empty_test = run_cli(environment, {"account", "list"});
    REQUIRE(empty_test.exit_code == kOk);
    CHECK(json::parse(empty_test.out)["items"].empty());
    REQUIRE(run_cli(environment, {"account", "add", "test"}).exit_code == kOk);
    REQUIRE(std::filesystem::exists(environment.test_config()));

    ProcessEnvironment::set_test_dc(false);
    const auto production = run_cli(environment, {"account", "list"});
    REQUIRE(production.exit_code == kOk);
    CHECK(json::parse(production.out)["items"] ==
          json::array({json{{"name", "prod"}, {"default", true}}}));
}

TEST_CASE("real CLI applies configured, environment, and explicit routing precedence",
          "[account][cli][process][tdlib]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    REQUIRE(run_cli(environment, {"account", "add", "work"}).exit_code == kOk);
    REQUIRE(run_cli(environment, {"account", "use", "work"}).exit_code == kOk);

    auto routed = run_cli(environment, {"--no-daemon", "doctor"});
    REQUIRE(routed.exit_code == kOk);
    CHECK(json::parse(routed.out)["account"] == "work");

    ProcessEnvironment::set_account("main");
    routed = run_cli(environment, {"--no-daemon", "doctor"});
    REQUIRE(routed.exit_code == kOk);
    CHECK(json::parse(routed.out)["account"] == "main");

    routed = run_cli(environment, {"--account", "work", "--no-daemon", "doctor"});
    REQUIRE(routed.exit_code == kOk);
    CHECK(json::parse(routed.out)["account"] == "work");
}

TEST_CASE("real CLI enforces implicit-main restrictions without materializing config",
          "[account][cli][process][tdlib]") {
    const ProcessEnvironment environment;

    const auto denied = run_cli(environment, {"--no-daemon", "version"});
    REQUIRE(denied.exit_code == kNotFound);
    CHECK(denied.out.empty());
    CHECK(json::parse(denied.err)["error"]["details"] == json{{"account", "main"}});
    CHECK_FALSE(std::filesystem::exists(environment.production_config()));

    const auto doctor = run_cli(environment, {"--no-daemon", "doctor"});
    REQUIRE(doctor.exit_code == kOk);
    CHECK(doctor.err.empty());
    CHECK(json::parse(doctor.out)["account"] == "main");
    CHECK_FALSE(std::filesystem::exists(environment.production_config()));
}

TEST_CASE("real CLI exposes the config-lock deadline as exact TIMEOUT",
          "[account][cli][process][schema]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    const std::string lock_path = environment.config_home() + "/tgcli/config.lock";
    const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
    REQUIRE(lock_fd >= 0);
    REQUIRE(::flock(lock_fd, LOCK_EX) == 0);

    const auto started = std::chrono::steady_clock::now();
    const auto outcome = run_cli(environment, {"--timeout", "0.02", "account", "add", "work"});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(lock_fd);

    REQUIRE(outcome.exit_code == kTimeout);
    CHECK(outcome.out.empty());
    const auto error = json::parse(outcome.err);
    CHECK(error["error"]["code"] == "TIMEOUT");
    CHECK(error["error"]["details"] == json{{"operation", "account_add"}, {"state", nullptr}});
    CHECK_THAT(error, test::matches_json_schema("account.error.schema.json"));
    CHECK(elapsed < std::chrono::seconds(1));
}

TEST_CASE("real CLI applies the deadline to bounded current-file reads",
          "[account][cli][process][schema]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    std::string large_config = "default_account = \"main\"\npadding = \"" +
                               std::string(900'000, 'x') +
                               "\"\n[accounts.main]\nallow_write = false\n";
    const int fd = ::open(environment.production_config().c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::size_t offset = 0;
    while (offset < large_config.size()) {
        const auto count = ::write(fd, large_config.data() + offset, large_config.size() - offset);
        REQUIRE(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    REQUIRE(::close(fd) == 0);

    const auto outcome = run_cli(environment, {"--timeout", "0.000001", "account", "list"});
    REQUIRE(outcome.exit_code == kTimeout);
    CHECK(outcome.out.empty());
    const auto error = json::parse(outcome.err);
    CHECK(error["error"]["code"] == "TIMEOUT");
    CHECK(error["error"]["details"] == json{{"operation", "account_list"}, {"state", nullptr}});
    CHECK_THAT(error, test::matches_json_schema("account.error.schema.json"));
}

TEST_CASE("config-global CLI stays local while the selected daemon is running",
          "[account][cli][process][tdlib]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    const auto current_environment = paths::real_environment();
    std::string socket_error;
    const auto socket = paths::socket_path("main", current_environment, socket_error);
    REQUIRE(socket.has_value());

    const pid_t daemon_pid = ::fork();
    REQUIRE(daemon_pid >= 0);
    if (daemon_pid == 0) {
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
            ::dup2(null_fd, STDOUT_FILENO);
            ::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
        }
        ::execl(TGCLI_TEST_BINARY, "tgcli", "--account", "main", "daemon", "run",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ChildGuard daemon_guard(daemon_pid);
    REQUIRE(wait_for_exists(*socket));

    const auto listed = run_cli(environment, {"account", "list"});
    REQUIRE(listed.exit_code == kOk);
    CHECK(listed.err.empty());
    CHECK(::kill(daemon_pid, 0) == 0);

    const auto no_daemon = run_cli(environment, {"--no-daemon", "account", "show", "main"});
    REQUIRE(no_daemon.exit_code == kOk);
    CHECK(no_daemon.err.empty());
    CHECK(::kill(daemon_pid, 0) == 0);

    REQUIRE(::kill(daemon_pid, SIGTERM) == 0);
    CHECK(daemon_guard.wait() == kOk);
}
