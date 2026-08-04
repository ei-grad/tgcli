#include "common/config.hpp"
#include "common/config_test_support.hpp"
#include "common/exit_codes.hpp"
#include "daemon/account_commands.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/request_session.hpp"
#include "schema_matcher.hpp"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace tgcli;
using nlohmann::json;

namespace {

class ConfigTree {
  public:
    ConfigTree() {
        std::string pattern = "/tmp/tgcli-account-command-XXXXXX";
        pattern.push_back('\0');
        root_ = ::mkdtemp(pattern.data());
        REQUIRE_FALSE(root_.empty());
    }

    ~ConfigTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ConfigTree(const ConfigTree&) = delete;
    ConfigTree& operator=(const ConfigTree&) = delete;
    ConfigTree(ConfigTree&&) = delete;
    ConfigTree& operator=(ConfigTree&&) = delete;

    [[nodiscard]] std::string config_path() const {
        return root_ + "/config.toml";
    }

    [[nodiscard]] std::string entry_path(std::string_view name) const {
        return root_ + "/" + std::string(name);
    }

    [[nodiscard]] paths::Environment environment() const {
        paths::Environment environment;
        environment.xdg_runtime_dir = root_ + "/run";
        environment.xdg_config_home = root_;
        environment.xdg_data_home = root_ + "/data";
        environment.xdg_state_home = root_ + "/state";
        environment.home = root_;
        environment.uid = ::getuid();
        return environment;
    }

    void write(std::string_view bytes) const {
        write_entry("config.toml", bytes);
    }

    void write_entry(std::string_view name, std::string_view bytes) const {
        const auto file = entry_path(name);
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        REQUIRE(fd >= 0);
        REQUIRE(::fchmod(fd, 0600) == 0);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            REQUIRE(count > 0);
            offset += static_cast<std::size_t>(count);
        }
        REQUIRE(::close(fd) == 0);
    }

    [[nodiscard]] std::string read() const {
        std::ifstream input(config_path(), std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] bool has_mutation_artifacts() const {
        const auto entries = std::filesystem::directory_iterator(root_);
        return std::ranges::any_of(entries, [](const auto& entry) {
            const auto name = entry.path().filename().string();
            return name.starts_with(".config.toml.tmp.") || name == ".config.toml.replacement" ||
                   name == ".config.toml.transaction.committed";
        });
    }

  private:
    std::string root_;
};

class ActiveConfigTransaction {
  public:
    explicit ActiveConfigTransaction(const ConfigTree& tree) {
        const auto lock_path = tree.entry_path("config.lock");
        lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
        REQUIRE(lock_fd_ >= 0);
        REQUIRE(::fchmod(lock_fd_, 0600) == 0);
        REQUIRE(::flock(lock_fd_, LOCK_EX) == 0);
        tree.write_entry(".config.toml.transaction", "active\n");
    }

    ~ActiveConfigTransaction() {
        if (lock_fd_ >= 0) {
            ::close(lock_fd_);
        }
    }

    ActiveConfigTransaction(const ActiveConfigTransaction&) = delete;
    ActiveConfigTransaction& operator=(const ActiveConfigTransaction&) = delete;
    ActiveConfigTransaction(ActiveConfigTransaction&&) = delete;
    ActiveConfigTransaction& operator=(ActiveConfigTransaction&&) = delete;

  private:
    int lock_fd_ = -1;
};

struct Outcome {
    std::optional<json> result;
    std::optional<json> error;
    int exit_code = -1;
};

Outcome dispatch(const daemon::ConfigGlobalContext& context, std::vector<std::string> command,
                 json args, std::optional<double> timeout = std::nullopt) {
    daemon::Dispatcher dispatcher;
    daemon::register_account_commands(dispatcher, context);
    Outcome outcome;
    daemon::CallbackSink sink(
        [](const json&) {}, [](const json&) {},
        [&outcome](json result) {
            outcome.result = std::move(result);
            outcome.exit_code = kOk;
        },
        [&outcome](std::string code, std::string message, json details, int exit_code) {
            outcome.error = json{{"error",
                                  {{"code", std::move(code)},
                                   {"message", std::move(message)},
                                   {"details", std::move(details)}}}};
            outcome.exit_code = exit_code;
        });
    proto::Request request;
    request.id = 1;
    request.command = std::move(command);
    request.args = std::move(args);
    request.context.timeout_seconds = timeout;
    dispatcher.dispatch(request, sink);
    return outcome;
}

json target_args(std::string account, bool global_account = false) {
    return {{"account", std::move(account)}, {"global_account_supplied", global_account}};
}

json list_args(bool global_account = false) {
    return {{"global_account_supplied", global_account}};
}

} // namespace

