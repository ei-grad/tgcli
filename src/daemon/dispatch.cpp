#include "daemon/dispatch.hpp"

#include "common/exit_codes.hpp"

#include <exception>

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

void ResponseSink::item(nlohmann::json data) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!terminal_) {
        emit_item(std::move(data));
    }
}

void ResponseSink::progress(nlohmann::json data) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!terminal_) {
        emit_progress(std::move(data));
    }
}

void ResponseSink::result(nlohmann::json data) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_) {
        return;
    }
    terminal_ = true;
    emit_result(std::move(data));
}

void ResponseSink::error(std::string code, std::string message, nlohmann::json details,
                         int exit_code) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_) {
        return;
    }
    terminal_ = true;
    emit_error(std::move(code), std::move(message), std::move(details), exit_code);
}

bool ResponseSink::has_terminal() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return terminal_;
}

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
    try {
        it->second.handler(request, sink);
    } catch (const std::exception&) {
        sink.error("GENERIC", "command handler failed", nlohmann::json::object(), kGeneric);
        return;
    } catch (...) {
        sink.error("GENERIC", "command handler failed", nlohmann::json::object(), kGeneric);
        return;
    }
    if (!sink.has_terminal()) {
        sink.error("GENERIC", "command handler returned without a terminal response",
                   nlohmann::json::object(), kGeneric);
    }
}

} // namespace tgcli::daemon
