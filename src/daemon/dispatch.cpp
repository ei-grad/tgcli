#include "daemon/dispatch.hpp"

#include "common/exit_codes.hpp"
#include "daemon/request_session.hpp"

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

std::optional<nlohmann::json> ResponseSink::challenge(nlohmann::json data) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_) {
        return std::nullopt;
    }
    auto reply = emit_challenge(std::move(data));
    if (reply.failure) {
        terminal_ = true;
        auto failure = std::move(*reply.failure);
        emit_error(std::move(failure.code), std::move(failure.message), std::move(failure.details),
                   failure.exit_code);
        return std::nullopt;
    }
    return std::move(reply.answer);
}

void Dispatcher::register_command(const std::string& path, CommandDescriptor descriptor) {
    commands_.emplace(path, std::move(descriptor));
}

void Dispatcher::dispatch(RequestSession& session) const {
    const auto& request = session.request();
    const auto key = command_key(request.command);
    const auto it = commands_.find(key);
    if (it == commands_.end()) {
        session.error("USAGE", "unknown command '" + key + "'", nlohmann::json::object(), kUsage);
        return;
    }
    // The write gate (DESIGN.md §6) is evaluated here and nowhere else. Its
    // full semantics (grants, explicit deny, confirmation challenges) land
    // with M3; until then every non-Read command is denied — fail closed.
    if (it->second.tier != Tier::Read) {
        session.error("DENIED", "write-tier commands are not implemented yet (fail-closed gate)",
                      nlohmann::json::object(), kDenied);
        return;
    }
    try {
        it->second.handler(request, session);
    } catch (const std::exception&) {
        session.error("GENERIC", "command handler failed", nlohmann::json::object(), kGeneric);
        return;
    } catch (...) {
        session.error("GENERIC", "command handler failed", nlohmann::json::object(), kGeneric);
        return;
    }
    if (!session.has_terminal()) {
        session.error("GENERIC", "command handler returned without a terminal response",
                      nlohmann::json::object(), kGeneric);
    }
}

void Dispatcher::dispatch(const proto::Request& request, ResponseSink& sink) const {
    RequestSession session(request, sink);
    dispatch(session);
}

} // namespace tgcli::daemon
