#include "daemon/daemon_run.hpp"

#include "common/daemon_lock.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "core/td_client.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/login_commands.hpp"
#include "daemon/server.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <tgcli/version.hpp>
#include <thread>
#include <unistd.h>

namespace tgcli::daemon {

namespace {

// Minimal sd_notify(READY=1) without libsystemd; a no-op outside systemd.
void notify_systemd_ready() {
    const char* socket_path = std::getenv("NOTIFY_SOCKET");
    if (socket_path == nullptr || *socket_path == '\0') {
        return;
    }
    const int fd = net::socket_cloexec(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0'; // abstract namespace
    }
    constexpr std::string_view ready = "READY=1";
    ::sendto(fd, ready.data(), ready.size(), 0, reinterpret_cast<const sockaddr*>(&addr),
             sizeof(addr));
    ::close(fd);
}

struct AccountPaths {
    std::string socket;
    std::string control_socket;
    std::string state_dir;
    std::string lock_file;
};

bool resolve_account_paths(const std::string& account, AccountPaths& out, std::string& error) {
    const auto env = paths::real_environment();
    const auto socket = paths::socket_path(account, env, error);
    if (!socket) {
        return false;
    }
    const auto control_socket = paths::control_socket_path(account, env, error);
    if (!control_socket) {
        return false;
    }
    if (!paths::ensure_private_dir(paths::runtime_dir(env), env.uid, error)) {
        return false;
    }
    // Parents of the account state dir (…/tgcli, …/tgcli/accounts) are not
    // secret-bearing; 0700 all the way down is still the simplest policy.
    const auto state_dir = paths::account_state_dir(account, env);
    std::string partial;
    for (std::size_t pos = 1; pos != std::string::npos;) {
        pos = state_dir.find('/', pos + 1);
        partial = state_dir.substr(0, pos);
        if (!paths::ensure_private_dir(partial, env.uid, error)) {
            // Pre-existing parents like ~/.local may legitimately be 0755;
            // only the tgcli subtree must be private.
            if (partial.find("/tgcli") == std::string::npos) {
                error.clear();
                continue;
            }
            return false;
        }
    }
    out.socket = *socket;
    out.control_socket = *control_socket;
    out.state_dir = state_dir;
    out.lock_file = state_dir + "/daemon.lock";
    return true;
}

} // namespace

int run_daemon(const std::string& account) {
    AccountPaths account_paths;
    std::string error;
    if (!resolve_account_paths(account, account_paths, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    daemon_lock::Identity lock_identity;
    const int lock_fd = daemon_lock::acquire(account_paths.lock_file, lock_identity, error);
    if (lock_fd < 0) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    // Consumed by a dedicated sigwait watcher below; blocked before any
    // thread (tdlib's included) is created so none of them steals the
    // signal. SIGPIPE would otherwise kill the daemon on a vanished client.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    const auto environment = paths::real_environment();
    core::TdClient td(core::TdLogConfiguration{
        .file_path = paths::tdlib_log_file(account, environment),
    });
    const config::Store config_store(paths::config_file(environment), environment.uid);

    const auto environment_value = [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' ? std::optional<std::string>{value}
                                                  : std::nullopt;
    };
    LoginCoordinator login(td, config_store, environment, account, kVersion,
                           environment_value("TGCLI_API_ID"), environment_value("TGCLI_API_HASH"));

    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.socket_path = account_paths.socket;
    context.login = &login;
    context.auth_state = [&td] {
        const auto state = td.auth_state();
        return state ? std::string(core::auth_state_name(state->data.state)) : "unknown";
    };

    Dispatcher dispatcher;
    register_commands(dispatcher, context);

    Server server({account, account_paths.socket, kVersion, proto::kProtocolVersion,
                   account_paths.control_socket, lock_identity.control_token},
                  dispatcher);
    context.request_shutdown = [&server] { server.request_stop(); };

    if (!server.start(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        ::close(lock_fd);
        return 1;
    }
    notify_systemd_ready();

    // sigwait (portable, unlike Linux-only sigtimedwait) parks a dedicated
    // thread on the blocked signals with no polling. SIGTERM/SIGINT request a
    // graceful stop; the `daemon stop` command sets the same flag directly.
    // The watcher exits once stop is requested; after the owning thread
    // observes stop it wakes a still-parked watcher with SIGPIPE — blocked, so
    // delivered to sigwait rather than a handler, and ignored by the watcher —
    // so it can be joined before the Server and TdClient are torn down.
    std::thread signal_watcher([&server, &signals] {
        while (!server.stop_requested()) {
            int sig = 0;
            if (sigwait(&signals, &sig) == 0 && (sig == SIGTERM || sig == SIGINT)) {
                server.request_stop();
            }
        }
    });

    server.wait_for_stop();
    ::pthread_kill(signal_watcher.native_handle(), SIGPIPE);
    signal_watcher.join();

    server.stop();
    td.close();
    ::close(lock_fd);
    return 0;
}

bool run_no_daemon(const proto::Request& request, ResponseSink& sink, const std::string& account,
                   std::string& error, const Dispatcher* dispatcher_override) {
    AccountPaths account_paths;
    if (!resolve_account_paths(account, account_paths, error)) {
        return false;
    }
    daemon_lock::Identity lock_identity;
    const int lock_fd = daemon_lock::acquire(account_paths.lock_file, lock_identity, error);
    if (lock_fd < 0) {
        return false;
    }

    const auto environment = paths::real_environment();
    core::TdClient td(core::TdLogConfiguration{
        .file_path = paths::tdlib_log_file(account, environment),
        .json_diagnostics = request.context.json,
    });
    const config::Store config_store(paths::config_file(environment), environment.uid);
    const auto environment_value = [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' ? std::optional<std::string>{value}
                                                  : std::nullopt;
    };
    LoginCoordinator login(td, config_store, environment, account, kVersion,
                           environment_value("TGCLI_API_ID"), environment_value("TGCLI_API_HASH"));
    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.in_process = true;
    context.login = &login;
    context.auth_state = [&td] {
        const auto state = td.auth_state();
        return state ? std::string(core::auth_state_name(state->data.state)) : "unknown";
    };

    Dispatcher dispatcher;
    register_commands(dispatcher, context);
    (dispatcher_override != nullptr ? *dispatcher_override : dispatcher).dispatch(request, sink);

    td.close();
    ::close(lock_fd);
    return true;
}

} // namespace tgcli::daemon
