#include "daemon/daemon_run.hpp"

#include "common/paths.hpp"
#include "core/td_client.hpp"
#include "daemon/commands.hpp"
#include "daemon/context.hpp"
#include "daemon/server.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <tgcli/version.hpp>
#include <unistd.h>

namespace tgcli::daemon {

namespace {

// Exclusive per-account lock: one process owns the tdlib database, whether
// a daemon or a --no-daemon invocation (DESIGN.md §10). The fd stays open
// (and the lock held) for the owner's lifetime.
int acquire_account_lock(const std::string& state_dir, std::string& error) {
    const std::string lock_path = state_dir + "/daemon.lock";
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot open " + lock_path + ": " + std::strerror(errno);
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        error = "another tgcli process holds the account lock (" + lock_path + ")";
        ::close(fd);
        return -1;
    }
    const std::string pid = std::to_string(getpid()) + "\n";
    if (::ftruncate(fd, 0) != 0 || ::write(fd, pid.data(), pid.size()) < 0) {
        // The pid note is informational; the flock itself is the guarantee.
    }
    return fd;
}

// Minimal sd_notify(READY=1) without libsystemd; a no-op outside systemd.
void notify_systemd_ready() {
    const char* socket_path = std::getenv("NOTIFY_SOCKET");
    if (socket_path == nullptr || *socket_path == '\0') {
        return;
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
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
    std::string state_dir;
};

bool resolve_account_paths(const std::string& account, AccountPaths& out, std::string& error) {
    const auto env = paths::real_environment();
    const auto socket = paths::socket_path(account, env, error);
    if (!socket) {
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
    out.state_dir = state_dir;
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
    const int lock_fd = acquire_account_lock(account_paths.state_dir, error);
    if (lock_fd < 0) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    // Handled synchronously via sigtimedwait below; blocked before any
    // thread (tdlib's included) is created so none of them steals the
    // signal. SIGPIPE would otherwise kill the daemon on a vanished client.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    core::TdClient td;

    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.socket_path = account_paths.socket;

    Dispatcher dispatcher;
    register_commands(dispatcher, context);

    Server server({account_paths.socket, kVersion, proto::kProtocolVersion}, dispatcher);
    context.request_shutdown = [&server] { server.request_stop(); };

    if (!server.start(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        ::close(lock_fd);
        return 1;
    }
    notify_systemd_ready();

    const timespec poll_interval{0, 100'000'000}; // 100ms
    while (!server.stop_requested()) {
        const int sig = ::sigtimedwait(&signals, nullptr, &poll_interval);
        if (sig == SIGTERM || sig == SIGINT) {
            break;
        }
    }

    server.stop();
    td.close();
    ::close(lock_fd);
    return 0;
}

bool run_no_daemon(const proto::Request& request, ResponseSink& sink, const std::string& account,
                   std::string& error) {
    AccountPaths account_paths;
    if (!resolve_account_paths(account, account_paths, error)) {
        return false;
    }
    const int lock_fd = acquire_account_lock(account_paths.state_dir, error);
    if (lock_fd < 0) {
        return false;
    }

    core::TdClient td;

    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.in_process = true;

    Dispatcher dispatcher;
    register_commands(dispatcher, context);
    dispatcher.dispatch(request, sink);

    td.close();
    ::close(lock_fd);
    return true;
}

} // namespace tgcli::daemon
