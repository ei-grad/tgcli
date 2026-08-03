#include "common/paths.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace tgcli::paths {

namespace {

std::optional<std::string> env_value(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

SocketIdentity socket_identity(const struct stat& st) {
#if defined(__APPLE__)
    return SocketIdentity{st.st_dev, st.st_ino, st.st_ctimespec.tv_sec, st.st_ctimespec.tv_nsec};
#else
    return SocketIdentity{st.st_dev, st.st_ino, st.st_ctim.tv_sec, st.st_ctim.tv_nsec};
#endif
}

std::optional<std::string> endpoint_path(const std::string& account, const Environment& env,
                                         std::string_view suffix, std::string& error) {
    if (!valid_account_name(account)) {
        error = "invalid account name '" + account + "': use 1-" +
                std::to_string(kMaxAccountNameLength) + " characters from [A-Za-z0-9_-]";
        return std::nullopt;
    }
    std::string path = runtime_dir(env) + "/" + account + std::string(suffix);
    // sun_path must hold the path plus its NUL terminator.
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "socket path too long for sun_path (" + std::to_string(path.size()) +
                " bytes): " + path;
        return std::nullopt;
    }
    return path;
}

std::optional<SocketIdentity> inspect_socket(const std::string& socket_path, uid_t uid,
                                             bool allow_missing, bool& missing,
                                             std::string& error) {
    struct stat st {};
    if (::lstat(socket_path.c_str(), &st) != 0) {
        if (errno == ENOENT && allow_missing) {
            missing = true;
            return std::nullopt;
        }
        error = "cannot stat " + socket_path + ": " + std::strerror(errno);
        return std::nullopt;
    }
    missing = false;
    if (allow_missing && st.st_nlink == 0) {
        missing = true;
        return std::nullopt;
    }
    if (!S_ISSOCK(st.st_mode)) {
        error = socket_path + " exists and is not a unix socket";
        return std::nullopt;
    }
    if (st.st_uid != uid) {
        error = socket_path + " is owned by uid " + std::to_string(st.st_uid) + ", not " +
                std::to_string(uid);
        return std::nullopt;
    }
    if ((st.st_mode & 07777) != 0600) {
        error = socket_path + " has unsafe permissions; expected mode 0600";
        return std::nullopt;
    }
    if (st.st_nlink != 1) {
        error = socket_path + " has an unexpected hard-link count";
        return std::nullopt;
    }
    return socket_identity(st);
}

} // namespace

bool valid_account_name(const std::string& name) {
    if (name.empty() || name.size() > kMaxAccountNameLength) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
    });
}

Environment real_environment() {
    Environment env;
    env.xdg_runtime_dir = env_value("XDG_RUNTIME_DIR");
    env.xdg_config_home = env_value("XDG_CONFIG_HOME");
    env.xdg_data_home = env_value("XDG_DATA_HOME");
    env.xdg_state_home = env_value("XDG_STATE_HOME");
    env.tmpdir = env_value("TMPDIR");
    env.home = env_value("HOME").value_or("/");
    env.uid = getuid();
    return env;
}

std::string runtime_dir(const Environment& env) {
    if (env.xdg_runtime_dir) {
        return *env.xdg_runtime_dir + "/tgcli";
    }
    const std::string base = env.tmpdir.value_or("/tmp");
    return base + "/tgcli-" + std::to_string(env.uid);
}

std::optional<std::string> socket_path(const std::string& account, const Environment& env,
                                       std::string& error) {
    return endpoint_path(account, env, ".sock", error);
}

std::optional<std::string> control_socket_path(const std::string& account, const Environment& env,
                                               std::string& error) {
    return endpoint_path(account, env, ".ctl", error);
}

std::string config_file(const Environment& env) {
    const std::string base = env.xdg_config_home.value_or(env.home + "/.config");
    return base + "/tgcli/config.toml";
}

std::string account_data_dir(const std::string& account, const Environment& env) {
    const std::string base = env.xdg_data_home.value_or(env.home + "/.local/share");
    return base + "/tgcli/accounts/" + account;
}

std::string account_state_dir(const std::string& account, const Environment& env) {
    const std::string base = env.xdg_state_home.value_or(env.home + "/.local/state");
    return base + "/tgcli/accounts/" + account;
}

bool ensure_private_dir(const std::string& dir, uid_t uid, std::string& error) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "cannot create " + dir + ": " + std::string(std::strerror(errno));
        return false;
    }
    return validate_private_dir(dir, uid, error);
}

bool validate_private_dir(const std::string& dir, uid_t uid, std::string& error) {
    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0) {
        error = "cannot stat " + dir + ": " + std::string(std::strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        error = dir + " exists and is not a directory";
        return false;
    }
    if (st.st_uid != uid) {
        error =
            dir + " is owned by uid " + std::to_string(st.st_uid) + ", not " + std::to_string(uid);
        return false;
    }
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        error = dir + " is group/other accessible; expected mode 0700";
        return false;
    }
    return true;
}

std::optional<SocketIdentity> inspect_socket_endpoint(const std::string& socket_path, uid_t uid,
                                                      std::string& error) {
    bool missing = false;
    return inspect_socket(socket_path, uid, false, missing, error);
}

bool find_socket_endpoint(const std::string& socket_path, uid_t uid,
                          std::optional<SocketIdentity>& identity, std::string& error) {
    bool missing = false;
    identity = inspect_socket(socket_path, uid, true, missing, error);
    return missing || identity.has_value();
}

bool prepare_socket_endpoint(const std::string& socket_path, uid_t uid, std::string& error) {
    const auto separator = socket_path.rfind('/');
    if (separator == std::string::npos || separator == 0) {
        error = "socket path has no private parent directory: " + socket_path;
        return false;
    }
    if (!ensure_private_dir(socket_path.substr(0, separator), uid, error)) {
        return false;
    }

    bool missing = false;
    const auto existing = inspect_socket(socket_path, uid, true, missing, error);
    if (missing) {
        return true;
    }
    if (!existing) {
        return false;
    }
    if (::unlink(socket_path.c_str()) != 0) {
        error = "cannot remove stale socket " + socket_path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool socket_endpoint_changed(const std::string& socket_path, uid_t uid,
                             const SocketIdentity& identity, bool& changed, std::string& error) {
    bool missing = false;
    const auto current = inspect_socket(socket_path, uid, true, missing, error);
    if (missing) {
        changed = true;
        return true;
    }
    if (!current) {
        return false;
    }
    changed = *current != identity;
    return true;
}

void unlink_socket_endpoint_if_same(const std::string& socket_path,
                                    const SocketIdentity& identity) {
    struct stat st {};
    if (::lstat(socket_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) &&
        socket_identity(st) == identity) {
        ::unlink(socket_path.c_str());
    }
}

} // namespace tgcli::paths
