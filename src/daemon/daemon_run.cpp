#include "daemon/daemon_run.hpp"

#include "common/daemon_lock.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "core/td_client.hpp"
#include "daemon/commands.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/context.hpp"
#include "daemon/login_commands.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/logout_commands.hpp"
#include "daemon/request_session.hpp"
#include "daemon/server.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
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
    ConfigRuntime config_runtime(config_store.path(), {}, environment.uid);

    const auto environment_value = [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' ? std::optional<std::string>{value}
                                                  : std::nullopt;
    };
    LoginCoordinator login(td, config_store, environment, account, kVersion,
                           environment_value("TGCLI_API_ID"), environment_value("TGCLI_API_HASH"));
    Server* server_pointer = nullptr;
    LogoutCoordinator logout(td, config_runtime, environment, account, config_store.path(),
                             [&server_pointer] {
                                 if (server_pointer != nullptr) {
                                     server_pointer->request_stop();
                                 }
                             });

    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.socket_path = account_paths.socket;
    context.login = &login;
    context.logout = &logout;
    context.auth_state = [&td] {
        const auto state = td.auth_state();
        return state ? std::string(core::auth_state_name(state->data.state)) : "unknown";
    };

    Dispatcher dispatcher;
    register_commands(dispatcher, context);

    Server server({account, account_paths.socket, kVersion, proto::kProtocolVersion,
                   account_paths.control_socket, lock_identity.control_token},
                  dispatcher);
    server_pointer = &server;
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
    ConfigRuntime config_runtime(config_store.path(), {}, environment.uid);
    const auto environment_value = [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' ? std::optional<std::string>{value}
                                                  : std::nullopt;
    };
    LoginCoordinator login(td, config_store, environment, account, kVersion,
                           environment_value("TGCLI_API_ID"), environment_value("TGCLI_API_HASH"));
    LogoutCoordinator logout(td, config_runtime, environment, account, config_store.path());
    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.in_process = true;
    context.login = &login;
    context.logout = &logout;
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

bool reconcile_logout_audit_offline(const std::string& account,
                                    std::chrono::steady_clock::time_point deadline) {
    const auto environment = paths::real_environment();
    const auto state_directory = paths::account_state_dir(account, environment);
    std::string error;
    if (!paths::validate_private_dir(state_directory, environment.uid, error)) {
        return false;
    }
    daemon_lock::Identity lock_identity;
    const int lock_fd =
        daemon_lock::acquire(state_directory + "/daemon.lock", lock_identity, error);
    if (lock_fd < 0) {
        return false;
    }

    const LogoutAuditLog audit(state_directory, account, environment.uid);
    auto definite = reconcile_definite_logout_audit(audit, [] {
        const auto now = std::chrono::system_clock::now();
        const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &seconds);
#else
        gmtime_r(&seconds, &utc);
#endif
        std::array<char, 21> rendered{};
        if (std::strftime(rendered.data(), rendered.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
            return std::string{};
        }
        return std::string(rendered.data());
    });
    if (definite.status != LogoutAuditReconcileStatus::ObservationRequired) {
        ::close(lock_fd);
        return definite.status == LogoutAuditReconcileStatus::Clean;
    }

    bool reconciled = false;
    try {
        core::TdClient td(core::TdLogConfiguration{
            .file_path = paths::tdlib_log_file(account, environment),
        });
        const config::Store store(paths::config_file(environment), environment.uid);
        ConfigRuntime config_runtime(store.path(), {}, environment.uid);
        LogoutCoordinator logout(td, config_runtime, environment, account, store.path());
        CallbackSink sink(
            [](const nlohmann::json&) {}, [](const nlohmann::json&) {},
            [](const nlohmann::json&) {},
            [](const std::string&, const std::string&, const nlohmann::json&, int) {});
        proto::Request request(account);
        request.id = 1;
        request.command = {"doctor"};
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining > std::chrono::steady_clock::duration::zero()) {
            request.context.timeout_seconds = std::chrono::duration<double>(remaining).count();
            RequestSession session(std::move(request), sink);
            reconciled = logout.preflight(session);
        }
        td.close();
    } catch (const std::exception&) {
        reconciled = false;
    }
    ::close(lock_fd);
    return reconciled;
}

} // namespace tgcli::daemon
