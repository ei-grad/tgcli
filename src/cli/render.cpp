#include "cli/render.hpp"

#include <cstdint>
#include <string_view>

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

std::string render_account_remove(const nlohmann::json& data) {
    if (data.value("dry_run", false)) {
        return data.dump(2) + "\n";
    }
    const std::string next_default =
        data["default_account"].is_null() ? "none" : data["default_account"].get<std::string>();
    return fmt::format("account: {}\nremoved: {}\nremote logout: {}\ndefault account: {}\n",
                       data.value("account", std::string("?")),
                       yes_no(data.value("removed", false)),
                       data.value("remote_logout", std::string("?")), next_default);
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

std::string render_logout(const nlohmann::json& data) {
    if (data.value("dry_run", false)) {
        const auto& plan = data.at("plan");
        return fmt::format("dry run: yes\noperation: logout\naccount: {}\nremote logout: yes\n"
                           "tdlib request: {}\n",
                           plan.value("account", std::string("?")),
                           plan.value("tdlib_request", std::string("?")));
    }
    return fmt::format("account: {}\nlogged out: {}\n", data.value("account", std::string("?")),
                       yes_no(data.value("logged_out", false)));
}

std::string render_saved_tags(const nlohmann::json& data) {
    std::string out = "tag\tcount\tlabel\n";
    for (const auto& item : data.at("items")) {
        out += fmt::format("{}\t{}\t{}\n", item.at("tag").get<std::string>(),
                           item.at("count").get<std::int32_t>(), item.at("label").dump());
    }
    out += "next: null\n";
    return out;
}

std::string render_saved_search(const nlohmann::json& data) {
    std::string out = "id\tchat_id\tdate\ttext\n";
    for (const auto& item : data.at("items")) {
        out += fmt::format("{}\t{}\t{}\t{}\n", item.at("id").get<std::int64_t>(),
                           item.at("chat_id").get<std::int64_t>(),
                           item.at("date").get<std::string>(), item.at("text").dump());
    }
    out += data.at("next").is_null()
               ? "next: null\n"
               : fmt::format("next: {}\n", data.at("next").get<std::string>());
    return out;
}

std::string_view true_false(bool value) {
    return value ? "true" : "false";
}

void append_json_member(std::string& out, std::string_view name, const nlohmann::json& value) {
    if (out.back() != '{') {
        out += ',';
    }
    out += '"';
    out += name;
    out += "\":";
    out += value.dump();
}

std::string render_session_target_json(const nlohmann::json& session) {
    std::string out{"{"};
    append_json_member(out, "id", session.at("id"));
    append_json_member(out, "is_current", session.at("is_current"));
    append_json_member(out, "is_password_pending", session.at("is_password_pending"));
    append_json_member(out, "is_unconfirmed", session.at("is_unconfirmed"));
    append_json_member(out, "device_type", session.at("device_type"));
    append_json_member(out, "application_name", session.at("application_name"));
    append_json_member(out, "application_version", session.at("application_version"));
    append_json_member(out, "device_model", session.at("device_model"));
    append_json_member(out, "platform", session.at("platform"));
    append_json_member(out, "system_version", session.at("system_version"));
    append_json_member(out, "last_active_date", session.at("last_active_date"));
    out += '}';
    return out;
}

std::string render_session_plan_json(const nlohmann::json& plan) {
    std::string out{"{"};
    append_json_member(out, "operation", plan.at("operation"));
    append_json_member(out, "account", plan.at("account"));
    append_json_member(out, "tdlib_request", plan.at("tdlib_request"));
    if (out.back() != '{') {
        out += ',';
    }
    out += "\"session\":";
    out += render_session_target_json(plan.at("session"));
    out += '}';
    return out;
}

std::string render_session_list(const nlohmann::json& data) {
    std::string out =
        "id\tcurrent\tpassword_pending\tunconfirmed\tdevice\tapi_id\tapplication\t"
        "application_version\tofficial\tdevice_model\tplatform\tsystem_version\tlogin\t"
        "last_active\tip\tlocation\taccept_secret_chats\taccept_calls\n";
    for (const auto& item : data.at("items")) {
        out += fmt::format(
            "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            item.at("id").get_ref<const std::string&>(),
            true_false(item.at("is_current").get<bool>()),
            true_false(item.at("is_password_pending").get<bool>()),
            true_false(item.at("is_unconfirmed").get<bool>()), item.at("device_type").dump(),
            item.at("api_id").get<std::int32_t>(), item.at("application_name").dump(),
            item.at("application_version").dump(),
            true_false(item.at("is_official_application").get<bool>()),
            item.at("device_model").dump(), item.at("platform").dump(),
            item.at("system_version").dump(), item.at("log_in_date").dump(),
            item.at("last_active_date").dump(), item.at("ip_address").dump(),
            item.at("location").dump(), true_false(item.at("can_accept_secret_chats").get<bool>()),
            true_false(item.at("can_accept_calls").get<bool>()));
    }
    out += fmt::format("inactive_session_ttl_days\t{}\nnext\tnull\n",
                       data.at("inactive_session_ttl_days").get<std::int32_t>());
    return out;
}

std::string render_session_terminate(const nlohmann::json& data) {
    if (data.contains("dry_run")) {
        return fmt::format("dry_run\ttrue\nplan\t{}\n", render_session_plan_json(data.at("plan")));
    }
    return fmt::format("session_id\t{}\nterminated\ttrue\n",
                       data.at("session_id").get_ref<const std::string&>());
}

std::string render_string_array(const nlohmann::json& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value.dump();
    }
    return out.empty() ? "(none)" : out;
}

