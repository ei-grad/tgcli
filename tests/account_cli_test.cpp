#include "common/config.hpp"
#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/paths.hpp"
#include "daemon/account_removal.hpp"
#include "daemon/daemon_run.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
        unsetenv("TGCLI_ALLOW_WRITE");
        unsetenv("TGCLI_API_ID");
        unsetenv("TGCLI_API_HASH");
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

    void write_production_config(std::string_view contents) const {
        const int descriptor = ::open(production_config().c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
        REQUIRE(descriptor >= 0);
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const auto count =
                ::write(descriptor, contents.data() + offset, contents.size() - offset);
            REQUIRE(count > 0);
            offset += static_cast<std::size_t>(count);
        }
        REQUIRE(::close(descriptor) == 0);
    }

    void set_idle_exit(bool test_dc, int seconds) const {
        const auto config = test_dc ? test_config() : production_config();
        const std::ifstream input(config, std::ios::binary);
        REQUIRE(input.good());
        std::ostringstream captured;
        captured << input.rdbuf();
        auto bytes = captured.str();
        const auto grant = bytes.find("allow_write = false");
        REQUIRE(grant != std::string::npos);
        const auto line_end = bytes.find('\n', grant);
        REQUIRE(line_end != std::string::npos);
        bytes.insert(line_end + 1, "idle_exit = " + std::to_string(seconds) + "\n");
        const auto replacement = config + ".idle-replacement";
        {
            std::ofstream output(replacement, std::ios::binary | std::ios::trunc);
            REQUIRE(output.good());
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        REQUIRE(::chmod(replacement.c_str(), 0600) == 0);
        REQUIRE(::rename(replacement.c_str(), config.c_str()) == 0);
    }

  private:
    static constexpr std::array<const char*, 10> kManagedVariables{
        "HOME",          "XDG_CONFIG_HOME",   "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR",
        "TGCLI_ACCOUNT", "TGCLI_ALLOW_WRITE", "TGCLI_API_ID",  "TGCLI_API_HASH", "TGCLI_TEST_DC"};

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

int run_daemon_direct(const std::string& account) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
        }
        ::_exit(daemon::run_daemon(account));
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    return WEXITSTATUS(status);
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

bool wait_for_missing(const std::string& first, const std::string& second) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::access(first.c_str(), F_OK) != 0 && ::access(second.c_str(), F_OK) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void seed_unconfirmed_logout(const ProcessEnvironment& environment) {
    const auto current_environment = paths::real_environment();
    const auto state = paths::account_state_dir("main", current_environment);
    for (const auto& directory : {environment.root() + "/state/tgcli",
                                  environment.root() + "/state/tgcli/accounts", state}) {
        std::error_code error;
        std::filesystem::create_directory(directory, error);
        REQUIRE_FALSE(error);
        REQUIRE(::chmod(directory.c_str(), 0700) == 0);
    }
    const config::Store store(environment.production_config());
    const auto loaded = store.load();
    REQUIRE(loaded.snapshot);
    std::string error;
    const auto plan = proto::make_logout_plan("main", error);
    REQUIRE(plan);
    const daemon::LogoutAuditLog audit(state, "main", current_environment.uid);
    const daemon::AuditRecordIdentity identity{"abcdef0123456789abcdef0123456789",
                                               "2026-08-04T12:00:00Z"};
    auto intent = daemon::make_logout_audit_intent(identity, *plan, loaded.snapshot->identity,
                                                   daemon::AuthoritySource::Config,
                                                   daemon::ConfirmationSource::Yes, error);
    REQUIRE(intent);
    daemon::LogoutAuditFailure failure;
    REQUIRE(audit.append(daemon::serialize(*intent), failure, true));
    auto checkpoint = daemon::make_logout_audit_checkpoint(
        identity, *plan, daemon::AuditStage::LogoutSendStarted, error);
    REQUIRE(checkpoint);
    REQUIRE(audit.append(daemon::serialize(*checkpoint), failure));
}

