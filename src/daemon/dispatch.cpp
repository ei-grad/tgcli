#include "daemon/dispatch.hpp"

#include "common/exit_codes.hpp"
#include "daemon/request_session.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>

namespace tgcli::daemon {

namespace {

constexpr M3OperationPolicy policy(M3Operation operation, Tier tier, M3BotPolicy bot_policy) {
    const auto* identity = proto::m3_operation_identity(operation);
    return {operation, identity->canonical_name, identity->command_path, tier, bot_policy};
}

constexpr std::array<M3OperationPolicy, 17> kM3OperationPolicies{{
    policy(M3Operation::Send, Tier::Write, M3BotPolicy::ImmediateOnly),
    policy(M3Operation::MsgEdit, Tier::Write, M3BotPolicy::Allowed),
    policy(M3Operation::MsgDelete, Tier::Destructive, M3BotPolicy::Allowed),
    policy(M3Operation::MsgForward, Tier::Write, M3BotPolicy::Allowed),
    policy(M3Operation::MsgReact, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::MsgPin, Tier::Write, M3BotPolicy::Allowed),
    policy(M3Operation::MsgUnpin, Tier::Write, M3BotPolicy::Allowed),
    policy(M3Operation::ChatMarkRead, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatMute, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatUnmute, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatPin, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatUnpin, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatArchive, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatUnarchive, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatJoin, Tier::Write, M3BotPolicy::UserOnly),
    policy(M3Operation::ChatLeave, Tier::Destructive, M3BotPolicy::Allowed),
    policy(M3Operation::SavedAttach, Tier::Write, M3BotPolicy::UserOnly),
}};

static_assert(kM3OperationPolicies.size() == proto::kM3OperationIdentities.size());

constexpr bool is_m1_destructive_command(std::string_view path) noexcept {
    return path == "logout" || path == "account remove";
}

constexpr bool allows_unlimited_default(std::string_view path) noexcept {
    return path == "fetch" || path == "download" || path == "listen" || path == "wait-for";
}

constexpr bool is_valid_m3_schedule(M3ScheduleKind schedule) noexcept {
    switch (schedule) {
    case M3ScheduleKind::None:
    case M3ScheduleKind::At:
    case M3ScheduleKind::Online:
        return true;
    }
    return false;
}

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

bool reject_invalid_idempotency(const proto::Request& request, RequestSession& session) {
    if (!request.context.idempotency_key) {
        return false;
    }
    if (!proto::valid_idempotency_key(*request.context.idempotency_key)) {
        session.error("USAGE", "invalid idempotency key",
                      {{"argument", "--idempotency-key"}, {"reason", "invalid_argument"}}, kUsage);
        return true;
    }
    if (request.context.dry_run) {
        session.error("USAGE", "idempotency key cannot be combined with dry-run",
                      {{"argument", "--idempotency-key"}, {"reason", "mutually_exclusive"}},
                      kUsage);
        return true;
    }
    if (!proto::m3_operation_for_command(request.command)) {
        session.error("USAGE", "idempotency key is unsupported for this command",
                      {{"argument", "--idempotency-key"}, {"reason", "unsupported_mode"}}, kUsage);
        return true;
    }
    return false;
}

constexpr bool publicly_active_m3(const std::optional<M3Operation>& operation) noexcept {
    return operation == M3Operation::Send || operation == M3Operation::MsgEdit ||
           operation == M3Operation::MsgDelete || operation == M3Operation::MsgForward ||
           operation == M3Operation::MsgReact || operation == M3Operation::MsgPin ||
           operation == M3Operation::MsgUnpin || operation == M3Operation::ChatMarkRead ||
           operation == M3Operation::ChatMute || operation == M3Operation::ChatUnmute ||
           operation == M3Operation::ChatPin || operation == M3Operation::ChatUnpin ||
           operation == M3Operation::ChatArchive || operation == M3Operation::ChatUnarchive ||
           operation == M3Operation::ChatJoin || operation == M3Operation::ChatLeave ||
           operation == M3Operation::SavedAttach;
}

} // namespace

std::span<const M3OperationPolicy> m3_operation_policies() {
    return kM3OperationPolicies;
}

const M3OperationPolicy* m3_operation_policy(M3Operation operation) {
    const auto* const found =
        std::ranges::find(kM3OperationPolicies, operation, &M3OperationPolicy::operation);
    return found == kM3OperationPolicies.end() ? nullptr : &*found;
}

std::optional<M3Operation> parse_m3_operation(std::string_view canonical_name) {
    return proto::parse_m3_operation(canonical_name);
}

std::optional<M3Operation> m3_operation_for_command(std::string_view command_path) {
    return proto::m3_operation_for_command(command_path);
}

M3BotAdmission evaluate_m3_bot_admission(M3Operation operation, bool is_bot,
                                         M3ScheduleKind schedule) {
    if (!is_valid_m3_schedule(schedule)) {
        return M3BotAdmission::Unsupported;
    }
    const auto* policy = m3_operation_policy(operation);
    if (policy == nullptr) {
        return M3BotAdmission::Unsupported;
    }
    if (!is_bot) {
        return M3BotAdmission::Allowed;
    }
    switch (policy->bot_policy) {
    case M3BotPolicy::Allowed:
        return M3BotAdmission::Allowed;
    case M3BotPolicy::ImmediateOnly:
        return schedule == M3ScheduleKind::None ? M3BotAdmission::Allowed
                                                : M3BotAdmission::Unsupported;
    case M3BotPolicy::UserOnly:
        return M3BotAdmission::Unsupported;
    }
    return M3BotAdmission::Unsupported;
}

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
    if ((descriptor.deadline_default == DeadlineDefault::Unlimited) !=
        allows_unlimited_default(path)) {
        throw std::invalid_argument("unlimited deadline policy does not match its command");
    }
    if (descriptor.m1_destructive_kernel &&
        (descriptor.tier != Tier::Destructive || !is_m1_destructive_command(path))) {
        throw std::invalid_argument("M1 destructive bypass does not match its static policy");
    }
    const auto reserved_operation = m3_operation_for_command(path);
    if (!descriptor.m3_operation) {
        if (reserved_operation) {
            throw std::invalid_argument("M3 command descriptor is missing its operation identity");
        }
    } else {
        const auto* policy = m3_operation_policy(*descriptor.m3_operation);
        if (policy == nullptr || policy->command_path != path || policy->tier != descriptor.tier ||
            descriptor.m1_destructive_kernel) {
            throw std::invalid_argument("M3 command descriptor does not match its static policy");
        }
    }
    commands_.emplace(path, std::move(descriptor));
}

