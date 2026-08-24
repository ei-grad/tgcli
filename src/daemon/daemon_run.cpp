#include "daemon/daemon_run.hpp"

#include "common/daemon_lock.hpp"
#include "common/exit_codes.hpp"
#include "common/net_compat.hpp"
#include "common/paths.hpp"
#include "core/td_client.hpp"
#include "daemon/account_removal.hpp"
#include "daemon/account_removal_remote.hpp"
#include "daemon/chats_commands.hpp"
#include "daemon/commands.hpp"
#include "daemon/config_runtime.hpp"
#include "daemon/context.hpp"
#include "daemon/fetch_commands.hpp"
#include "daemon/idempotency_reconciliation.hpp"
#include "daemon/login_commands.hpp"
#include "daemon/logout_audit.hpp"
#include "daemon/logout_commands.hpp"
#include "daemon/message_commands.hpp"
#include "daemon/read_commands.hpp"
#include "daemon/removal_journal.hpp"
#include "daemon/removal_recovery.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "daemon/saved_commands.hpp"
#include "daemon/server.hpp"
#include "daemon/stream_service.hpp"
#include "daemon/write_commands.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
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

class ScopedDescriptor final {
  public:
    explicit ScopedDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
    ~ScopedDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
    ScopedDescriptor(ScopedDescriptor&&) = delete;
    ScopedDescriptor& operator=(ScopedDescriptor&&) = delete;

    [[nodiscard]] explicit operator bool() const {
        return descriptor_ >= 0;
    }

  private:
    int descriptor_;
};

class LocalRemovalRemote final : public AccountRemovalRemote {
  public:
    RemovalRemoteProof
    prove_remote_logout(const proto::AccountRemovePlan& /*plan*/,
                        const std::shared_ptr<const config::ConfigSnapshot>& /*config_snapshot*/,
                        bool /*send_checkpointed*/, RequestSession& /*session*/,
                        const RemovalCheckpoint& /*checkpoint*/) override {
        return RemovalOperationError{
            "INTERNAL",
            "local removal cannot inspect a remote Telegram session",
            {{"operation", "account_remove"}, {"reason", "internal_error"}},
            kGeneric};
    }

    std::optional<RemovalOperationError> quiesce(RequestSession& /*session*/) override {
        return std::nullopt;
    }
};

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

bool ensure_private_tree(const std::string& directory, const paths::Environment& environment,
                         std::string& error) {
    std::string partial;
    for (std::size_t pos = 1; pos != std::string::npos;) {
        pos = directory.find('/', pos + 1);
        partial = directory.substr(0, pos);
        if (!paths::ensure_private_dir(partial, environment.uid, error)) {
            if (partial.find("/tgcli") == std::string::npos) {
                error.clear();
                continue;
            }
            return false;
        }
    }
    return true;
}

int acquire_removal_gate(const std::string& account, const paths::Environment& environment,
                         daemon_lock::Identity& identity, std::string& error) {
    const auto directory = paths::removals_state_dir(environment);
    if (!ensure_private_tree(directory, environment, error)) {
        return -1;
    }
    return daemon_lock::acquire(directory + "/." + account + ".lock", identity, error);
}

bool daemon_start_preserves_removal_recovery(const std::string& account,
                                             const paths::Environment& environment,
                                             std::string& error) {
    const RemovalJournal journal(paths::removals_state_dir(environment), environment.uid);
    const auto inspection = journal.inspect_account(account);
    if (inspection.status == RemovalInspectionStatus::Invalid) {
        error = "cannot inspect pending account removal: " + (inspection.failure.reason.empty()
                                                                  ? std::string("path_invalid")
                                                                  : inspection.failure.reason);
        return false;
    }
    if (inspection.status == RemovalInspectionStatus::Incomplete && inspection.tombstone &&
        can_resume_removal_without_tdlib(*inspection.tombstone)) {
        error = "pending account removal must resume without starting TDLib";
        return false;
    }
    return true;
}

bool resolve_account_paths(const std::string& account, const paths::Environment& environment,
                           AccountPaths& out, std::string& error) {
    const auto socket = paths::socket_path(account, environment, error);
    if (!socket) {
        return false;
    }
    const auto control_socket = paths::control_socket_path(account, environment, error);
    if (!control_socket) {
        return false;
    }
    if (!paths::ensure_private_dir(paths::runtime_dir(environment), environment.uid, error)) {
        return false;
    }
    // Parents of the account state dir (…/tgcli, …/tgcli/accounts) are not
    // secret-bearing; 0700 all the way down is still the simplest policy.
    const auto state_dir = paths::account_state_dir(account, environment);
    if (!ensure_private_tree(state_dir, environment, error)) {
        return false;
    }
    out.socket = *socket;
    out.control_socket = *control_socket;
    out.state_dir = state_dir;
    out.lock_file = state_dir + "/daemon.lock";
    return true;
}

} // namespace

