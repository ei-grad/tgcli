#include "cli/render.hpp"

#include <fmt/format.h>

namespace tgcli::cli {

namespace {

std::string render_version(const nlohmann::json& data) {
    return fmt::format("tgcli {} (protocol {}, tdlib {})\n",
                       data.value("version", std::string("?")), data.value("protocol", 0),
                       data.value("tdlib", std::string("?")));
}

std::string render_doctor(const nlohmann::json& data) {
    std::string out;
    out += fmt::format("account: {}\n", data.value("account", std::string("?")));
    if (data.contains("daemon")) {
        const auto& daemon = data["daemon"];
        if (daemon.value("running", false)) {
            out += fmt::format("daemon:  running (pid {})", daemon.value("pid", 0));
            if (daemon.contains("socket")) {
                out += fmt::format(", socket {}", daemon["socket"].get<std::string>());
            }
            out += '\n';
        } else if (daemon.value("in_process", false)) {
            out += "daemon:  bypassed (--no-daemon)\n";
        } else {
            out += "daemon:  not running\n";
            if (daemon.contains("socket")) {
                out += fmt::format("socket:  {} (absent)\n", daemon["socket"].get<std::string>());
            }
        }
    }
    if (data.contains("tdlib")) {
        out += fmt::format("tdlib:   {}\n", data["tdlib"].value("version", std::string("?")));
    }
    if (data.contains("config")) {
        out += fmt::format("config:  {}{}\n", data["config"].value("path", std::string("?")),
                           data["config"].value("exists", false) ? "" : " (missing)");
    }
    if (data.contains("auth")) {
        out += fmt::format("auth:    {}\n", data["auth"].value("state", std::string("?")));
    }
    return out;
}

} // namespace

std::string render_human(const std::string& command_key, const nlohmann::json& data) {
    if (command_key == "version") {
        return render_version(data);
    }
    if (command_key == "doctor") {
        return render_doctor(data);
    }
    // Until a command grows a dedicated renderer, readable JSON is the
    // honest fallback.
    return data.dump(2) + "\n";
}

} // namespace tgcli::cli