std::string render_integer_array(const nlohmann::json& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value.dump();
    }
    return out.empty() ? "(none)" : out;
}

std::string render_chats(const nlohmann::json& data) {
    std::string out;
    const auto& items = data.at("items");
    if (items.empty()) {
        out = "chats: (none)\n";
    }
    for (const auto& chat : items) {
        out += fmt::format(
            "chat:\n  id: {}\n  title: {}\n  type: {}\n  bot: {}\n  usernames: {}\n"
            "  archived: {}\n  folder ids: {}\n  marked unread: {}\n  unread: {}\n"
            "  mentions: {}\n  reactions: {}\n  poll votes: {}\n",
            chat.at("id").get<std::int64_t>(), chat.at("title").dump(),
            chat.at("type").get<std::string>(), yes_no(chat.at("is_bot").get<bool>()),
            render_string_array(chat.at("usernames")), yes_no(chat.at("is_archived").get<bool>()),
            render_integer_array(chat.at("folder_ids")),
            yes_no(chat.at("is_marked_unread").get<bool>()),
            chat.at("unread_count").get<std::int32_t>(),
            chat.at("unread_mention_count").get<std::int32_t>(),
            chat.at("unread_reaction_count").get<std::int32_t>(),
            chat.at("unread_poll_vote_count").get<std::int32_t>());
        const auto& message = chat.at("last_message");
        if (message.is_null()) {
            out += "  last message: null\n";
            continue;
        }
        const std::string date =
            message.at("date").is_null() ? "null" : message.at("date").get<std::string>();
        const std::string topic =
            message.at("topic").is_null()
                ? "null"
                : fmt::format("{}:{}", message.at("topic").at("kind").get<std::string>(),
                              message.at("topic").at("id").get<std::int64_t>());
        out += fmt::format(
            "  last message:\n    id: {}\n    chat id: {}\n    date: {}\n    sender: {}:{}\n"
            "    outgoing: {}\n    topic: {}\n    type: {}\n    text: {}\n",
            message.at("id").get<std::int64_t>(), message.at("chat_id").get<std::int64_t>(), date,
            message.at("sender").at("type").get<std::string>(),
            message.at("sender").at("id").get<std::int64_t>(),
            yes_no(message.at("is_outgoing").get<bool>()), topic,
            message.at("type").get<std::string>(), message.at("text").dump());
    }
    out += data.at("next").is_null()
               ? "next: null\n"
               : fmt::format("next: {}\n", data.at("next").get<std::string>());
    return out;
}

std::string render_resolve(const nlohmann::json& data) {
    const auto& chat = data.at("chat");
    std::string usernames;
    for (const auto& username : chat.at("usernames")) {
        if (!usernames.empty()) {
            usernames += ", ";
        }
        usernames += username.get<std::string>();
    }
    const std::string message_id =
        data.at("message_id").is_null() ? "null" : data.at("message_id").dump();
    std::string topic = "null";
    if (!data.at("topic").is_null()) {
        topic = fmt::format("{}:{}", data.at("topic").at("kind").get<std::string>(),
                            data.at("topic").at("id").get<std::int64_t>());
    }
    const std::string link_type =
        data.at("link_type").is_null() ? "null" : data.at("link_type").get<std::string>();
    const std::string is_public =
        data.at("is_public").is_null() ? "null" : yes_no(data.at("is_public").get<bool>());
    return fmt::format(
        "kind: {}\nchat:\n  id: {}\n  title: {}\n  type: {}\n  bot: {}\n  usernames: {}\n"
        "message id: {}\ntopic: {}\nlink type: {}\npublic: {}\n",
        data.at("kind").get<std::string>(), chat.at("id").get<std::int64_t>(),
        chat.at("title").dump(), chat.at("type").get<std::string>(),
        yes_no(chat.at("is_bot").get<bool>()), usernames, message_id, topic, link_type, is_public);
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
    if (command_key == "account remove") {
        return render_account_remove(data);
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
    if (command_key == "logout") {
        return render_logout(data);
    }
    if (command_key == "me") {
        return render_user(data);
    }
    if (command_key == "saved tags") {
        return render_saved_tags(data);
    }
    if (command_key == "saved search") {
        return render_saved_search(data);
    }
    if (command_key == "session list") {
        return render_session_list(data);
    }
    if (command_key == "session terminate") {
        return render_session_terminate(data);
    }
    if (command_key == "chats") {
        return render_chats(data);
    }
    if (command_key == "resolve") {
        return render_resolve(data);
    }
    // Until a command grows a dedicated renderer, readable JSON is the
    // honest fallback.
    return data.dump(2) + "\n";
}

} // namespace tgcli::cli
