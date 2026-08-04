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