std::vector<json> account_audit(const ProcessEnvironment& environment) {
    std::ifstream input(environment.root() + "/state/tgcli/accounts/main/audit.log");
    std::vector<json> records;
    std::string line;
    while (std::getline(input, line)) {
        records.push_back(json::parse(line));
    }
    return records;
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

    int wait_for(std::chrono::steady_clock::duration timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if (waited < 0 && errno != EINTR) {
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return -1;
    }

  private:
    pid_t pid_;
};

class ProcessRemovalRemote final : public daemon::AccountRemovalRemote {
  public:
    daemon::RemovalRemoteProof
    prove_remote_logout(const proto::AccountRemovePlan& /*plan*/,
                        const std::shared_ptr<const config::ConfigSnapshot>& /*config_snapshot*/,
                        bool /*send_checkpointed*/, daemon::RequestSession& /*session*/,
                        const daemon::RemovalCheckpoint& checkpoint) override {
        if (!checkpoint(daemon::AuditStage::RemoteNotPresent)) {
            return daemon::RemovalOperationError{
                "INTERNAL",
                "checkpoint failed",
                {{"operation", "account_remove"}, {"reason", "internal_error"}},
                kGeneric};
        }
        return daemon::AccountRemoveRemoteResult::NotPresent;
    }

    std::optional<daemon::RemovalOperationError>
    quiesce(daemon::RequestSession& /*session*/) override {
        return std::nullopt;
    }
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

TEST_CASE("real CLI logout dry-run is config-global and creates no runtime or account state",
          "[logout][cli][process][dry-run][schema]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);

    const auto outcome = run_cli(environment, {"--allow-write", "--yes", "--dry-run", "logout"});
    REQUIRE(outcome.exit_code == kOk);
    CHECK(outcome.err.empty());
    const auto result = json::parse(outcome.out);
    CHECK(result == json{{"dry_run", true},
                         {"plan",
                          {{"operation", "logout"},
                           {"account", "main"},
                           {"remote_logout", true},
                           {"tdlib_request", "logOut"}}}});
    CHECK_THAT(result, test::matches_json_schema("logout.result.schema.json"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));
}

TEST_CASE("real CLI rejects unsupported global dry-run before routing or runtime creation",
          "[logout][cli][process][dry-run][safety]") {
    const ProcessEnvironment environment;

    const auto outcome = run_cli(environment, {"--dry-run", "login"});

    REQUIRE(outcome.exit_code == kUsage);
    CHECK(outcome.out.empty());
    const auto error = json::parse(outcome.err);
    CHECK(error["error"]["code"] == "USAGE");
    CHECK(error["error"]["details"] ==
          json{{"argument", "--dry-run"}, {"reason", "unsupported_mode"}});
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));
}

TEST_CASE("real CLI rejects invalid TGCLI_ALLOW_WRITE before routing",
          "[logout][cli][process][safety]") {
    const ProcessEnvironment environment;
    ProcessEnvironment::set_variable("TGCLI_ALLOW_WRITE", "true");

    const auto outcome = run_cli(environment, {"--allow-write", "logout"});
    REQUIRE(outcome.exit_code == kUsage);
    CHECK(outcome.out.empty());
    const auto error = json::parse(outcome.err);
    CHECK(error["error"]["code"] == "USAGE");
    CHECK(error["error"]["details"] ==
          json{{"argument", "TGCLI_ALLOW_WRITE"}, {"reason", "invalid_environment"}});
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
}

