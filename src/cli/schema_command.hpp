#pragma once

#include <span>
#include <string>
#include <string_view>

namespace tgcli::cli {

struct SchemaCommandInvocation {
    std::span<const std::string> target_tokens;
    std::string_view unsupported_option;
    bool all = false;
    bool help = false;
    bool verbose = false;
};

int run_schema_command(const SchemaCommandInvocation& invocation);

} // namespace tgcli::cli
