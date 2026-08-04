#include "cli/render.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

std::string golden(std::string_view filename) {
    const std::ifstream input(std::string(TGCLI_GOLDEN_DIR) + "/" + std::string(filename));
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

} // namespace

TEST_CASE("account human renderers match reviewed goldens", "[account][render][golden]") {
    CHECK(tgcli::cli::render_human("account add",
                                   {{"account", "work"}, {"created", true}, {"default", false}}) ==
          golden("account-add.txt"));
    CHECK(tgcli::cli::render_human(
              "account list", {{"items", json::array({json{{"name", "main"}, {"default", true}},
                                                      json{{"name", "work"}, {"default", false}}})},
                               {"next", nullptr}}) == golden("account-list.txt"));
    CHECK(tgcli::cli::render_human("account list", {{"items", json::array()}, {"next", nullptr}}) ==
          golden("account-list-empty.txt"));
    CHECK(tgcli::cli::render_human("account show", {{"account", "main"},
                                                    {"default", true},
                                                    {"allow_write", false},
                                                    {"idle_exit", nullptr},
                                                    {"credentials",
                                                     {{"api_id", "value"},
                                                      {"api_hash", "command"},
                                                      {"db_key", "none"},
                                                      {"password", "interactive"},
                                                      {"bot_token", "command"}}},
                                                    {"paths",
                                                     {{"data", "/data/tgcli/accounts/main"},
                                                      {"state", "/state/tgcli/accounts/main"},
                                                      {"socket", "/run/tgcli/main.sock"}}}}) ==
          golden("account-show.txt"));
    CHECK(tgcli::cli::render_human("account use",
                                   {{"default_account", "work"}, {"previous_default", "main"}}) ==
          golden("account-use.txt"));
}

TEST_CASE("login and me human renderers match reviewed goldens", "[auth][render][golden]") {
    const json user{{"id", 123456},
                    {"first_name", "Ada"},
                    {"last_name", "Lovelace"},
                    {"usernames", json::array({"ada", "analytical_engine"})},
                    {"phone_number", "12025550123"},
                    {"is_bot", false},
                    {"is_premium", true}};
    CHECK(tgcli::cli::render_human("me", user) == golden("me.txt"));
    CHECK(tgcli::cli::render_human(
              "login", {{"account", "main"}, {"auth_state", "ready"}, {"user", user}}) ==
          golden("login.txt"));
}

TEST_CASE("logout human renderers match reviewed goldens", "[logout][render][golden]") {
    CHECK(tgcli::cli::render_human("logout", {{"account", "main"}, {"logged_out", true}}) ==
          golden("logout.txt"));
    CHECK(tgcli::cli::render_human("logout", {{"dry_run", true},
                                              {"plan",
                                               {{"operation", "logout"},
                                                {"account", "main"},
                                                {"remote_logout", true},
                                                {"tdlib_request", "logOut"}}}}) ==
          golden("logout-dry-run.txt"));
}

TEST_CASE("daemon control human renderers match reviewed goldens",
          "[daemon-control][render][golden]") {
    CHECK(tgcli::cli::render_human("daemon status", {{"account", "main"},
                                                     {"running", true},
                                                     {"pid", 123},
                                                     {"version", "0.1.0"},
                                                     {"protocol", 1},
                                                     {"socket", "/run/tgcli/main.sock"}}) ==
          golden("daemon-status-running.txt"));
    CHECK(tgcli::cli::render_human(
              "daemon status",
              {{"account", "main"}, {"running", false}, {"socket", "/run/tgcli/main.sock"}}) ==
          golden("daemon-status-absent.txt"));
    CHECK(tgcli::cli::render_human("daemon stop", {{"stopping", true}}) ==
          golden("daemon-stop.txt"));
    CHECK(tgcli::cli::render_human("daemon restart", {{"account", "main"},
                                                      {"restarted", true},
                                                      {"pid", 124},
                                                      {"version", "0.1.0"},
                                                      {"protocol", 1},
                                                      {"socket", "/run/tgcli/main.sock"}}) ==
          golden("daemon-restart.txt"));
}
