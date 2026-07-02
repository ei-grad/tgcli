#include "common/exit_codes.hpp"
#include "proto/frame.hpp"

#include <cstdio>
#include <string>
#include <tgcli/version.hpp>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

namespace {

// Last-resort reporter for main's catch blocks: must not itself throw, so it
// formats without allocating. `message` is not JSON-escaped — callers pass
// literals or exception messages, and this path only exists for bugs.
void report_fatal(const char* message) noexcept {
    std::fprintf(stderr, "{\"error\":{\"code\":\"INTERNAL\",\"message\":\"%s\",\"details\":{}}}\n",
                 message);
}

int run(int argc, char** argv) {
    CLI::App app{"tgcli — Telegram CLI"};
    app.require_subcommand(1);

    bool json_output = false;
    app.add_flag("--json", json_output, "machine-readable JSON output");

    CLI::App* version_cmd = app.add_subcommand("version", "print tgcli version");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // CLI11 already prints the message; only the exit code is ours.
        return app.exit(e) == 0 ? tgcli::kOk : tgcli::kUsage;
    }

    if (version_cmd->parsed()) {
        if (json_output) {
            const nlohmann::json out{{"version", tgcli::kVersion},
                                     {"protocol", tgcli::proto::kProtocolVersion}};
            std::puts(out.dump().c_str());
        } else {
            std::printf("tgcli %s\n", tgcli::kVersion);
        }
        return tgcli::kOk;
    }

    return tgcli::kUsage;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        report_fatal(e.what());
        return tgcli::kGeneric;
    } catch (...) { // exit, not continue: anything unnamed here is a bug
        report_fatal("unhandled exception");
        return tgcli::kGeneric;
    }
}