TEST_CASE("account commands preserve the exact empty and add/list/use results",
          "[account][dispatch][schema]") {
    const ConfigTree tree;
    const config::Store store(tree.config_path());
    const daemon::ConfigGlobalContext context{store, tree.environment()};

    const auto empty = dispatch(context, {"account", "list"}, list_args());
    REQUIRE(empty.result.has_value());
    CHECK(*empty.result == json{{"items", json::array()}, {"next", nullptr}});
    CHECK_THAT(*empty.result, test::matches_json_schema("account-list.result.schema.json"));
    CHECK_FALSE(std::filesystem::exists(tree.config_path()));

    const auto main = dispatch(context, {"account", "add"}, target_args("main"));
    REQUIRE(main.result.has_value());
    CHECK(*main.result == json{{"account", "main"}, {"created", true}, {"default", true}});
    CHECK_THAT(*main.result, test::matches_json_schema("account-add.result.schema.json"));

    const auto work = dispatch(context, {"account", "add"}, target_args("work"));
    REQUIRE(work.result.has_value());
    CHECK(*work.result == json{{"account", "work"}, {"created", true}, {"default", false}});

    const auto listed = dispatch(context, {"account", "list"}, list_args());
    REQUIRE(listed.result.has_value());
    CHECK(*listed.result ==
          json{{"items", json::array({json{{"name", "main"}, {"default", true}},
                                      json{{"name", "work"}, {"default", false}}})},
               {"next", nullptr}});

    const auto used = dispatch(context, {"account", "use"}, target_args("work"));
    REQUIRE(used.result.has_value());
    CHECK(*used.result == json{{"default_account", "work"}, {"previous_default", "main"}});
    CHECK_THAT(*used.result, test::matches_json_schema("account-use.result.schema.json"));
}

TEST_CASE("account show reports only credential sources and isolated derived paths",
          "[account][dispatch][schema][secret]") {
    const ConfigTree tree;
    tree.write("default_account = \"main\"\n"
               "[accounts.main]\n"
               "api_id = 12345\n"
               "api_hash_cmd = \"secret-tool hash\"\n"
               "db_key_cmd = \"secret-tool db\"\n"
               "password_cmd = \"secret-tool password\"\n"
               "bot_token_cmd = \"secret-tool bot\"\n"
               "allow_write = true\n"
               "idle_exit = 45\n");
    const config::Store store(tree.config_path());
    const auto environment = tree.environment();
    const daemon::ConfigGlobalContext context{store, environment};
    std::string socket_error;
    const auto socket = paths::socket_path("main", environment, socket_error);
    REQUIRE(socket.has_value());

    const auto shown = dispatch(context, {"account", "show"}, target_args("main"));
    REQUIRE(shown.result.has_value());
    CHECK(*shown.result == json{{"account", "main"},
                                {"default", true},
                                {"allow_write", true},
                                {"idle_exit", 45},
                                {"credentials",
                                 {{"api_id", "value"},
                                  {"api_hash", "command"},
                                  {"db_key", "command"},
                                  {"password", "command"},
                                  {"bot_token", "command"}}},
                                {"paths",
                                 {{"data", paths::account_data_dir("main", environment)},
                                  {"state", paths::account_state_dir("main", environment)},
                                  {"socket", *socket}}}});
    CHECK_THAT(*shown.result, test::matches_json_schema("account-show.result.schema.json"));
    const auto serialized = shown.result->dump();
    CHECK(serialized.find("secret-tool") == std::string::npos);
    CHECK(serialized.find("12345") == std::string::npos);
}