DeadlineDefault Dispatcher::deadline_default(const proto::Request& request) const {
    const auto found = commands_.find(command_key(request.command));
    return found == commands_.end() ? DeadlineDefault::Default60 : found->second.deadline_default;
}

bool Dispatcher::requires_frozen_config_admission(const proto::Request& request) const {
    const auto found = commands_.find(command_key(request.command));
    return found != commands_.end() && found->second.m3_operation.has_value();
}

void Dispatcher::set_request_preflight(
    std::function<bool(const std::string&, RequestSession&)> request_preflight) {
    request_preflight_ = std::move(request_preflight);
}

void Dispatcher::dispatch(RequestSession& session) const {
    const auto& request = session.request();
    if (reject_invalid_idempotency(request, session)) {
        return;
    }
    const auto key = command_key(request.command);
    if (request_observer_) {
        request_observer_(testing::RequestObservationStage::DispatcherLookup);
    }
    const auto it = commands_.find(key);
    if (it == commands_.end()) {
        session.error("USAGE", "unknown command '" + key + "'", nlohmann::json::object(), kUsage);
        return;
    }
    if (request_preflight_ && !request_preflight_(key, session)) {
        return;
    }
    // M1 opens only the audited destructive kernel used by logout and account
    // removal. Every other non-read descriptor remains denied until M3.
    const bool m1_destructive = is_m1_destructive_command(key) &&
                                it->second.tier == Tier::Destructive &&
                                it->second.m1_destructive_kernel;
    const bool active_m3 = publicly_active_m3(it->second.m3_operation);
    if (it->second.tier != Tier::Read && !m1_destructive && !active_m3) {
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
    if (session.cancellation_requested()) {
        return;
    }
    if (!session.has_terminal()) {
        session.error("GENERIC", "command handler returned without a terminal response",
                      nlohmann::json::object(), kGeneric);
    }
}

void Dispatcher::dispatch(const proto::Request& request, ResponseSink& sink) const {
    std::string source_error;
    auto admitted_request = proto::admit_request_source(request, source_error);
    if (!admitted_request) {
        throw std::invalid_argument(source_error);
    }
    if (requires_frozen_config_admission(*admitted_request)) {
        throw std::invalid_argument("direct M3 dispatch requires frozen config admission");
    }
    const auto admitted_at = RequestClock::now();
    const auto admission_wall_time =
        request_wall_clock_ ? request_wall_clock_() : RequestSession::WallClock::now();
    const auto deadline = request_deadline(admitted_request->context.timeout_seconds,
                                           deadline_default(*admitted_request), admitted_at);
    if (!deadline) {
        throw std::invalid_argument("request timeout must be finite, positive, and representable");
    }
    RequestSession session(*admitted_request, sink, 0, RequestSession::NonceGenerator{},
                           ActivityTracker::Token{}, nullptr, deadline,
                           ConfigAdmissionMode::DirectFallback, admission_wall_time);
    dispatch(session);
}

} // namespace tgcli::daemon
