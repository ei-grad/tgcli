#include "cli/routing.hpp"
#include "common/exit_codes.hpp"

#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;

namespace {

class RoutingTree {
  public:
    RoutingTree() {
        std::string pattern = "/tmp/tgcli-routing-XXXXXX";
        pattern.push_back('\0');
        root_ = ::mkdtemp(pattern.data());
        REQUIRE_FALSE(root_.empty());
    }

    ~RoutingTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    RoutingTree(const RoutingTree&) = delete;
    RoutingTree& operator=(const RoutingTree&) = delete;
    RoutingTree(RoutingTree&&) = delete;
    RoutingTree& operator=(RoutingTree&&) = delete;

    [[nodiscard]] paths::Environment environment(bool test_dc = false) const {
        paths::Environment environment;
        environment.xdg_config_home = root_;
        environment.xdg_data_home = root_ + "/data";
        environment.xdg_state_home = root_ + "/state";
        environment.xdg_runtime_dir = root_ + "/run";
        environment.home = root_;
        environment.uid = ::getuid();
        environment.test_dc = test_dc;
        return environment;
    }

    void write_config(std::string_view bytes, bool test_dc = false) const {
        const std::string directory = root_ + (test_dc ? "/tgcli-test" : "/tgcli");
        REQUIRE(std::filesystem::create_directory(directory));
        REQUIRE(::chmod(directory.c_str(), 0700) == 0);
        const std::string filename = directory + "/config.toml";
        const int fd = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        REQUIRE(fd >= 0);
        REQUIRE(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
        REQUIRE(::close(fd) == 0);
    }

  private:
    std::string root_;
};

} // namespace

TEST_CASE("production routing applies explicit, environment, default, then implicit precedence",
          "[routing][account]") {
    const RoutingTree tree;
    tree.write_config("default_account = \"configured\"\n"
                      "[accounts.explicit]\nallow_write = false\n"
                      "[accounts.environment]\nallow_write = false\n"
                      "[accounts.configured]\nallow_write = false\n");
    const auto environment = tree.environment();

    auto routed = cli::resolve_account_route({"doctor"}, environment, "explicit", "environment");
    REQUIRE(routed.selection.has_value());
    CHECK(routed.selection->name == "explicit");
    CHECK(routed.selection->source == paths::AccountSelectionSource::Explicit);

    routed = cli::resolve_account_route({"doctor"}, environment, std::nullopt, "environment");
    REQUIRE(routed.selection.has_value());
    CHECK(routed.selection->name == "environment");
    CHECK(routed.selection->source == paths::AccountSelectionSource::Environment);

    routed = cli::resolve_account_route({"doctor"}, environment, std::nullopt, std::nullopt);
    REQUIRE(routed.selection.has_value());
    CHECK(routed.selection->name == "configured");
    CHECK(routed.selection->source == paths::AccountSelectionSource::Default);
}

TEST_CASE("routing enforces configured membership and the implicit-main boundary",
          "[routing][account]") {
    const RoutingTree tree;
    const auto environment = tree.environment();

    const auto doctor =
        cli::resolve_account_route({"doctor"}, environment, std::nullopt, std::nullopt);
    REQUIRE(doctor.selection.has_value());
    CHECK(doctor.selection->name == "main");
    CHECK(doctor.selection->source == paths::AccountSelectionSource::ImplicitMain);

    const auto version =
        cli::resolve_account_route({"version"}, environment, std::nullopt, std::nullopt);
    REQUIRE(version.error.has_value());
    CHECK(version.error->code == "ACCOUNT_NOT_FOUND");
    CHECK(version.error->exit_code == kNotFound);
    CHECK(version.error->details == nlohmann::json{{"account", "main"}});
    CHECK_FALSE(std::filesystem::exists(paths::config_file(environment)));

    const auto explicit_missing =
        cli::resolve_account_route({"doctor"}, environment, "main", std::nullopt);
    REQUIRE(explicit_missing.error.has_value());
    CHECK(explicit_missing.error->code == "ACCOUNT_NOT_FOUND");
}

TEST_CASE("routing errors distinguish explicit and environment account syntax",
          "[routing][account]") {
    const RoutingTree tree;
    const auto environment = tree.environment();

    const auto explicit_invalid =
        cli::resolve_account_route({"doctor"}, environment, "bad.name", std::nullopt);
    REQUIRE(explicit_invalid.error.has_value());
    CHECK(explicit_invalid.error->code == "USAGE");
    CHECK(explicit_invalid.error->details ==
          nlohmann::json{{"argument", "--account"}, {"reason", "invalid_argument"}});

    const auto environment_invalid =
        cli::resolve_account_route({"doctor"}, environment, std::nullopt, "bad.name");
    REQUIRE(environment_invalid.error.has_value());
    CHECK(environment_invalid.error->code == "USAGE");
    CHECK(environment_invalid.error->details ==
          nlohmann::json{{"argument", "TGCLI_ACCOUNT"}, {"reason", "invalid_environment"}});
}

TEST_CASE("routing uses isolated production and test-DC current files", "[routing][account]") {
    const RoutingTree tree;
    tree.write_config("default_account = \"prod\"\n[accounts.prod]\nallow_write = false\n");
    tree.write_config("default_account = \"test\"\n[accounts.test]\nallow_write = false\n", true);

    const auto production =
        cli::resolve_account_route({"doctor"}, tree.environment(false), std::nullopt, std::nullopt);
    const auto test =
        cli::resolve_account_route({"doctor"}, tree.environment(true), std::nullopt, std::nullopt);
    REQUIRE(production.selection.has_value());
    REQUIRE(test.selection.has_value());
    CHECK(production.selection->name == "prod");
    CHECK(test.selection->name == "test");
}

TEST_CASE("routing keeps doctor available when the current config is invalid",
          "[routing][account]") {
    const RoutingTree tree;
    tree.write_config("[accounts.main\n");
    const auto environment = tree.environment();

    const auto routed =
        cli::resolve_account_route({"doctor"}, environment, std::nullopt, std::nullopt);
    REQUIRE(routed.selection.has_value());
    CHECK(routed.selection->name == "main");
    CHECK_FALSE(routed.current_config_valid);

    const auto version =
        cli::resolve_account_route({"version"}, environment, std::nullopt, std::nullopt);
    REQUIRE(version.error.has_value());
    CHECK(version.error->code == "CONFIG_INVALID");
    CHECK(version.error->details ==
          nlohmann::json{{"path", paths::config_file(environment)}, {"reason", "parse_error"}});
}
