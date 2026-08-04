#pragma once

#include <string>
#include <sys/types.h>

namespace tgcli::cli::detail {

enum class RuntimeDirectoryState { Absent, Valid, Invalid };

RuntimeDirectoryState inspect_runtime_directory(const std::string& runtime_dir, uid_t uid,
                                                std::string& error);

} // namespace tgcli::cli::detail
