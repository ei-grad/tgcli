#include "daemon/commands.hpp"

#include "common/exit_codes.hpp"
#include "daemon/login_commands.hpp"
#include "daemon/logout_commands.hpp"
#include "daemon/request_session.hpp"

#include <cstdint>
#include <unistd.h>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

json version_payload(const DaemonContext& context) {
    return {{"version", context.binary_version},
            {"protocol", context.protocol_version},
            {"tdlib", context.tdlib_version}};
}

json doctor_payload(const DaemonContext& context) {
    json daemon_info{{"running", !context.in_process},
                     {"in_process", context.in_process},
                     {"pid", static_cast<std::int64_t>(getpid())},
                     {"version", context.binary_version}};
    if (!context.socket_path.empty()) {
        daemon_info["socket"] = context.socket_path;
    }
    return {{"account", context.account},
            {"daemon", std::move(daemon_info)},
            {"tdlib", json{{"version", context.tdlib_version}}},
            {"auth", json{{"state", context.auth_state()}}}};
}

} // namespace

void register_commands(Dispatcher& dispatcher, const DaemonContext& context) {
    if (context.login != nullptr) {
        register_login_commands(dispatcher, *context.login);
    }
    if (context.logout != nullptr) {
        register_logout_command(dispatcher, *context.logout);
        dispatcher.set_request_preflight(
            [&context](const std::string& command, RequestSession& session) {
                if (command == "logout" && session.request().context.dry_run) {
                    return true;
                }
                if (command == "login" || command == "logout" || command == "me" ||
                    command == "doctor") {
                    return context.logout->preflight(session);
                }
                return true;
            });
    }
    dispatcher.register_command(
        "version", {Tier::Read, [&context](const proto::Request&, RequestSession& sink) {
                        sink.result(version_payload(context));
                    }});
    dispatcher.register_command(
        "doctor", {Tier::Read, [&context](const proto::Request&, RequestSession& sink) {
                       sink.result(doctor_payload(context));
                   }});
    dispatcher.register_command(
        "daemon stop", {Tier::Read, [&context](const proto::Request&, RequestSession& sink) {
                            if (context.in_process) {
                                sink.error("USAGE", "no daemon to stop in --no-daemon mode",
                                           nlohmann::json::object(), kUsage);
                                return;
                            }
                            sink.result({{"stopping", true}});
                            context.request_shutdown();
                        }});
}

} // namespace tgcli::daemon
