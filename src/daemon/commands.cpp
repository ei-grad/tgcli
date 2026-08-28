#include "daemon/commands.hpp"

#include "common/exit_codes.hpp"
#include "daemon/account_removal.hpp"
#include "daemon/chats_commands.hpp"
#include "daemon/download_commands.hpp"
#include "daemon/fetch_commands.hpp"
#include "daemon/login_commands.hpp"
#include "daemon/logout_commands.hpp"
#include "daemon/m2_read_commands.hpp"
#include "daemon/m6_commands.hpp"
#include "daemon/message_commands.hpp"
#include "daemon/read_commands.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "daemon/saved_commands.hpp"
#include "daemon/stream_coordinator.hpp"
#include "daemon/write_commands.hpp"

#include <array>
#include <cstdint>
#include <tgcli/version.hpp>
#include <unistd.h>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

json version_payload(const DaemonContext& context) {
    json payload{{"version", context.binary_version},
                 {"protocol", context.protocol_version},
                 {"tdlib", context.tdlib_version}};
    if (kBuildCommit[0] != '\0') {
        payload["commit"] = kBuildCommit;
    }
    return payload;
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

bool uses_account_removal_preflight(std::string_view command) {
    return command == "login" || command == "logout" || command == "me" || command == "doctor" ||
           command == "saved tags" || command == "saved search" || command == "resolve" ||
           command == "chats" || command == "search" || command == "chat info" ||
           command == "chat members" || command == "download" || command == "msg get" ||
           command == "msg link" || command == "fetch" || command == "send" ||
           command == "msg edit" || command == "listen" || command == "wait-for" ||
           command == "msg delete" || command == "msg forward" || command == "msg react" ||
           command == "msg pin" || command == "msg unpin" || command == "chat mark-read" ||
           command == "chat mute" || command == "chat unmute" || command == "chat pin" ||
           command == "chat unpin" || command == "chat archive" || command == "chat unarchive" ||
           command == "chat join" || command == "chat leave" || command == "daemon status" ||
           command == "daemon stop" || command == "daemon restart";
}

bool uses_logout_preflight(std::string_view command) {
    return command == "login" || command == "logout" || command == "me" || command == "doctor" ||
           command == "saved tags" || command == "saved search" || command == "resolve" ||
           command == "chats" || command == "search" || command == "chat info" ||
           command == "chat members" || command == "download" || command == "msg get" ||
           command == "msg link" || command == "fetch" || command == "send" ||
           command == "msg edit" || command == "listen" || command == "wait-for" ||
           command == "msg delete" || command == "msg forward" || command == "msg react" ||
           command == "msg pin" || command == "msg unpin" || command == "chat mark-read" ||
           command == "chat mute" || command == "chat unmute" || command == "chat pin" ||
           command == "chat unpin" || command == "chat archive" || command == "chat unarchive" ||
           command == "chat join" || command == "chat leave";
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): fixed recovery-order matrix.
void configure_request_preflight(Dispatcher& dispatcher, const DaemonContext& context) {
    if (context.logout == nullptr && context.account_removal == nullptr) {
        return;
    }
    dispatcher.set_request_preflight([&context](const std::string& command,
                                                RequestSession& session) {
        if (command == "account remove") {
            return true;
        }
        for (const auto preflight : recovery_preflight_order(command)) {
            if (preflight == RecoveryPreflight::Removal && context.account_removal != nullptr &&
                !context.account_removal->preflight(context.account, session)) {
                return false;
            }
            if (preflight == RecoveryPreflight::Logout) {
                if (command == "logout" && session.request().context.dry_run) {
                    continue;
                }
                const bool persistence_free_m3 =
                    session.request().context.dry_run &&
                    (command == "send" || command == "msg edit" || command == "msg delete" ||
                     command == "msg forward" || command == "msg react" || command == "msg pin" ||
                     command == "msg unpin" || command == "chat mark-read" ||
                     command == "chat mute" || command == "chat unmute" || command == "chat pin" ||
                     command == "chat unpin" || command == "chat archive" ||
                     command == "chat unarchive" || command == "chat join" ||
                     command == "chat leave");
                if (context.logout != nullptr &&
                    !(persistence_free_m3 ? context.logout->preflight_read_only(session)
                                          : context.logout->preflight(session))) {
                    return false;
                }
            }
        }
        return true;
    });
}

} // namespace

std::span<const RecoveryPreflight> recovery_preflight_order(std::string_view command) {
    static constexpr std::array both{RecoveryPreflight::Removal, RecoveryPreflight::Logout};
    static constexpr std::array removal{RecoveryPreflight::Removal};
    if (uses_logout_preflight(command)) {
        return both;
    }
    if (uses_account_removal_preflight(command)) {
        return removal;
    }
    return {};
}

void register_commands(Dispatcher& dispatcher, const DaemonContext& context) {
    if (context.login != nullptr) {
        register_login_commands(dispatcher, *context.login);
    }
    if (context.logout != nullptr) {
        register_logout_command(dispatcher, *context.logout);
    }
    if (context.account_removal != nullptr) {
        register_account_removal_command(dispatcher, *context.account_removal);
    }
    if (context.saved != nullptr) {
        register_saved_commands(dispatcher, *context.saved);
    }
    if (context.chats != nullptr) {
        register_chats_command(dispatcher, *context.chats);
        register_unread_command(dispatcher, *context.chats);
    }
    if (context.messages != nullptr) {
        register_message_commands(dispatcher, *context.messages);
    }
    if (context.read != nullptr) {
        register_read_command(dispatcher, *context.read);
    }
    if (context.m2_read != nullptr) {
        register_search_command(dispatcher, *context.m2_read);
        register_chat_info_command(dispatcher, *context.m2_read);
        register_chat_members_command(dispatcher, *context.m2_read);
    }
    if (context.download != nullptr) {
        register_download_command(dispatcher, *context.download);
    }
    if (context.fetch != nullptr) {
        register_fetch_command(dispatcher, *context.fetch);
    }
    if (context.resolver != nullptr) {
        register_resolve_command(dispatcher, *context.resolver);
    }
    if (context.writes != nullptr) {
        register_write_commands(dispatcher, *context.writes);
    }
    if (context.m6 != nullptr && context.writes != nullptr) {
        register_m6_commands(dispatcher, *context.m6, *context.writes);
    }
    if (context.streams != nullptr) {
        register_stream_commands(dispatcher, *context.streams);
    }
    configure_request_preflight(dispatcher, context);
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
