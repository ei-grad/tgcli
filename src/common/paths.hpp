#pragma once

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

// XDG layout from DESIGN.md §9. Pure.
std::string config_file(const Environment& env);
std::string account_data_dir(const std::string& account, const Environment& env);
std::string account_state_dir(const std::string& account, const Environment& env);

// Creates `dir` with mode 0700 if missing, then verifies it is a directory
// owned by `uid` with no group/other access — refuses sockets in a
// tamperable location. Returns false with a reason otherwise.
bool ensure_private_dir(const std::string& dir, uid_t uid, std::string& error);

} // namespace tgcli::paths
