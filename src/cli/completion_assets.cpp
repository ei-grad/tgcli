#include "cli/completion_assets.hpp"

namespace tgcli::cli {

namespace {

#include "cli/completion_assets.generated.inc"

} // namespace

std::optional<std::string_view> completion_asset(std::string_view shell) noexcept {
    if (shell == "bash") {
        return kBashCompletion;
    }
    if (shell == "zsh") {
        return kZshCompletion;
    }
    if (shell == "fish") {
        return kFishCompletion;
    }
    return std::nullopt;
}

} // namespace tgcli::cli
