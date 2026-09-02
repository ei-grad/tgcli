#include "common/cancellation.hpp"
#include "common/secret_hook.hpp"
#include "common/secret_hook_test_support.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;
using namespace tgcli::secret_hook;

namespace {

class ScopedEnvironment {
  public:
    ScopedEnvironment(const char* name, std::optional<std::string> value) : name_(name) {
        if (const char* current = std::getenv(name); current != nullptr) {
            old_ = current;
        }
        if (value) {
            REQUIRE(::setenv(name, value->c_str(), 1) == 0);
        } else {
            REQUIRE(::unsetenv(name) == 0);
        }
    }

    ~ScopedEnvironment() {
        if (old_) {
            ::setenv(name_.c_str(), old_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
    ScopedEnvironment(ScopedEnvironment&&) = delete;
    ScopedEnvironment& operator=(ScopedEnvironment&&) = delete;

  private:
    std::string name_;
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("trusted hook returns one opaque value", "[hook]") {
    HookRequest request{HookField::ApiHash, "printf ' value '", std::nullopt};
    const auto result = run(request);
    REQUIRE(result);
    CHECK(result.value == " value ");

    request.command = "printf 'value\\r\\n'";
    const auto crlf = run(request);
    REQUIRE(crlf);
    CHECK(crlf.value == "value");
}

TEST_CASE("trusted hook validates canonical api id", "[hook]") {
    std::int32_t value = 0;
    CHECK(parse_api_id("2147483647", value));
    CHECK(value == 2147483647);
    CHECK_FALSE(parse_api_id("0", value));
    CHECK_FALSE(parse_api_id("01", value));
    CHECK_FALSE(parse_api_id("2147483648", value));
    CHECK_FALSE(parse_api_id(" 123", value));
}

TEST_CASE("trusted hook rejects invalid stdout records", "[hook]") {
    const auto check = [](std::string command, HookFailure expected) {
        const auto result = run({HookField::Password, std::move(command), std::nullopt});
        REQUIRE_FALSE(result);
        CHECK(result.error->reason == expected);
    };

    check("true", HookFailure::StdoutEmpty);
    check("printf 'one\\ntwo\\n'", HookFailure::StdoutInvalid);
    check("printf 'one\\000two'", HookFailure::StdoutInvalid);
    check("head -c 65537 /dev/zero | tr '\\000' x", HookFailure::StdoutTooLarge);
    check("while :; do printf x; done", HookFailure::StdoutTooLarge);
}

TEST_CASE("trusted hook reports nonzero signal timeout and stderr overflow without output",
          "[hook]") {
    SECTION("nonzero exit redacts stderr") {
        const auto result =
            run({HookField::BotToken, "printf 'sensitive-stderr' >&2; exit 23", std::nullopt});
        REQUIRE_FALSE(result);
        REQUIRE(result.error);
        CHECK(result.error->reason == HookFailure::Exit);
        CHECK(result.error->status == 23);
        const auto public_error = describe(*result.error);
        CHECK(public_error.find("sensitive") == std::string::npos);
        CHECK(public_error.find("printf") == std::string::npos);
    }
    SECTION("signal") {
        const auto result = run({HookField::DatabaseKey, "kill -TERM $$", std::nullopt});
        REQUIRE_FALSE(result);
        CHECK(result.error->reason == HookFailure::Signal);
        CHECK(result.error->status == SIGTERM);
    }
    SECTION("timeout kills the process group") {
        const auto deadline = std::chrono::steady_clock::now() + 150ms;
        const auto started = std::chrono::steady_clock::now();
        const auto result = run({HookField::Password, "sleep 30 & wait", deadline});
        const auto elapsed = std::chrono::steady_clock::now() - started;
        REQUIRE_FALSE(result);
        CHECK(result.error->reason == HookFailure::Timeout);
        CHECK(elapsed < 3s);
    }
    SECTION("stderr is bounded independently") {
        const auto result =
            run({HookField::ApiHash, "head -c 65537 /dev/zero | tr '\\000' x >&2; printf ok",
                 std::nullopt});
        REQUIRE_FALSE(result);
        CHECK(result.error->reason == HookFailure::StderrTooLarge);
    }
}

TEST_CASE("trusted hook receives only the inherited allowlist", "[hook]") {
    REQUIRE(std::getenv("TGCLI_TEST_SECRET") == nullptr);
    REQUIRE(std::getenv("UNLISTED_SECRET") == nullptr);
    REQUIRE(::setenv("TGCLI_TEST_SECRET", "must-not-leak", 1) == 0);
    REQUIRE(::setenv("UNLISTED_SECRET", "must-not-leak", 1) == 0);

    const auto result = run({HookField::Password,
                             "test -z \"${TGCLI_TEST_SECRET+x}\" && "
                             "test -z \"${UNLISTED_SECRET+x}\" && "
                             "test -n \"$HOME\" && printf isolated",
                             std::nullopt});

    REQUIRE(::unsetenv("TGCLI_TEST_SECRET") == 0);
    REQUIRE(::unsetenv("UNLISTED_SECRET") == 0);
    REQUIRE(result);
    CHECK(result.value == "isolated");
}

TEST_CASE("trusted hook fixes cwd stdin and the complete inherited environment", "[hook]") {
    constexpr std::array<const char*, 13> names = {"HOME",           "PATH",
                                                   "LANG",           "LC_ALL",
                                                   "LC_CTYPE",       "XDG_CONFIG_HOME",
                                                   "XDG_DATA_HOME",  "XDG_STATE_HOME",
                                                   "XDG_CACHE_HOME", "XDG_RUNTIME_DIR",
                                                   "GNUPGHOME",      "PASSWORD_STORE_DIR",
                                                   "SSH_AUTH_SOCK"};
    std::vector<std::unique_ptr<ScopedEnvironment>> environment;
    std::string command = "test \"$PWD\" = / && ! read ignored";
    for (const char* name : names) {
        const std::string value = "tgcli_test_" + std::string(name);
        environment.push_back(std::make_unique<ScopedEnvironment>(name, value));
        command += " && test \"$" + std::string(name) + "\" = \"" + value + "\"";
    }
    command += " && printf isolated";

    const auto result = run({HookField::Password, command, std::nullopt});
    REQUIRE(result);
    CHECK(result.value == "isolated");
}

TEST_CASE("trusted hook supplies a fixed PATH when the parent has none", "[hook]") {
    const ScopedEnvironment path("PATH", std::nullopt);
    const auto result = run(
        {HookField::Password, "test \"$PATH\" = /usr/bin:/bin && printf fixed-path", std::nullopt});
    REQUIRE(result);
    CHECK(result.value == "fixed-path");
}

TEST_CASE("trusted hook deadline is authoritative before spawn and before acceptance", "[hook]") {
    SECTION("already expired") {
        int spawned = 0;
        testing::RunHooks hooks;
        hooks.on_spawn = [&](pid_t) { ++spawned; };
        const auto result = testing::run(
            {HookField::Password, "printf should-not-run", std::chrono::steady_clock::now()},
            hooks);
        REQUIRE_FALSE(result);
        CHECK(result.error->reason == HookFailure::Timeout);
        CHECK(spawned == 0);
    }
    SECTION("completion at the deadline is not accepted") {
        const auto deadline = std::chrono::steady_clock::now() + 50ms;
        testing::RunHooks hooks;
        hooks.before_accept = [deadline] { std::this_thread::sleep_until(deadline); };
        const auto result =
            testing::run({HookField::Password, "printf completed", deadline}, hooks);
        REQUIRE_FALSE(result);
        CHECK(result.error->reason == HookFailure::Timeout);
    }
}

TEST_CASE("trusted hook timeout leaves no process in its process group", "[hook]") {
    pid_t process_group = -1;
    testing::RunHooks hooks;
    hooks.on_spawn = [&](pid_t child) { process_group = child; };
    const auto result = testing::run(
        {HookField::Password, "sleep 30 & wait", std::chrono::steady_clock::now() + 100ms}, hooks);
    REQUIRE_FALSE(result);
    CHECK(result.error->reason == HookFailure::Timeout);
    REQUIRE(process_group > 0);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (::kill(-process_group, 0) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(::kill(-process_group, 0) == -1);
    CHECK(errno == ESRCH);
}

TEST_CASE("trusted hook cancellation kills and reaps its process group promptly", "[hook]") {
    pid_t process_group = -1;
    tgcli::cancellation::Source cancellation;
    testing::RunHooks hooks;
    hooks.on_spawn = [&](pid_t child) {
        process_group = child;
        static_cast<void>(cancellation.request_stop());
    };
    const auto started = std::chrono::steady_clock::now();
    const auto result = testing::run(
        {HookField::DatabaseKey, "sleep 30 & wait", std::nullopt, cancellation.get_token()}, hooks);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(result);
    CHECK(result.cancelled);
    CHECK_FALSE(result.error);
    CHECK(elapsed < 3s);
    REQUIRE(process_group > 0);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (::kill(-process_group, 0) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(::kill(-process_group, 0) == -1);
    CHECK(errno == ESRCH);
}
