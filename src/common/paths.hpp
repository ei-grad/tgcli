#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

namespace tgcli::paths {

// Account names end up in socket paths and directory names; the charset is
// restricted accordingly and the length keeps the socket path within
// sun_path even for the /tmp fallback (DESIGN.md §9).
inline constexpr std::size_t kMaxAccountNameLength = 32;

bool valid_account_name(const std::string& name);

struct Environment {
    std::optional<std::string> xdg_runtime_dir;
    std::optional<std::string> xdg_config_home;
    std::optional<std::string> xdg_data_home;
    std::optional<std::string> xdg_state_home;
    std::optional<std::string> tmpdir;
    std::string home;
    uid_t uid = 0;
};

struct SocketIdentity {
    dev_t device = 0;
    ino_t inode = 0;
    std::int64_t change_seconds = 0;
    long change_nanoseconds = 0;

    friend bool operator==(const SocketIdentity&, const SocketIdentity&) = default;
};

// Snapshot of the real process environment.
Environment real_environment();

// Directory that holds the account's daemon socket:
// $XDG_RUNTIME_DIR/tgcli, falling back to $TMPDIR/tgcli-<uid> then
// /tmp/tgcli-<uid> (DESIGN.md §9). Pure — no filesystem access.
std::string runtime_dir(const Environment& env);

// Full socket path for an account. Fails (with a reason) on an invalid
// account name or a path that does not fit sockaddr_un::sun_path.
std::optional<std::string> socket_path(const std::string& account, const Environment& env,
                                       std::string& error);

// Version-independent local control endpoint used only to request graceful
// shutdown when the main protocol versions cannot communicate.
std::optional<std::string> control_socket_path(const std::string& account, const Environment& env,
                                               std::string& error);

// XDG layout from DESIGN.md §9. Pure.
std::string config_file(const Environment& env);
std::string account_data_dir(const std::string& account, const Environment& env);
std::string account_state_dir(const std::string& account, const Environment& env);

// Creates `dir` with mode 0700 if missing, then verifies it is a directory
// owned by `uid` with no group/other access — refuses sockets in a
// tamperable location. Returns false with a reason otherwise.
bool ensure_private_dir(const std::string& dir, uid_t uid, std::string& error);

// Read-only counterpart for paths that must already exist.
bool validate_private_dir(const std::string& dir, uid_t uid, std::string& error);

// Validates an existing filesystem unix socket as a private endpoint and
// returns the identity used to detect replacement races.
std::optional<SocketIdentity> inspect_socket_endpoint(const std::string& socket_path, uid_t uid,
                                                      std::string& error);

// Strictly validates an endpoint when present; absence is represented by an
// empty `identity`, while unsafe filesystem objects remain errors.
bool find_socket_endpoint(const std::string& socket_path, uid_t uid,
                          std::optional<SocketIdentity>& identity, std::string& error);

// Validates the private parent directory and removes an existing endpoint
// only when it is a private socket owned by `uid`. Call immediately before
// bind while holding the account lock.
bool prepare_socket_endpoint(const std::string& socket_path, uid_t uid, std::string& error);

// Reports whether the endpoint captured in `identity` disappeared or was
// replaced. Unexpected file types, ownership, or permissions fail closed.
bool socket_endpoint_changed(const std::string& socket_path, uid_t uid,
                             const SocketIdentity& identity, bool& changed, std::string& error);

// Best-effort shutdown cleanup that never removes a replacement endpoint.
void unlink_socket_endpoint_if_same(const std::string& socket_path, const SocketIdentity& identity);

} // namespace tgcli::paths
