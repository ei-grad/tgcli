#include "daemon/dispatch.hpp"

#include "common/exit_codes.hpp"

namespace tgcli::daemon {

namespace {

std::string command_key(const std::vector<std::string>& command) {
    std::string key;
    for (const auto& part : command) {
        if (!key.empty()) {
            key += ' ';
        }
        key += part;
    }
    return key;
}

} // namespace

void Dispatcher::register_command(const std::string& path, CommandDescriptor descriptor) {
    commands_.emplace(path, std::move(descriptor));
}

void Dispatcher::dispatch(const proto::Request& request, ResponseSink& sink) const {
    const auto key = command_key(request.command);
    const auto it = commands_.find(key);
    if (it == commands_.end()) {
        sink.error("USAGE", "unknown command '" + key + "'", nlohmann::json::object(), kUsage);
        return;
    }
    // The write gate (DESIGN.md §6) is evaluated here and nowhere else. Its
    // full semantics (grants, explicit deny, confirmation challenges) land
    // with M3; until then every non-Read command is denied — fail closed.
    if (it->second.tier != Tier::Read) {
        sink.error("DENIED", "write-tier commands are not implemented yet (fail-closed gate)",
                   nlohmann::json::object(), kDenied);
        return;
    }
    it->second.handler(request, sink);
}

} // namespace tgcli::daemon
