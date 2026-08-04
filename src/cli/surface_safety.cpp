#include "cli/surface_safety.hpp"

#include "common/paths.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

namespace tgcli::cli::detail {

RuntimeDirectoryState inspect_runtime_directory(const std::string& runtime_dir, uid_t uid,
                                                std::string& error) {
    struct stat metadata {};
    if (::lstat(runtime_dir.c_str(), &metadata) != 0) {
        if (errno == ENOENT) {
            error.clear();
            return RuntimeDirectoryState::Absent;
        }
        error = "cannot inspect " + runtime_dir + ": " + std::strerror(errno);
        return RuntimeDirectoryState::Invalid;
    }
    if (!paths::validate_private_dir(runtime_dir, uid, error)) {
        return RuntimeDirectoryState::Invalid;
    }
    return RuntimeDirectoryState::Valid;
}

} // namespace tgcli::cli::detail
