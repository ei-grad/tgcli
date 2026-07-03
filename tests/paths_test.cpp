#include "common/paths.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli::paths;

namespace {

Environment fake_env() {
    Environment env;
    env.xdg_runtime_dir = "/run/user/1000";
    env.xdg_config_home = std::nullopt;
    env.xdg_data_home = std::nullopt;
    env.xdg_state_home = std::nullopt;
    env.tmpdir = std::nullopt;
    env.home = "/home/user";
    env.uid = 1000;
    return env;
}

} // namespace

TEST_CASE("account name validation", "[paths]") {
    CHECK(valid_account_name("main"));
    CHECK(valid_account_name("work-2"));
    CHECK(valid_account_name("A_b3"));
    CHECK_FALSE(valid_account_name(""));
    CHECK_FALSE(valid_account_name("has space"));
    CHECK_FALSE(valid_account_name("dot.dot"));
    CHECK_FALSE(valid_account_name("slash/evil"));
    CHECK_FALSE(valid_account_name(std::string(kMaxAccountNameLength + 1, 'a')));
    CHECK(valid_account_name(std::string(kMaxAccountNameLength, 'a')));
}

TEST_CASE("socket path prefers XDG_RUNTIME_DIR", "[paths]") {
    std::string error;
    auto path = socket_path("main", fake_env(), error);
    REQUIRE(path.has_value());
    CHECK(*path == "/run/user/1000/tgcli/main.sock");
}

TEST_CASE("socket path falls back to TMPDIR then /tmp with uid isolation", "[paths]") {
    auto env = fake_env();
    env.xdg_runtime_dir = std::nullopt;
    std::string error;

    SECTION("TMPDIR set") {
        env.tmpdir = "/var/tmp-x";
        auto path = socket_path("main", env, error);
        REQUIRE(path.has_value());
        CHECK(*path == "/var/tmp-x/tgcli-1000/main.sock");
    }
    SECTION("TMPDIR unset") {
        auto path = socket_path("main", env, error);
        REQUIRE(path.has_value());
        CHECK(*path == "/tmp/tgcli-1000/main.sock");
    }
}

TEST_CASE("socket path rejects invalid names and over-long paths", "[paths]") {
    std::string error;
    CHECK_FALSE(socket_path("../oops", fake_env(), error).has_value());
    CHECK(error.find("invalid account name") != std::string::npos);

    auto env = fake_env();
    env.xdg_runtime_dir = std::string(100, 'd');
    CHECK_FALSE(socket_path("main", env, error).has_value());
    CHECK(error.find("sun_path") != std::string::npos);
}

TEST_CASE("XDG layout matches DESIGN.md §9", "[paths]") {
    const auto env = fake_env();
    CHECK(config_file(env) == "/home/user/.config/tgcli/config.toml");
    CHECK(account_data_dir("main", env) == "/home/user/.local/share/tgcli/accounts/main");
    CHECK(account_state_dir("work", env) == "/home/user/.local/state/tgcli/accounts/work");

    Environment overridden = env;
    overridden.xdg_config_home = "/cfg";
    overridden.xdg_data_home = "/data";
    overridden.xdg_state_home = "/state";
    CHECK(config_file(overridden) == "/cfg/tgcli/config.toml");
    CHECK(account_data_dir("main", overridden) == "/data/tgcli/accounts/main");
    CHECK(account_state_dir("main", overridden) == "/state/tgcli/accounts/main");
}

TEST_CASE("ensure_private_dir creates 0700 and rejects tampering", "[paths]") {
    const auto base = std::filesystem::temp_directory_path() / "tgcli-paths-test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    std::string error;
    const auto good = (base / "private").string();
    REQUIRE(ensure_private_dir(good, getuid(), error));
    CHECK(std::filesystem::status(good).permissions() == (std::filesystem::perms::owner_all));
    // Idempotent on a healthy directory.
    CHECK(ensure_private_dir(good, getuid(), error));

    SECTION("rejects a non-directory") {
        const auto file = (base / "file").string();
        { std::ofstream(file) << ""; }
        CHECK_FALSE(ensure_private_dir(file, getuid(), error));
        CHECK(error.find("not a directory") != std::string::npos);
    }
    SECTION("rejects group/other access") {
        const auto loose = (base / "loose").string();
        std::filesystem::create_directory(loose);
        std::filesystem::permissions(loose, std::filesystem::perms::owner_all |
                                                std::filesystem::perms::group_read);
        CHECK_FALSE(ensure_private_dir(loose, getuid(), error));
        CHECK(error.find("group/other") != std::string::npos);
    }
    SECTION("rejects a foreign owner") {
        CHECK_FALSE(ensure_private_dir(good, getuid() + 1, error));
        CHECK(error.find("owned by uid") != std::string::npos);
    }
    std::filesystem::remove_all(base);
}