TEST_CASE("account command failures use exact structured details", "[account][dispatch][schema]") {
    const ConfigTree tree;
    tree.write("default_account = \"main\"\n[accounts.main]\nallow_write = false\n");
    const config::Store store(tree.config_path());
    const daemon::ConfigGlobalContext context{store, tree.environment()};

    const auto duplicate = dispatch(context, {"account", "add"}, target_args("main"));
    REQUIRE(duplicate.error.has_value());
    CHECK(duplicate.exit_code == kUsage);
    CHECK((*duplicate.error)["error"]["code"] == "ACCOUNT_EXISTS");
    CHECK((*duplicate.error)["error"]["details"] == json{{"account", "main"}});
    CHECK_THAT(*duplicate.error, test::matches_json_schema("account.error.schema.json"));

    for (const auto& command : {std::vector<std::string>{"account", "show"},
                                std::vector<std::string>{"account", "use"}}) {
        const auto missing = dispatch(context, command, target_args("missing"));
        REQUIRE(missing.error.has_value());
        CHECK(missing.exit_code == kNotFound);
        CHECK((*missing.error)["error"]["code"] == "ACCOUNT_NOT_FOUND");
        CHECK((*missing.error)["error"]["details"] == json{{"account", "missing"}});
        CHECK_THAT(*missing.error, test::matches_json_schema("account.error.schema.json"));
    }

    const auto invalid = dispatch(context, {"account", "show"}, target_args("bad.name"));
    REQUIRE(invalid.error.has_value());
    CHECK(invalid.exit_code == kUsage);
    CHECK((*invalid.error)["error"]["details"] ==
          json{{"argument", "name"}, {"reason", "invalid_argument"}});

    const auto competing_target = dispatch(context, {"account", "show"}, target_args("main", true));
    REQUIRE(competing_target.error.has_value());
    CHECK(competing_target.exit_code == kUsage);
    CHECK((*competing_target.error)["error"]["details"] ==
          json{{"argument", "--account"}, {"reason", "mutually_exclusive"}});
}

TEST_CASE("account commands always read the current file", "[account][dispatch][config]") {
    const ConfigTree tree;
    tree.write("[accounts.main\n");
    const config::Store store(tree.config_path());
    const daemon::ConfigGlobalContext context{store, tree.environment()};

    const auto outcome = dispatch(context, {"account", "list"}, list_args());
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.exit_code == kGeneric);
    CHECK((*outcome.error)["error"]["code"] == "CONFIG_INVALID");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"path", tree.config_path()}, {"reason", "parse_error"}});
    CHECK_THAT(*outcome.error, test::matches_json_schema("account.error.schema.json"));
}

TEST_CASE("read-only account commands honor their absolute request deadline",
          "[account][dispatch][config]") {
    const ConfigTree tree;
    tree.write("default_account = \"main\"\npadding = \"" + std::string(900'000, 'x') +
               "\"\n[accounts.main]\nallow_write = false\n");
    const config::Store store(tree.config_path());
    const daemon::ConfigGlobalContext context{store, tree.environment()};

    const auto outcome = dispatch(context, {"account", "list"}, list_args(), 0.000001);
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.exit_code == kTimeout);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"operation", "account_list"}, {"state", nullptr}});
    CHECK_THAT(*outcome.error, test::matches_json_schema("account.error.schema.json"));
}

TEST_CASE("read-only account config loads stop at deadline during an active transaction",
          "[account][dispatch][config][deadline]") {
    const ConfigTree tree;
    tree.write("default_account = \"main\"\n[accounts.main]\nallow_write = false\n");
    const auto original = tree.read();
    const ActiveConfigTransaction transaction(tree);
    const config::Store store(tree.config_path());
    const daemon::ConfigGlobalContext context{store, tree.environment()};

    const auto started = std::chrono::steady_clock::now();
    const auto outcome = dispatch(context, {"account", "list"}, list_args(), 0.02);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.exit_code == kTimeout);
    CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
    CHECK((*outcome.error)["error"]["details"] ==
          json{{"operation", "account_list"}, {"state", nullptr}});
    CHECK(elapsed < std::chrono::milliseconds(150));
    CHECK(tree.read() == original);
    CHECK_FALSE(tree.has_mutation_artifacts());
}

