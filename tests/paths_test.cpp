#include "common/paths.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

int bind_test_socket(const std::string& socket_path) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    const mode_t old_umask = ::umask(0177);
    const int result = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    ::umask(old_umask);
    if (result != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
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

TEST_CASE("stale endpoint preparation refuses unexpected files", "[paths][socket-safety]") {
    const auto base = std::filesystem::temp_directory_path() /
                      ("tgcli-endpoint-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all);
    const auto endpoint = (base / "main.ctl").string();
    { std::ofstream(endpoint) << "do not remove"; }
    std::filesystem::permissions(endpoint, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_write);

    std::string error;
    CHECK_FALSE(prepare_socket_endpoint(endpoint, getuid(), error));
    CHECK(error.find("not a unix socket") != std::string::npos);
    CHECK(std::filesystem::is_regular_file(endpoint));
    std::filesystem::remove_all(base);
}

TEST_CASE("socket replacement is detected and identity cleanup preserves it",
          "[paths][socket-safety]") {
    const auto base = std::filesystem::temp_directory_path() /
                      ("tgcli-replacement-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all);
    const auto endpoint = (base / "main.ctl").string();

    const int old_fd = bind_test_socket(endpoint);
    REQUIRE(old_fd >= 0);
    std::string error;
    const auto old_identity = inspect_socket_endpoint(endpoint, getuid(), error);
    REQUIRE(old_identity.has_value());
    REQUIRE(::unlink(endpoint.c_str()) == 0);
    const int replacement_fd = bind_test_socket(endpoint);
    REQUIRE(replacement_fd >= 0);

    bool changed = false;
    REQUIRE(socket_endpoint_changed(endpoint, getuid(), *old_identity, changed, error));
    CHECK(changed);
    unlink_socket_endpoint_if_same(endpoint, *old_identity);
    CHECK(std::filesystem::is_socket(endpoint));

    ::close(old_fd);
    ::close(replacement_fd);
    std::filesystem::remove_all(base);
}
