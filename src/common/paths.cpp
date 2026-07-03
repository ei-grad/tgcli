#include "common/paths.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
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
    if (!valid_account_name(account)) {
        error = "invalid account name '" + account + "': use 1-" +
                std::to_string(kMaxAccountNameLength) + " characters from [A-Za-z0-9_-]";
        return std::nullopt;
    }
    std::string path = runtime_dir(env) + "/" + account + ".sock";
    // sun_path must hold the path plus its NUL terminator.
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "socket path too long for sun_path (" + std::to_string(path.size()) +
                " bytes): " + path;
        return std::nullopt;
    }
    return path;
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

} // namespace tgcli::paths