TEST_CASE("read-only account config loads stop on disconnect during an active transaction",
          "[account][dispatch][config][cancel]") {
    const ConfigTree tree;
    tree.write("default_account = \"main\"\n[accounts.main]\nallow_write = false\n");
    const auto original = tree.read();
    const ActiveConfigTransaction transaction(tree);
    const config::Store store(tree.config_path());
    const daemon::ConfigGlobalContext context{store, tree.environment()};
    daemon::Dispatcher dispatcher;
    daemon::register_account_commands(dispatcher, context);
    int terminal_count = 0;
    daemon::CallbackSink sink([](const json&) {}, [](const json&) {},
                              [&terminal_count](const json&) { ++terminal_count; },
                              [&terminal_count](const std::string&, const std::string&, const json&,
                                                int) { ++terminal_count; });
    proto::Request request;
    request.id = 1;
    request.command = {"account", "show"};
    request.args = target_args("main");
    request.context.timeout_seconds = 2.0;
    daemon::RequestSession session(std::move(request), sink);

    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&dispatcher, &session] { dispatcher.dispatch(session); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    session.disconnect();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(terminal_count == 0);
    CHECK(elapsed < std::chrono::milliseconds(150));
    CHECK(tree.read() == original);
    CHECK_FALSE(tree.has_mutation_artifacts());
}

TEST_CASE("account mutations expose CAS conflicts and one absolute deadline",
          "[account][dispatch][config]") {
    SECTION("CAS conflict") {
        const ConfigTree tree;
        tree.write("default_account = \"main\"\n[accounts.main]\nallow_write = false\n");
        auto hooks = std::make_shared<config::testing::StoreHooks>();
        hooks->at_stage = [&tree](config::testing::MutationStage stage) {
            if (stage == config::testing::MutationStage::AfterLock) {
                tree.write("default_account = \"main\"\n[accounts.main]\nallow_write = true\n");
            }
        };
        const config::Store store(tree.config_path(), hooks);
        const daemon::ConfigGlobalContext context{store, tree.environment()};

        const auto outcome = dispatch(context, {"account", "add"}, target_args("work"));
        REQUIRE(outcome.error.has_value());
        CHECK(outcome.exit_code == kGeneric);
        CHECK((*outcome.error)["error"]["code"] == "CONFIG_CONFLICT");
        CHECK((*outcome.error)["error"]["details"]["expected"] !=
              (*outcome.error)["error"]["details"]["current"]);
        CHECK_THAT(*outcome.error, test::matches_json_schema("account.error.schema.json"));
    }

    SECTION("deadline while config.lock is held") {
        const ConfigTree tree;
        tree.write("default_account = \"main\"\n[accounts.main]\nallow_write = false\n");
        const std::string lock_path =
            tree.config_path().substr(0, tree.config_path().rfind('/')) + "/config.lock";
        const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
        REQUIRE(lock_fd >= 0);
        REQUIRE(::flock(lock_fd, LOCK_EX) == 0);
        const config::Store store(tree.config_path());
        const daemon::ConfigGlobalContext context{store, tree.environment()};

        const auto started = std::chrono::steady_clock::now();
        const auto outcome = dispatch(context, {"account", "add"}, target_args("work"), 0.02);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        ::close(lock_fd);

        REQUIRE(outcome.error.has_value());
        CHECK(outcome.exit_code == kTimeout);
        CHECK((*outcome.error)["error"]["code"] == "TIMEOUT");
        CHECK((*outcome.error)["error"]["details"] ==
              json{{"operation", "account_add"}, {"state", nullptr}});
        CHECK(elapsed < std::chrono::seconds(1));
    }
}
