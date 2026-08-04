#include "cli/render.hpp"

#include <cstdint>

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

std::string yes_no(bool value) {
    return value ? "yes" : "no";
}

std::string render_account_add(const nlohmann::json& data) {
    return fmt::format("account: {}\ncreated: {}\ndefault: {}\n",
                       data.value("account", std::string("?")),
                       yes_no(data.value("created", false)), yes_no(data.value("default", false)));
}

std::string render_account_list(const nlohmann::json& data) {
    std::string out = "accounts:";
    const auto& items = data.at("items");
    if (items.empty()) {
        out += " (none)\n";
    } else {
        out += '\n';
        for (const auto& item : items) {
            out += fmt::format("- {}{}\n", item.value("name", std::string("?")),
                               item.value("default", false) ? " (default)" : "");
        }
    }
    out += "next: null\n";
    return out;
}

std::string render_account_show(const nlohmann::json& data) {
    const auto& credentials = data.at("credentials");
    const auto& account_paths = data.at("paths");
    const std::string idle_exit = data["idle_exit"].is_null()
                                      ? "disabled"
                                      : std::to_string(data["idle_exit"].get<std::int64_t>());
    return fmt::format(
        "account: {}\ndefault: {}\nallow write: {}\nidle exit: {}\ncredentials:\n"
        "  api_id: {}\n  api_hash: {}\n  db_key: {}\n  password: {}\n  bot_token: {}\n"
        "paths:\n  data: {}\n  state: {}\n  socket: {}\n",
        data.value("account", std::string("?")), yes_no(data.value("default", false)),
        yes_no(data.value("allow_write", false)), idle_exit,
        credentials.value("api_id", std::string("?")),
        credentials.value("api_hash", std::string("?")),
        credentials.value("db_key", std::string("?")),
        credentials.value("password", std::string("?")),
        credentials.value("bot_token", std::string("?")),
        account_paths.value("data", std::string("?")),
        account_paths.value("state", std::string("?")),
        account_paths.value("socket", std::string("?")));
}

std::string render_account_use(const nlohmann::json& data) {
    const std::string previous =
        data["previous_default"].is_null() ? "none" : data["previous_default"].get<std::string>();
    return fmt::format("default account: {}\nprevious default: {}\n",
                       data.value("default_account", std::string("?")), previous);
}

std::string render_daemon_status(const nlohmann::json& data) {
    std::string out =
        fmt::format("account: {}\nrunning: {}\n", data.value("account", std::string("?")),
                    yes_no(data.value("running", false)));
    if (data.value("running", false)) {
        out +=
            fmt::format("pid: {}\nversion: {}\nprotocol: {}\n", data.value("pid", std::int64_t{0}),
                        data.value("version", std::string("?")), data.value("protocol", 0));
    }
    out += fmt::format("socket: {}\n", data.value("socket", std::string("?")));
    return out;
}

std::string render_daemon_restart(const nlohmann::json& data) {
    return fmt::format("account: {}\nrestarted: {}\npid: {}\nversion: {}\nprotocol: {}\n"
                       "socket: {}\n",
                       data.value("account", std::string("?")),
                       yes_no(data.value("restarted", false)), data.value("pid", std::int64_t{0}),
                       data.value("version", std::string("?")), data.value("protocol", 0),
                       data.value("socket", std::string("?")));
}

std::string render_user(const nlohmann::json& data) {
    std::string usernames;
    for (const auto& username : data.at("usernames")) {
        if (!usernames.empty()) {
            usernames += ", ";
        }
        usernames += username.get<std::string>();
    }
    return fmt::format("id: {}\nname: {} {}\nusernames: {}\nphone number: {}\nbot: {}\n"
                       "premium: {}\n",
                       data.value("id", std::int64_t{0}), data.value("first_name", std::string{}),
                       data.value("last_name", std::string{}), usernames,
                       data.value("phone_number", std::string{}),
                       yes_no(data.value("is_bot", false)),
                       yes_no(data.value("is_premium", false)));
}

std::string render_login(const nlohmann::json& data) {
    return fmt::format("account: {}\nauth state: {}\n{}", data.value("account", std::string("?")),
                       data.value("auth_state", std::string("?")), render_user(data.at("user")));
}

} // namespace

std::string render_human(const std::string& command_key, const nlohmann::json& data) {
    if (command_key == "version") {
        return render_version(data);
    }
    if (command_key == "doctor") {
        return render_doctor(data);
    }
    if (command_key == "account add") {
        return render_account_add(data);
    }
    if (command_key == "account list") {
        return render_account_list(data);
    }
    if (command_key == "account show") {
        return render_account_show(data);
    }
    if (command_key == "account use") {
        return render_account_use(data);
    }
    if (command_key == "daemon status") {
        return render_daemon_status(data);
    }
    if (command_key == "daemon stop") {
        return fmt::format("stopping: {}\n", yes_no(data.value("stopping", false)));
    }
    if (command_key == "daemon restart") {
        return render_daemon_restart(data);
    }
    if (command_key == "login") {
        return render_login(data);
    }
    if (command_key == "me") {
        return render_user(data);
    }
    // Until a command grows a dedicated renderer, readable JSON is the
    // honest fallback.
    return data.dump(2) + "\n";
}

} // namespace tgcli::cli
