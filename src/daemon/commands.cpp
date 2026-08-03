#include "daemon/commands.hpp"

#include "common/exit_codes.hpp"
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
            // Real auth reporting arrives with the M1 auth FSM.
            {"auth", json{{"state", "unknown"}}}};
}

} // namespace

void register_commands(Dispatcher& dispatcher, const DaemonContext& context) {
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