int run_daemon(const std::string& account) {
    const auto environment = paths::real_environment();
    daemon_lock::Identity removal_gate_identity;
    std::string error;
    const int removal_gate_fd =
        acquire_removal_gate(account, environment, removal_gate_identity, error);
    if (removal_gate_fd < 0) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    if (!daemon_start_preserves_removal_recovery(account, environment, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        ::close(removal_gate_fd);
        return 1;
    }
    AccountPaths account_paths;
    if (!resolve_account_paths(account, environment, account_paths, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        ::close(removal_gate_fd);
        return 1;
    }
    daemon_lock::Identity lock_identity;
    auto lifetime_lease =
        daemon_lock::acquire_lifetime(account_paths.lock_file, lock_identity, error);
    if (!lifetime_lease) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        ::close(removal_gate_fd);
        return 1;
    }
    auto foundation_result = IdempotencyFoundation::create(account_paths.state_dir, account,
                                                           environment.uid, lifetime_lease);
    if (auto* failure = std::get_if<IdempotencyFailure>(&foundation_result)) {
        std::fprintf(stderr, "error: cannot initialize account durability foundation: %s\n",
                     failure->detail.c_str());
        ::close(removal_gate_fd);
        return 1;
    }
    auto idempotency = std::make_shared<IdempotencyFoundation>(
        std::get<IdempotencyFoundation>(std::move(foundation_result)));

    // Consumed by a dedicated sigwait watcher below; blocked before any
    // thread (tdlib's included) is created so none of them steals the
    // signal. SIGPIPE would otherwise kill the daemon on a vanished client.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    StreamService stream_service;
    core::TdClient td(
        core::TdLogConfiguration{.file_path = paths::tdlib_log_file(account, environment)},
        stream_service.observer_factory());
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
    const auto stop_server = [&server_pointer] {
        if (server_pointer != nullptr) {
            server_pointer->request_stop();
        }
    };
    LogoutCoordinator logout(td, config_runtime, environment, account, config_store.path(),
                             stop_server);
    RemovalJournal removal_journal(paths::removals_state_dir(environment), environment.uid);
    TdAccountRemovalRemote removal_remote(td, config_store, environment, account, kVersion,
                                          environment_value("TGCLI_API_ID"),
                                          environment_value("TGCLI_API_HASH"));
    AccountRemovalCoordinator account_removal(config_store, removal_journal, environment, account,
                                              removal_remote, stop_server, {}, stop_server);
    SavedCoordinator saved(td, account);
    ChatsCoordinator chats(td, account);
    MessageCoordinator messages(td, account);
    ReadCoordinator read(td, account);
    FetchCoordinator fetch(td, account);
    ResolveCoordinator resolver(td, account);
    WriteCoordinator writes(td, account, config_store.path(), environment.uid, idempotency,
                            stop_server);

    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.socket_path = account_paths.socket;
    context.login = &login;
    context.logout = &logout;
    context.account_removal = &account_removal;
    context.saved = &saved;
    context.chats = &chats;
    context.messages = &messages;
    context.read = &read;
    context.fetch = &fetch;
    context.resolver = &resolver;
    context.writes = &writes;
    context.idempotency = idempotency;
    context.auth_state = [&td] {
        const auto state = td.auth_state();
        return state ? std::string(core::auth_state_name(state->data.state)) : "unknown";
    };

    Dispatcher dispatcher;
    register_commands(dispatcher, context);

    Server server({account,
                   account_paths.socket,
                   kVersion,
                   proto::kProtocolVersion,
                   account_paths.control_socket,
                   lock_identity.control_token,
                   {},
                   {},
                   {},
                   &config_runtime},
                  dispatcher);
    server_pointer = &server;
    context.request_shutdown = [&server] { server.request_stop(); };

    if (!server.start(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        ::close(removal_gate_fd);
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
    ::close(removal_gate_fd);
    return 0;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool run_no_daemon(const proto::Request& request, ResponseSink& sink, const std::string& account,
                   std::string& error, const Dispatcher* dispatcher_override,
                   core::TdClient* td_client_override,
                   const testing::RequestWallClock& request_wall_clock) {
    auto admitted_request = proto::admit_request_source(request, error);
    if (!admitted_request) {
        return false;
    }
    const auto& admitted = *admitted_request;
    const auto admitted_at = RequestClock::now();
    const auto admission_wall_time =
        request_wall_clock ? request_wall_clock() : RequestSession::WallClock::now();
    std::optional<RequestDeadline> admitted_deadline;
    if (admitted.context.timeout_seconds) {
        admitted_deadline = request_deadline(admitted.context.timeout_seconds,
                                             DeadlineDefault::Default60, admitted_at);
        if (!admitted_deadline) {
            sink.error("USAGE", "invalid request timeout",
                       {{"argument", "--timeout"}, {"reason", "invalid_argument"}}, kUsage);
            return true;
        }
    }
    const auto environment = paths::real_environment();
    if (admitted.command == std::vector<std::string>{"account", "remove"} &&
        admitted.args.is_object()) {
        const RemovalJournal journal(paths::removals_state_dir(environment), environment.uid);
        const auto inspection = journal.inspect_account(account);
        const bool local_recovery = inspection.status == RemovalInspectionStatus::Incomplete &&
                                    inspection.tombstone &&
                                    can_resume_removal_without_tdlib(*inspection.tombstone);
        if (admitted.context.dry_run || admitted.args.value("keep_session", false) ||
            local_recovery) {
            return run_account_removal_local(admitted, sink, account, error);
        }
    }
    daemon_lock::Identity removal_gate_identity;
    const int removal_gate_fd =
        acquire_removal_gate(account, environment, removal_gate_identity, error);
    if (removal_gate_fd < 0) {
        return false;
    }
    AccountPaths account_paths;
    if (!resolve_account_paths(account, environment, account_paths, error)) {
        ::close(removal_gate_fd);
        return false;
    }
    daemon_lock::Identity lock_identity;
    auto lifetime_lease =
        daemon_lock::acquire_lifetime(account_paths.lock_file, lock_identity, error);
    if (!lifetime_lease) {
        ::close(removal_gate_fd);
        return false;
    }
    auto foundation_result = IdempotencyFoundation::create(account_paths.state_dir, account,
                                                           environment.uid, lifetime_lease);
    if (auto* failure = std::get_if<IdempotencyFailure>(&foundation_result)) {
        error = "cannot initialize account durability foundation: " + failure->detail;
        ::close(removal_gate_fd);
        return false;
    }
    auto idempotency = std::make_shared<IdempotencyFoundation>(
        std::get<IdempotencyFoundation>(std::move(foundation_result)));

    std::unique_ptr<core::TdClient> owned_td;
    if (td_client_override == nullptr) {
        owned_td = std::make_unique<core::TdClient>(core::TdLogConfiguration{
            .file_path = paths::tdlib_log_file(account, environment),
            .json_diagnostics = admitted.context.json,
        });
        td_client_override = owned_td.get();
    }
    core::TdClient& td = *td_client_override;
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
    RemovalJournal removal_journal(paths::removals_state_dir(environment), environment.uid);
    TdAccountRemovalRemote removal_remote(td, config_store, environment, account, kVersion,
                                          environment_value("TGCLI_API_ID"),
                                          environment_value("TGCLI_API_HASH"));
    AccountRemovalCoordinator account_removal(config_store, removal_journal, environment, account,
                                              removal_remote);
    SavedCoordinator saved(td, account);
    ChatsCoordinator chats(td, account);
    MessageCoordinator messages(td, account);
    ReadCoordinator read(td, account);
    FetchCoordinator fetch(td, account);
    ResolveCoordinator resolver(td, account);
    WriteCoordinator writes(td, account, config_store.path(), environment.uid, idempotency);
    DaemonContext context;
    context.account = account;
    context.binary_version = kVersion;
    context.protocol_version = proto::kProtocolVersion;
    context.tdlib_version = core::TdClient::tdlib_version();
    context.in_process = true;
    context.login = &login;
    context.logout = &logout;
    context.account_removal = &account_removal;
    context.saved = &saved;
    context.chats = &chats;
    context.messages = &messages;
    context.read = &read;
    context.fetch = &fetch;
    context.resolver = &resolver;
    context.writes = &writes;
    context.idempotency = idempotency;
    context.auth_state = [&td] {
        const auto state = td.auth_state();
        return state ? std::string(core::auth_state_name(state->data.state)) : "unknown";
    };

    Dispatcher dispatcher;
    register_commands(dispatcher, context);
    const auto& selected_dispatcher =
        dispatcher_override != nullptr ? *dispatcher_override : dispatcher;
    const auto deadline_policy = selected_dispatcher.deadline_default(admitted);
    if (!admitted_deadline) {
        admitted_deadline = request_deadline(std::nullopt, deadline_policy, admitted_at);
    }
    const auto& deadline = admitted_deadline;
    if (!deadline) {
        sink.error("USAGE", "invalid request timeout",
                   {{"argument", "--timeout"}, {"reason", "invalid_argument"}}, kUsage);
    } else if (deadline_policy == DeadlineDefault::Default60 &&
               !selected_dispatcher.requires_frozen_config_admission(admitted)) {
        RequestSession session(admitted, sink, 0, RequestSession::NonceGenerator{},
                               ActivityTracker::Token{}, nullptr, deadline,
                               ConfigAdmissionMode::DirectFallback, admission_wall_time);
        selected_dispatcher.dispatch(session);
    } else {
        const auto admission = config_runtime.admit(admitted.account, *deadline);
        if (admission.refresh_status == ConfigRefreshStatus::TimedOut) {
            const bool fetch = admitted.command == std::vector<std::string>{"fetch"} &&
                               deadline_policy == DeadlineDefault::Unlimited;
            sink.error("TIMEOUT", "config admission timed out",
                       {{"operation", fetch ? "fetch" : "config_admission"}, {"state", nullptr}},
                       kTimeout);
        } else if (admission.refresh_status != ConfigRefreshStatus::Completed ||
                   !admission.decision) {
            sink.error("DAEMON_SHUTDOWN", "daemon is shutting down",
                       {{"reason", "daemon_shutdown"}}, kGeneric);
        } else if (const auto* accepted = std::get_if<std::shared_ptr<const AdmittedAccountConfig>>(
                       &*admission.decision)) {
            RequestSession session(admitted, sink, 0, RequestSession::NonceGenerator{},
                                   ActivityTracker::Token{}, *accepted, deadline,
                                   ConfigAdmissionMode::FrozenRuntime, admission_wall_time);
            selected_dispatcher.dispatch(session);
        } else {
            const auto& denied = std::get<ConfigAdmissionDenied>(*admission.decision);
            if (denied.state == ConfigAdmissionState::AccountMissing) {
                sink.error("ACCOUNT_NOT_FOUND", "account is not configured",
                           {{"account", denied.account}}, kNotFound);
            } else {
                const auto reason = denied.reload_diagnostic
                                        ? config::reason_name(denied.reload_diagnostic->reason)
                                        : std::string_view{"io_error"};
                sink.error("CONFIG_INVALID", "cannot use current config.toml",
                           {{"path", config_runtime.config_path()}, {"reason", reason}}, kGeneric);
            }
        }
    }

    if (owned_td) {
        td.close();
    }
    ::close(removal_gate_fd);
    return true;
}

bool run_account_removal_local(const proto::Request& request, ResponseSink& sink,
                               const std::string& account, std::string& error) {
    const auto environment = paths::real_environment();
    RemovalJournal journal(paths::removals_state_dir(environment), environment.uid);
    const auto inspection = journal.inspect_account(account);
    const bool resumable_without_tdlib = inspection.status == RemovalInspectionStatus::Incomplete &&
                                         inspection.tombstone &&
                                         can_resume_removal_without_tdlib(*inspection.tombstone);
    if (request.command != std::vector<std::string>{"account", "remove"} ||
        !request.args.is_object() || !request.args.contains("keep_session") ||
        !request.args["keep_session"].is_boolean() ||
        (!request.context.dry_run && !request.args["keep_session"].get<bool>() &&
         !resumable_without_tdlib)) {
        error = "local removal requires dry-run, --keep-session, or a durable remote proof";
        return false;
    }
    daemon_lock::Identity removal_gate_identity;
    const ScopedDescriptor removal_gate(
        request.context.dry_run
            ? -1
            : acquire_removal_gate(account, environment, removal_gate_identity, error));
    if (!request.context.dry_run && !removal_gate) {
        return false;
    }
    const config::Store store(paths::config_file(environment), environment.uid);
    LocalRemovalRemote remote;
    AccountRemovalCoordinator coordinator(store, journal, environment, account, remote);
    Dispatcher dispatcher;
    register_account_removal_command(dispatcher, coordinator);
    dispatcher.dispatch(request, sink);
    return true;
}

bool reconcile_logout_audit_offline(const std::string& account, const RequestDeadline& deadline) {
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
        if (!deadline_expired(deadline)) {
            RequestSession session(std::move(request), sink, 0, RequestSession::NonceGenerator{},
                                   ActivityTracker::Token{}, nullptr, deadline);
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