TEST_CASE("TGCLI_ALLOW_WRITE folds to request authority with exact deny precedence",
          "[logout][cli][process][safety][tdlib]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    environment.write_production_config(
        "default_account = \"main\"\n[accounts.main]\nallow_write = true\n");
    const pid_t daemon_pid = ::fork();
    REQUIRE(daemon_pid >= 0);
    if (daemon_pid == 0) {
        const int null_descriptor = ::open("/dev/null", O_RDWR);
        if (null_descriptor >= 0) {
            ::dup2(null_descriptor, STDIN_FILENO);
            ::dup2(null_descriptor, STDOUT_FILENO);
            ::dup2(null_descriptor, STDERR_FILENO);
            ::close(null_descriptor);
        }
        ::execl(TGCLI_TEST_BINARY, "tgcli", "--account", "main", "daemon", "run",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ChildGuard daemon_guard(daemon_pid);
    const std::string socket = environment.root() + "/run/tgcli/main.sock";
    const std::string control = environment.root() + "/run/tgcli/main.ctl";
    REQUIRE(wait_for_exists(socket));

    ProcessEnvironment::set_variable("TGCLI_ALLOW_WRITE", "0");
    auto outcome = run_cli(environment, {"--allow-write", "--yes", "logout"});
    REQUIRE(outcome.exit_code == kDenied);
    CHECK(json::parse(outcome.err)["error"]["details"]["reason"] == "explicit_deny");

    environment.write_production_config(
        "default_account = \"main\"\n[accounts.main]\nallow_write = false\n");
    ProcessEnvironment::set_variable("TGCLI_ALLOW_WRITE", "1");
    outcome = run_cli(environment, {"--yes", "logout"});
    REQUIRE(outcome.exit_code == kNotAuthed);
    CHECK(json::parse(outcome.err)["error"]["code"] == "NOT_AUTHED");

    const auto stopped = run_cli(environment, {"daemon", "stop"});
    REQUIRE(stopped.exit_code == kOk);
    CHECK(wait_for_missing(socket, control));
    CHECK(daemon_guard.wait() == kOk);
}

TEST_CASE("account show reports an offline logout observation that cannot open the database",
          "[account][logout][audit][cli][process]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    seed_unconfirmed_logout(environment);

    const auto outcome = run_cli(environment, {"--timeout", "0.2", "account", "show", "main"});
    REQUIRE(outcome.exit_code == kGeneric);
    CHECK(outcome.out.empty());
    const auto error = json::parse(outcome.err);
    CHECK(error["error"]["code"] == "AUDIT_INCOMPLETE");
    CHECK(error["error"]["details"]["mutation_state"] == "possible");
    CHECK(error["error"]["details"]["completed_stages"] ==
          json::array({"intent_synced", "logout_send_started"}));
    CHECK(account_audit(environment).size() == 2);
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
}

TEST_CASE("account show asks a running daemon to reconcile send-started logout audit",
          "[account][logout][audit][cli][process][tdlib]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    environment.write_production_config(
        "default_account = \"main\"\n[accounts.main]\nallow_write = false\n"
        "api_id = 12345\napi_hash = \"0123456789abcdef0123456789abcdef\"\n");
    seed_unconfirmed_logout(environment);

    const std::string daemon_error_path = environment.root() + "/daemon-recovery.err";
    const std::string leak_suppression_path = environment.root() + "/tdlib-lsan.supp";
    {
        std::ofstream suppression(leak_suppression_path, std::ios::binary | std::ios::trunc);
        REQUIRE(suppression.good());
        suppression << "leak:tdsqlite3MemMalloc\n";
    }
    const pid_t daemon_pid = ::fork();
    REQUIRE(daemon_pid >= 0);
    if (daemon_pid == 0) {
        std::string sanitizer_options = "suppressions=" + leak_suppression_path;
        if (const char* inherited = std::getenv("LSAN_OPTIONS");
            inherited != nullptr && *inherited != '\0') {
            sanitizer_options = std::string(inherited) + ":" + sanitizer_options;
        }
        ::setenv("LSAN_OPTIONS", sanitizer_options.c_str(), 1);
        const int null_descriptor = ::open("/dev/null", O_RDWR);
        const int error_descriptor =
            ::open(daemon_error_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (null_descriptor >= 0) {
            ::dup2(null_descriptor, STDIN_FILENO);
            ::dup2(null_descriptor, STDOUT_FILENO);
            ::close(null_descriptor);
        }
        if (error_descriptor >= 0) {
            ::dup2(error_descriptor, STDERR_FILENO);
            ::close(error_descriptor);
        }
        ::execl(TGCLI_TEST_BINARY, "tgcli", "--account", "main", "daemon", "run",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ChildGuard daemon_guard(daemon_pid);
    const std::string socket = environment.root() + "/run/tgcli/main.sock";
    const std::string control = environment.root() + "/run/tgcli/main.ctl";
    REQUIRE(wait_for_exists(socket));

    const auto outcome = run_cli(environment, {"--timeout", "5", "account", "show", "main"});
    INFO(outcome.err);
    REQUIRE(outcome.exit_code == kOk);
    CHECK(outcome.err.empty());
    CHECK(json::parse(outcome.out)["account"] == "main");
    const auto records = account_audit(environment);
    REQUIRE(records.size() == 3);
    CHECK(records.back()["phase"] == "outcome");
    CHECK(records.back()["success"] == false);
    CHECK(records.back()["mutation_state"] == "possible");
    CHECK(records.back()["error"]["code"] == "REMOTE_LOGOUT_UNCONFIRMED");

    const auto stopped = run_cli(environment, {"daemon", "stop"});
    REQUIRE(stopped.exit_code == kOk);
    CHECK(wait_for_missing(socket, control));
    const int daemon_exit = daemon_guard.wait();
    INFO(read_file(daemon_error_path));
    CHECK(daemon_exit == kOk);
}

TEST_CASE("account show bounds an unresponsive running preflight by its original deadline",
          "[account][logout][audit][cli][process][deadline]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    seed_unconfirmed_logout(environment);
    const std::string runtime = environment.root() + "/run/tgcli";
    REQUIRE(std::filesystem::create_directory(runtime));
    REQUIRE(::chmod(runtime.c_str(), 0700) == 0);
    const std::string socket_path = runtime + "/main.sock";
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    REQUIRE(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::chmod(socket_path.c_str(), 0600) == 0);
    REQUIRE(::listen(listener, 1) == 0);

    const auto started = std::chrono::steady_clock::now();
    const auto outcome = run_cli(environment, {"--timeout", "0.03", "account", "show", "main"});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(listener);

    REQUIRE(outcome.exit_code == kGeneric);
    CHECK(outcome.out.empty());
    CHECK(json::parse(outcome.err)["error"]["code"] == "AUDIT_INCOMPLETE");
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(account_audit(environment).size() == 2);
}

TEST_CASE("real CLI freezes explicit environment and default routes across daemon surfaces",
          "[account][routing][cli][process][tdlib]") {
    const ProcessEnvironment environment;
    for (const auto* account : {"explicit", "environment", "configured"}) {
        REQUIRE(run_cli(environment, {"account", "add", account}).exit_code == kOk);
    }
    REQUIRE(run_cli(environment, {"account", "use", "configured"}).exit_code == kOk);

    struct RouteCase {
        std::string account;
        std::optional<std::string> environment_account;
        std::vector<std::string> prefix;
    };
    for (const auto& route :
         {RouteCase{"explicit", std::nullopt, {"--account", "explicit"}},
          RouteCase{"environment", "environment", {}}, RouteCase{"configured", std::nullopt, {}}}) {
        ProcessEnvironment::set_account(route.environment_account);
        auto command = route.prefix;
        command.emplace_back("doctor");
        const auto doctor = run_cli(environment, command);
        INFO(doctor.err);
        REQUIRE(doctor.exit_code == kOk);
        CHECK(doctor.err.empty());
        const auto result = json::parse(doctor.out);
        CHECK(result["account"] == route.account);

        const std::string socket = environment.root() + "/run/tgcli/" + route.account + ".sock";
        const std::string control = environment.root() + "/run/tgcli/" + route.account + ".ctl";
        CHECK(result["daemon"]["socket"] == socket);
        CHECK(wait_for_exists(socket));
        CHECK(wait_for_exists(control));

        command = route.prefix;
        command.insert(command.end(), {"daemon", "stop"});
        const auto stopped = run_cli(environment, command);
        INFO(stopped.err);
        REQUIRE(stopped.exit_code == kOk);
        CHECK(stopped.err.empty());
        CHECK(wait_for_missing(socket, control));
    }
}

TEST_CASE("real CLI freezes implicit main across no-daemon and daemon routing",
          "[account][routing][cli][process][tdlib]") {
    const ProcessEnvironment environment;
    const auto local = run_cli(environment, {"--no-daemon", "doctor"});
    INFO(local.err);
    REQUIRE(local.exit_code == kOk);
    CHECK(local.err.empty());
    CHECK(json::parse(local.out)["account"] == "main");

    const auto remote = run_cli(environment, {"doctor"});
    INFO(remote.err);
    REQUIRE(remote.exit_code == kOk);
    CHECK(remote.err.empty());
    const std::string socket = environment.root() + "/run/tgcli/main.sock";
    const std::string control = environment.root() + "/run/tgcli/main.ctl";
    const auto result = json::parse(remote.out);
    CHECK(result["account"] == "main");
    CHECK(result["daemon"]["socket"] == socket);
    CHECK(wait_for_exists(socket));
    CHECK(wait_for_exists(control));

    const auto stopped = run_cli(environment, {"daemon", "stop"});
    INFO(stopped.err);
    REQUIRE(stopped.exit_code == kOk);
    CHECK(stopped.err.empty());
    CHECK(wait_for_missing(socket, control));
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

TEST_CASE("real CLI removal dry-run stays local and keep-session removes without a daemon",
          "[account][removal][cli][process]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "work"}).exit_code == kOk);

    const auto dry = run_cli(environment, {"--dry-run", "account", "remove", "work"});
    INFO(dry.err);
    REQUIRE(dry.exit_code == kOk);
    CHECK(dry.err.empty());
    const auto plan = json::parse(dry.out);
    CHECK(plan["dry_run"] == true);
    CHECK(plan["plan"]["account"] == "work");
    CHECK(plan["plan"]["remote_logout"] == true);
    CHECK(plan["plan"]["keep_session"] == false);
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli/removals"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli/removals/.work.lock"));

    const auto competing =
        run_cli(environment, {"--account", "other", "account", "remove", "work"});
    REQUIRE(competing.exit_code == kUsage);
    CHECK(json::parse(competing.err)["error"]["details"] ==
          json{{"argument", "--account"}, {"reason", "mutually_exclusive"}});
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));

    const auto removed = run_cli(
        environment, {"--allow-write", "--yes", "account", "remove", "work", "--keep-session"});
    INFO(removed.err);
    REQUIRE(removed.exit_code == kOk);
    CHECK(removed.err.empty());
    CHECK(json::parse(removed.out) == json{{"account", "work"},
                                           {"removed", true},
                                           {"remote_logout", "kept"},
                                           {"default_account", nullptr}});
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli/work.sock"));

    const auto listed = run_cli(environment, {"account", "list"});
    REQUIRE(listed.exit_code == kOk);
    CHECK(json::parse(listed.out) == json{{"items", json::array()}, {"next", nullptr}});
}

TEST_CASE("real CLI default removal auto-spawns proves absent session and stops its daemon",
          "[account][removal][cli][process][tdlib]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "work"}).exit_code == kOk);

    const auto removed = run_cli(
        environment, {"--allow-write", "--yes", "--timeout", "5", "account", "remove", "work"});
    INFO(removed.err);
    REQUIRE(removed.exit_code == kOk);
    CHECK(removed.err.empty());
    CHECK(json::parse(removed.out) == json{{"account", "work"},
                                           {"removed", true},
                                           {"remote_logout", "not_present"},
                                           {"default_account", nullptr}});

    const std::string socket = environment.root() + "/run/tgcli/work.sock";
    const std::string control = environment.root() + "/run/tgcli/work.ctl";
    CHECK(wait_for_missing(socket, control));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli/accounts/work"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli/accounts/work"));
    const auto listed = run_cli(environment, {"account", "list"});
    REQUIRE(listed.exit_code == kOk);
    CHECK(json::parse(listed.out) == json{{"items", json::array()}, {"next", nullptr}});
}

TEST_CASE("real CLI late recovery does not auto-spawn or recreate the removed state root",
          "[account][removal][cli][process][recovery]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "work"}).exit_code == kOk);
    const auto real_environment = paths::real_environment();
    REQUIRE(real_environment.xdg_data_home);
    REQUIRE(real_environment.xdg_state_home);
    const auto data_root = paths::account_data_dir("work", real_environment);
    const auto state_root = paths::account_state_dir("work", real_environment);
    for (const auto& directory :
         {std::filesystem::path(*real_environment.xdg_data_home) / "tgcli",
          std::filesystem::path(*real_environment.xdg_data_home) / "tgcli" / "accounts",
          std::filesystem::path(data_root),
          std::filesystem::path(*real_environment.xdg_state_home) / "tgcli",
          std::filesystem::path(*real_environment.xdg_state_home) / "tgcli" / "accounts",
          std::filesystem::path(state_root)}) {
        REQUIRE(std::filesystem::create_directories(directory));
        REQUIRE(::chmod(directory.c_str(), 0700) == 0);
    }
    { std::ofstream(data_root + "/database") << "data"; }
    { std::ofstream(state_root + "/audit.log") << "state"; }

    const config::Store store(paths::config_file(real_environment), real_environment.uid);
    auto crash_hooks = std::make_shared<daemon::testing::RemovalJournalHooks>();
    crash_hooks->after_tombstone_sync = [](std::string_view, daemon::AuditStage stage) {
        if (stage == daemon::AuditStage::StateRemoved) {
            throw std::runtime_error("injected late-recovery crash");
        }
    };
    daemon::RemovalJournal journal(paths::removals_state_dir(real_environment),
                                   real_environment.uid, crash_hooks);
    ProcessRemovalRemote remote;
    auto coordinator_hooks = std::make_shared<daemon::testing::AccountRemovalHooks>();
    coordinator_hooks->invocation_id = [] {
        return std::string("00112233445566778899aabbccddeeff");
    };
    coordinator_hooks->timestamp = [] { return std::string("2026-08-04T12:00:00Z"); };
    daemon::AccountRemovalCoordinator coordinator(store, journal, real_environment, "work", remote,
                                                  {}, coordinator_hooks);
    proto::Request request("work");
    request.id = 7;
    request.command = {"account", "remove"};
    request.args = {{"account", "work"},
                    {"global_account_supplied", false},
                    {"keep_session", false},
                    {"reassign_default", nullptr}};
    request.context.yes = true;
    request.context.write_authority = proto::WriteAuthority::Grant;
    daemon::CallbackSink sink([](const json&) {}, [](const json&) {}, [](const json&) {},
                              [](const std::string&, const std::string&, const json&, int) {});
    daemon::RequestSession session(std::move(request), sink, 1,
                                   [] { return std::string("ffeeddccbbaa99887766554433221100"); });
    CHECK_THROWS_AS(coordinator.remove(session.request(), session), std::runtime_error);
    CHECK_FALSE(std::filesystem::exists(data_root));
    CHECK_FALSE(std::filesystem::exists(state_root));
    CHECK(run_daemon_direct("work") == kGeneric);
    CHECK_FALSE(std::filesystem::exists(state_root));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli/work.sock"));

    daemon_lock::Identity competing_identity;
    std::string gate_error;
    const std::string gate_file = paths::removals_state_dir(real_environment) + "/.work.lock";
    const int competing_gate = daemon_lock::acquire(gate_file, competing_identity, gate_error);
    INFO(gate_error);
    REQUIRE(competing_gate >= 0);
    const auto blocked = run_cli(
        environment, {"--allow-write", "--yes", "--timeout", "5", "account", "remove", "work"});
    CHECK(blocked.exit_code == kGeneric);
    CHECK_FALSE(std::filesystem::exists(state_root));
    CHECK(::close(competing_gate) == 0);

    const auto recovered = run_cli(
        environment, {"--allow-write", "--yes", "--timeout", "5", "account", "remove", "work"});
    INFO(recovered.err);
    REQUIRE(recovered.exit_code == kOk);
    CHECK(recovered.err.empty());
    CHECK(json::parse(recovered.out) == json{{"account", "work"},
                                             {"removed", true},
                                             {"remote_logout", "not_present"},
                                             {"default_account", nullptr}});
    CHECK_FALSE(std::filesystem::exists(state_root));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli/work.sock"));
    CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli/work.ctl"));
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

TEST_CASE("explicit logout preserves invalid-config routing without starting TD",
          "[logout][cli][process][routing][config][safety]") {
    SECTION("absent daemon returns the preserved current-file error") {
        const ProcessEnvironment environment;
        REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
        environment.write_production_config("[accounts.main\n");

        const auto outcome =
            run_cli(environment, {"--account", "main", "--allow-write", "--yes", "logout"});

        REQUIRE(outcome.exit_code == kGeneric);
        CHECK(outcome.out.empty());
        const auto error = json::parse(outcome.err);
        CHECK(error["error"]["code"] == "CONFIG_INVALID");
        CHECK(error["error"]["details"] ==
              json{{"path", environment.production_config()}, {"reason", "parse_error"}});
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));
    }

    SECTION("no-daemon fails before creating account state or TD data") {
        const ProcessEnvironment environment;
        REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
        environment.write_production_config("[accounts.main\n");

        const auto outcome = run_cli(
            environment, {"--account", "main", "--allow-write", "--yes", "--no-daemon", "logout"});

        REQUIRE(outcome.exit_code == kGeneric);
        CHECK(json::parse(outcome.err)["error"]["code"] == "CONFIG_INVALID");
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));
    }

    SECTION("explicit daemon run remains blocked by invalid current config") {
        const ProcessEnvironment environment;
        REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
        environment.write_production_config("[accounts.main\n");

        const auto outcome = run_cli(environment, {"--account", "main", "daemon", "run"});

        REQUIRE(outcome.exit_code == kGeneric);
        CHECK(json::parse(outcome.err)["error"]["code"] == "CONFIG_INVALID");
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/run/tgcli"));
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/state/tgcli"));
        CHECK_FALSE(std::filesystem::exists(environment.root() + "/data/tgcli"));
    }

    SECTION("existing daemon admits an explicit request grant against last-good config") {
        const ProcessEnvironment environment;
        REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
        const pid_t daemon_pid = ::fork();
        REQUIRE(daemon_pid >= 0);
        if (daemon_pid == 0) {
            const int null_descriptor = ::open("/dev/null", O_RDWR);
            if (null_descriptor >= 0) {
                ::dup2(null_descriptor, STDIN_FILENO);
                ::dup2(null_descriptor, STDOUT_FILENO);
                ::dup2(null_descriptor, STDERR_FILENO);
                ::close(null_descriptor);
            }
            ::execl(TGCLI_TEST_BINARY, "tgcli", "--account", "main", "daemon", "run",
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ChildGuard daemon_guard(daemon_pid);
        const std::string socket = environment.root() + "/run/tgcli/main.sock";
        const std::string control = environment.root() + "/run/tgcli/main.ctl";
        REQUIRE(wait_for_exists(socket));
        environment.write_production_config("[accounts.main\n");

        const auto outcome =
            run_cli(environment, {"--account", "main", "--allow-write", "--yes", "logout"});

        REQUIRE(outcome.exit_code == kNotAuthed);
        CHECK(json::parse(outcome.err)["error"]["code"] == "NOT_AUTHED");
        environment.write_production_config(
            "default_account = \"main\"\n[accounts.main]\nallow_write = false\n");
        const auto stopped = run_cli(environment, {"--account", "main", "daemon", "stop"});
        REQUIRE(stopped.exit_code == kOk);
        CHECK(wait_for_missing(socket, control));
        CHECK(daemon_guard.wait() == kOk);
    }
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

TEST_CASE("real test-DC daemon observes only its config and exits after idle reload",
          "[account][daemon][config-runtime][idle][process][tdlib]") {
    const ProcessEnvironment environment;
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    environment.set_idle_exit(false, 1);

    ProcessEnvironment::set_test_dc(true);
    REQUIRE(run_cli(environment, {"account", "add", "main"}).exit_code == kOk);
    const auto current_environment = paths::real_environment();
    REQUIRE(current_environment.test_dc);
    std::string socket_error;
    const auto socket = paths::socket_path("main", current_environment, socket_error);
    const auto control = paths::control_socket_path("main", current_environment, socket_error);
    REQUIRE(socket.has_value());
    REQUIRE(control.has_value());

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

    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    CHECK(::kill(daemon_pid, 0) == 0);

    const auto replaced_at = std::chrono::steady_clock::now();
    environment.set_idle_exit(true, 1);
    CHECK(daemon_guard.wait_for(std::chrono::milliseconds(2200)) == kOk);
    CHECK(std::chrono::steady_clock::now() - replaced_at < std::chrono::seconds(2));
    CHECK(wait_for_missing(*socket, *control));
}
