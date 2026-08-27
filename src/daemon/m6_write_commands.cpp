#include "common/canonical_json.hpp"
#include "common/exit_codes.hpp"
#include "common/secure_wipe.hpp"
#include "common/sha256.hpp"
#include "daemon/chat_identity.hpp"
#include "daemon/direct_rpc.hpp"
#include "daemon/m6_audit_contract.hpp"
#include "daemon/m6_capability.hpp"
#include "daemon/m6_commands.hpp"
#include "daemon/m6_domain.hpp"
#include "daemon/m6_model.hpp"
#include "daemon/m6_write_policy.hpp"
#include "daemon/rate_limit.hpp"
#include "daemon/request_fingerprint.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "daemon/write_commands.hpp"
#include "daemon/write_contract.hpp"
#include "daemon/write_kernel.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

struct M6DispatchState {
    std::optional<core::TdM6Request> request;
    std::shared_ptr<const core::AuthStateSnapshot> authorization;
    std::unique_ptr<DirectRpcCoordinator> coordinator;
};

struct SessionDispatchState {
    std::optional<core::TdTerminateSessionRequest> request;
    std::shared_ptr<const core::AuthStateSnapshot> authorization;
    std::unique_ptr<DirectRpcCoordinator> coordinator;
};

struct M6WriteDefinition {
    proto::M6Operation operation = proto::M6Operation::ContactAdd;
    ResolverPrincipal principal;
    std::function<WriteAdmissionOutcome()> admit;
    std::function<WritePlanningOutcome()> plan;
    std::function<WriteLookupAdmissionOutcome()> lookup_admit;
    std::function<WriteMaterializationOutcome(const WriteLookupAdmission&)> materialize;
    std::function<std::optional<write_contract::StoredTerminal>(const write_contract::Plan&,
                                                                const core::TdM6Response&)>
        success;
    std::function<WriteConfirmationOutcome(const write_contract::Plan&, bool)> confirm;
    std::function<WritePostIntentPreparation(const write_contract::Plan&, const WriteAdmission&,
                                             std::string_view invocation_id)>
        post_intent;
    std::shared_ptr<M6DispatchState> dispatch;
};

struct PreparedM6Mutation {
    proto::M6Operation operation = proto::M6Operation::ContactAdd;
    ResolverPrincipal principal;
    json fingerprint_payload;
    json arguments;
    json plan;
    core::TdM6Request request;
    std::shared_ptr<PreparedSource> pass1_source;
    std::function<std::optional<json>(const core::TdM6Response&, const json&)> result;
};

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields,
                               [&](std::string_view field) { return value.contains(field); });
}

std::string_view operation_name(proto::M6Operation operation) {
    const auto* identity = proto::m6_operation_identity(operation);
    return identity == nullptr ? std::string_view{} : identity->canonical_name;
}

json terminal(std::string code, std::string message, json details, int exit_code) {
    return {{"kind", "error"},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"details", std::move(details)},
            {"exit_code", exit_code}};
}

json internal(proto::M6Operation operation) {
    return terminal("INTERNAL", "internal error",
                    {{"operation", operation_name(operation)}, {"reason", "internal_error"}},
                    kGeneric);
}

json malformed(proto::M6Operation operation) {
    return terminal(
        "INTERNAL", "TDLib returned a malformed response",
        {{"operation", operation_name(operation)}, {"reason", "malformed_tdlib_response"}},
        kGeneric);
}

json not_authed(std::string_view account, core::AuthState state) {
    return terminal("NOT_AUTHED", "authorization was lost",
                    {{"account", account},
                     {"state", core::auth_state_name(state)},
                     {"reason", "authorization_lost"}},
                    kNotAuthed);
}

json timeout(proto::M6Operation operation, std::string_view phase, std::string_view idempotency) {
    return terminal("TIMEOUT", "request timed out",
                    {{"operation", operation_name(operation)},
                     {"phase", phase},
                     {"state", "ready"},
                     {"outcome", phase == "dispatch" ? "unknown" : "not_started"},
                     {"idempotency", idempotency}},
                    kTimeout);
}

json td_error_terminal(proto::M6Operation operation, const core::TdError& error) {
    if (error.code == 429) {
        return terminal("RATE_LIMITED", "Telegram rate limit exceeded",
                        {{"operation", operation_name(operation)},
                         {"tdlib_code", 429},
                         {"retry_after", parse_retry_after_seconds(error.message)}},
                        kRateLimited);
    }
    return terminal("TDLIB_ERROR", "Telegram request failed",
                    {{"operation", operation_name(operation)}, {"tdlib_code", error.code}},
                    kGeneric);
}

json shutdown_terminal() {
    return terminal("DAEMON_SHUTDOWN", "daemon is shutting down", {{"reason", "daemon_shutdown"}},
                    kGeneric);
}

std::string_view spool_reason_name(DurabilityReason reason) {
    switch (reason) {
    case DurabilityReason::PathInvalid:
        return "path_invalid";
    case DurabilityReason::WrongOwner:
        return "wrong_owner";
    case DurabilityReason::WrongType:
        return "wrong_type";
    case DurabilityReason::WrongMode:
        return "wrong_mode";
    case DurabilityReason::WrongLinkCount:
        return "wrong_link_count";
    case DurabilityReason::TooLarge:
        return "too_large";
    case DurabilityReason::CapacityExhausted:
        return "capacity_exhausted";
    case DurabilityReason::OpenFailed:
        return "open_failed";
    case DurabilityReason::LockFailed:
        return "lock_failed";
    case DurabilityReason::ReadFailed:
        return "read_failed";
    case DurabilityReason::WriteFailed:
        return "write_failed";
    case DurabilityReason::SyncFailed:
        return "sync_failed";
    case DurabilityReason::RenameFailed:
        return "rename_failed";
    case DurabilityReason::DirectorySyncFailed:
        return "directory_sync_failed";
    case DurabilityReason::ParseError:
        return "parse_error";
    case DurabilityReason::SchemaError:
        return "schema_error";
    case DurabilityReason::Contradiction:
        return "contradiction";
    }
    return "contradiction";
}

std::string_view source_reason_name(SourceFileReason reason) {
    switch (reason) {
    case SourceFileReason::Missing:
        return "missing";
    case SourceFileReason::Symlink:
        return "symlink";
    case SourceFileReason::WrongType:
        return "wrong_type";
    case SourceFileReason::Empty:
        return "empty";
    case SourceFileReason::Unreadable:
        return "unreadable";
    }
    return "unreadable";
}

json file_snapshot_json(const FileSnapshot& file) {
    return {{"path", file.path},         {"name", file.name},        {"size", file.size},
            {"sha256", file.sha256},     {"device", file.device},    {"inode", file.inode},
            {"mtime_ns", file.mtime_ns}, {"ctime_ns", file.ctime_ns}};
}

void emit_terminal(RequestSession& session, const json& value) {
    if (!value.is_object()) {
        return;
    }
    if (value.value("kind", std::string{}) == "result") {
        session.result(value.at("data"));
        return;
    }
    if (value.value("kind", std::string{}) == "error") {
        session.error(value.at("code").get<std::string>(), value.at("message").get<std::string>(),
                      value.at("details"), value.at("exit_code").get<int>());
    }
}

write_contract::StoredTerminal stored_error(proto::M6Operation operation, std::string code,
                                            std::string message, json details, int exit_code) {
    std::string error;
    auto value = write_contract::make_error_terminal(operation, std::move(code), std::move(message),
                                                     std::move(details), exit_code, error);
    if (!value) {
        throw std::logic_error(error);
    }
    return std::move(*value);
}

write_contract::StoredTerminal stored_from_terminal(proto::M6Operation operation,
                                                    const json& value) {
    return stored_error(operation, value.at("code").get<std::string>(),
                        value.at("message").get<std::string>(), value.at("details"),
                        value.at("exit_code").get<int>());
}

std::optional<write_contract::StoredTerminal> stored_result(proto::M6Operation operation,
                                                            json data) {
    std::string error;
    auto result = write_contract::make_result(operation, std::move(data), error);
    if (!result) {
        return std::nullopt;
    }
    return write_contract::make_result_terminal(*result, error);
}

constexpr auto kSessionTerminate = AccountAuditOperation::SessionTerminate;

WriteOperation session_operation() noexcept {
    return WriteOperation{kSessionTerminate};
}

write_contract::StoredTerminal session_stored_error(std::string code, std::string message,
                                                    json details, int exit_code) {
    std::string error;
    auto value = write_contract::make_error_terminal(session_operation(), std::move(code),
                                                     std::move(message), std::move(details),
                                                     exit_code, error);
    if (!value) {
        throw std::logic_error(error);
    }
    return std::move(*value);
}

write_contract::StoredTerminal session_stored_from(const json& value) {
    return session_stored_error(value.at("code").get<std::string>(),
                                value.at("message").get<std::string>(), value.at("details"),
                                value.at("exit_code").get<int>());
}

std::optional<write_contract::StoredTerminal> session_stored_result(json data) {
    std::string error;
    auto result = write_contract::make_result(session_operation(), std::move(data), error);
    return result ? write_contract::make_result_terminal(*result, error) : std::nullopt;
}

std::uint64_t unix_seconds() {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds < 0 ? std::numeric_limits<std::uint64_t>::max()
                       : static_cast<std::uint64_t>(seconds);
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (gmtime_r(&seconds, &utc) == nullptr) {
        return {};
    }
    std::array<char, 21> rendered{};
    if (std::strftime(rendered.data(), rendered.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
    }
    return rendered.data();
}

std::string random_hex32() {
    std::array<unsigned char, 16> bytes{};
    const int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return {};
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(descriptor);
            return {};
        }
        offset += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const auto byte : bytes) {
        result.push_back(digits.at(byte >> 4U));
        result.push_back(digits.at(byte & 0x0fU));
    }
    return result;
}

std::optional<IdempotencyKeyHash> key_hash(const proto::Request& request) {
    if (!request.context.idempotency_key) {
        return std::nullopt;
    }
    return parse_idempotency_key_hash(idempotency_key_hash(*request.context.idempotency_key));
}

std::optional<IdempotencyRequestFingerprint> m6_fingerprint(std::string_view account,
                                                            const ResolverPrincipal& principal,
                                                            proto::M6Operation operation,
                                                            json payload) {
    if (operation == proto::M6Operation::ChatInviteLink && payload.contains("revoke") &&
        payload["revoke"].is_string()) {
        payload["revoke"] = common::domain_separated_sha256(
            "tgcli.m6.invite-link.v1", payload["revoke"].get_ref<const std::string&>());
    }
    const json root{{"version", 1},
                    {"account", account},
                    {"principal", {{"id", principal.id}, {"is_bot", principal.is_bot}}},
                    {"operation", operation_name(operation)},
                    {"payload", std::move(payload)}};
    auto canonical = common::canonical_json(root);
    const auto* serialized = std::get_if<std::string>(&canonical);
    if (serialized == nullptr) {
        return std::nullopt;
    }
    return parse_idempotency_request_fingerprint(
        common::domain_separated_sha256("tgcli-idempotency-request-v1", *serialized));
}

std::optional<AuthoritySource> authorize(const proto::Request& request, RequestSession& session,
                                         std::string_view account, proto::M6Operation operation) {
    const auto& admitted = session.admitted_config();
    if (!admitted || admitted->account != account || !admitted->account_snapshot) {
        session.error("INTERNAL", "write config admission is missing",
                      {{"operation", operation_name(operation)}, {"reason", "internal_error"}},
                      kGeneric);
        return std::nullopt;
    }
    if (request.context.dry_run) {
        return AuthoritySource::Request;
    }
    const auto decision = evaluate_destructive_authority(
        request.context, {.grant_valid = admitted->standing_write_grants_valid,
                          .allow_write = admitted->settings.allow_write});
    if (const auto* denied = std::get_if<DeniedAuthority>(&decision)) {
        session.error("WRITE_DENIED", "write requires explicit authority",
                      {{"account", account}, {"reason", write_denial_reason_name(denied->reason)}},
                      kDenied);
        return std::nullopt;
    }
    const auto* granted = std::get_if<GrantedAuthority>(&decision);
    if (granted == nullptr) {
        session.error("INTERNAL", "write authority decision is invalid",
                      {{"operation", operation_name(operation)}, {"reason", "internal_error"}},
                      kGeneric);
        return std::nullopt;
    }
    return granted->source;
}

WriteKernelRequest kernel_request(const proto::Request& request, RequestSession& session,
                                  proto::M6Operation operation, AuthoritySource source,
                                  std::optional<IdempotencyKeyHash> hash, std::string invocation,
                                  std::string config_path) {
    const auto& admitted = session.admitted_config();
    return {.operation = operation,
            .account = request.account,
            .idempotency_key_hash = std::move(hash),
            .invocation_id = std::move(invocation),
            .intent_timestamp = timestamp(),
            .config_path = std::move(config_path),
            .config_snapshot = admitted ? admitted->snapshot_identity : std::string("missing"),
            .authority_source = source,
            .request_source_bytes = session.request_source_bytes(),
            .sample_now = unix_seconds,
            .dry_run = request.context.dry_run,
            .deadline = session.deadline(),
            .cancellation_token = session.cancellation_token(),
            .cancelled = [&session] { return session.cancellation_requested(); }};
}

AccountAuditMutationState audit_state(DirectMutationState state) {
    switch (state) {
    case DirectMutationState::None:
        return AccountAuditMutationState::None;
    case DirectMutationState::Possible:
        return AccountAuditMutationState::Possible;
    case DirectMutationState::Confirmed:
        return AccountAuditMutationState::Confirmed;
    }
    return AccountAuditMutationState::Possible;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact two-epoch M6 transaction.
void execute_m6_write(
    core::TdClient& client, std::string_view account, config::Store& config_store,
    const std::shared_ptr<IdempotencyFoundation>& foundation,
    const std::function<void()>& audit_fatal_shutdown,
    const std::shared_ptr<const testing::WriteCoordinatorHooks>& coordinator_hooks,
    const proto::Request& request, RequestSession& session, AuthoritySource authority,
    M6WriteDefinition definition) {
    if (!request.context.dry_run &&
        session.begin_audited_terminal() != AuditedTerminalStatus::Designated) {
        return;
    }
    auto hash = key_hash(request);
    if (request.context.idempotency_key && !hash) {
        session.error(
            "INTERNAL", "cannot hash idempotency key",
            {{"operation", operation_name(definition.operation)}, {"reason", "internal_error"}},
            kGeneric);
        return;
    }
    auto invocation = request.context.dry_run ? std::string{} : random_hex32();
    if (!request.context.dry_run && invocation.empty()) {
        session.error("AUDIT_UNAVAILABLE", "cannot create audit identity",
                      {{"account", account},
                       {"path", foundation ? foundation->audit().path() : std::string{}},
                       {"reason", "open_failed"}},
                      kDenied);
        return;
    }
    const bool deferred_planning = definition.lookup_admit || definition.materialize;
    if ((deferred_planning ? (!definition.lookup_admit || !definition.materialize)
                           : (!definition.admit || !definition.plan)) ||
        !definition.success || !definition.dispatch) {
        session.error(
            "INTERNAL", "write operation is incomplete",
            {{"operation", operation_name(definition.operation)}, {"reason", "internal_error"}},
            kGeneric);
        return;
    }

    const auto operation = definition.operation;
    const WriteKernel kernel(foundation);
    auto input = kernel_request(request, session, operation, authority, std::move(hash),
                                std::move(invocation), config_store.path());
    WriteKernelHooks hooks;
    hooks.admit = std::move(definition.admit);
    hooks.plan = definition.plan
                     ? [plan = std::move(definition.plan)](const WriteAdmission&) { return plan(); }
                     : std::function<WritePlanningOutcome(const WriteAdmission&)>{};
    hooks.lookup_admit = std::move(definition.lookup_admit);
    hooks.materialize = std::move(definition.materialize);
    hooks.confirm =
        definition.confirm ? std::move(definition.confirm) : [](const write_contract::Plan&, bool) {
            return WriteConfirmationOutcome{.status = WriteConfirmationStatus::ConfirmedYes,
                                            .terminal = std::nullopt};
        };
    hooks.verify_config_grant = [&config_store](std::string_view expected,
                                                std::string_view expected_account,
                                                const config::MutationControl& control) {
        return config_store.verify_write_grant(expected, expected_account, control);
    };
    const auto principal = definition.principal;
    hooks.revalidate_principal = [&client, account_value = std::string(account), principal,
                                  coordinator_hooks]() -> std::optional<json> {
        if (coordinator_hooks && coordinator_hooks->before_principal_cas) {
            coordinator_hooks->before_principal_cas();
        }
        const auto current = client.auth_state();
        if (current && current->data.state == core::AuthState::Ready &&
            current->client_id == principal.client_id &&
            current->client_generation == principal.client_generation &&
            current->auth_sequence == principal.auth_sequence) {
            return std::nullopt;
        }
        return not_authed(account_value,
                          current != nullptr ? current->data.state : core::AuthState::Unknown);
    };
    if (definition.post_intent) {
        hooks.post_intent =
            [post_intent = std::move(definition.post_intent), invocation_id = input.invocation_id](
                const write_contract::Plan& plan, const WriteAdmission& admission) mutable {
                return post_intent(plan, admission, invocation_id);
            };
    } else {
        hooks.post_intent = [](const write_contract::Plan&, const WriteAdmission&) {
            return WritePostIntentPreparation{};
        };
    }
    const auto dispatch = definition.dispatch;
    hooks.revalidate_auth_and_schedule =
        [&client, &session, &request, dispatch, operation, account_value = std::string(account),
         coordinator_hooks,
         principal](const write_contract::Plan& plan) -> WriteDispatchAdmissionOutcome {
        if (coordinator_hooks && coordinator_hooks->before_dispatch_principal_cas) {
            coordinator_hooks->before_dispatch_principal_cas();
        }
        if (deadline_expired(session.deadline())) {
            return stored_from_terminal(
                operation, timeout(operation, "preflight",
                                   request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (session.cancellation_requested()) {
            return WriteDispatchStopped{};
        }
        auto current = client.auth_state();
        if (!current || current->data.state != core::AuthState::Ready ||
            current->client_id != principal.client_id ||
            current->client_generation != principal.client_generation ||
            current->auth_sequence != principal.auth_sequence) {
            return stored_from_terminal(operation,
                                        not_authed(account_value, current != nullptr
                                                                      ? current->data.state
                                                                      : core::AuthState::Unknown));
        }
        dispatch->authorization = std::move(current);
        if (!dispatch->request || plan.value()["tdlib_request"].is_null() ||
            !plan.value()["tdlib_request"].is_string()) {
            return stored_from_terminal(operation, internal(operation));
        }
        const auto direct_hooks =
            coordinator_hooks ? coordinator_hooks->direct_rpc : DirectRpcHooks{};
        dispatch->coordinator =
            std::make_unique<DirectRpcCoordinator>(client, session, direct_hooks);
        auto preparation = dispatch->coordinator->prepare(
            core::TdDirectRequest{std::move(*dispatch->request)}, dispatch->authorization);
        dispatch->request.reset();
        if (std::holds_alternative<DirectTimedOut>(preparation)) {
            return stored_from_terminal(
                operation, timeout(operation, "preflight",
                                   request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (std::holds_alternative<DirectCancelled>(preparation)) {
            return WriteDispatchStopped{};
        }
        if (const auto* lost = std::get_if<DirectAuthorizationLost>(&preparation)) {
            return stored_from_terminal(operation,
                                        not_authed(account_value, lost->snapshot != nullptr
                                                                      ? lost->snapshot->data.state
                                                                      : core::AuthState::Unknown));
        }
        if (std::holds_alternative<DirectRejected>(preparation)) {
            return stored_from_terminal(operation, internal(operation));
        }
        const auto token = random_hex32();
        if (token.empty()) {
            throw std::runtime_error("cannot create dispatch token");
        }
        return WriteDispatchPreparation{
            .proof = {{"tdlib_function", plan.value()["tdlib_request"]},
                      {"dispatch_token", token},
                      {"client_generation", dispatch->authorization->client_generation}}};
    };
    hooks.dispatch = [dispatch, success = std::move(definition.success), operation,
                      &request](const write_contract::Plan& plan, const WriteDispatchPreparation&,
                                WriteDurableObservationSink&) -> WriteDispatchOutcome {
        if (!dispatch->authorization || !dispatch->coordinator) {
            throw std::logic_error("M6 direct dispatch state is incomplete");
        }
        auto selected = dispatch->coordinator->execute_prepared();
        return std::visit(
            [&](auto&& outcome) -> WriteDispatchOutcome {
                using Outcome = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<Outcome, DirectSuccess>) {
                    const auto* response = std::get_if<core::TdM6Response>(&outcome.result);
                    auto terminal_value =
                        response != nullptr ? success(plan, *response) : std::nullopt;
                    if (!terminal_value) {
                        return {.terminal = stored_from_terminal(operation, malformed(operation)),
                                .mutation_state = AccountAuditMutationState::Possible,
                                .mutation_confirmed = false};
                    }
                    return {.terminal = std::move(*terminal_value),
                            .mutation_state = AccountAuditMutationState::Confirmed,
                            .mutation_confirmed = true};
                } else if constexpr (std::is_same_v<Outcome, DirectTdError>) {
                    return {.terminal = stored_from_terminal(
                                operation, td_error_terminal(operation, outcome.error)),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectAuthorizationLost>) {
                    return {
                        .terminal = stored_from_terminal(
                            operation, not_authed(plan.account(), outcome.snapshot != nullptr
                                                                      ? outcome.snapshot->data.state
                                                                      : core::AuthState::Unknown)),
                        .mutation_state = audit_state(outcome.mutation_state),
                        .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectTimedOut>) {
                    return {
                        .terminal = stored_from_terminal(
                            operation,
                            timeout(operation, "dispatch",
                                    request.context.idempotency_key ? "pending" : "not_requested")),
                        .mutation_state = audit_state(outcome.mutation_state),
                        .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectCancelled>) {
                    return {.terminal = stored_from_terminal(operation, shutdown_terminal()),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else {
                    return {.terminal = stored_from_terminal(operation, malformed(operation)),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                }
            },
            std::move(selected));
    };
    hooks.timestamp = timestamp;
    hooks.audit_fatal_shutdown = [&session, &audit_fatal_shutdown] {
        session.audit_fatal();
        if (audit_fatal_shutdown) {
            audit_fatal_shutdown();
        }
    };
    const auto result = kernel.run(input, hooks);
    if (result.status == WriteKernelStatus::DryRunPlanned && result.plan) {
        session.result({{"dry_run", true}, {"plan", result.plan->value()}});
    } else if (result.terminal) {
        emit_terminal(session, *result.terminal);
        if (result.status == WriteKernelStatus::DurabilityFatal && audit_fatal_shutdown) {
            audit_fatal_shutdown();
        }
    }
}

using M6DeferredPlanner =
    std::function<std::optional<PreparedM6Mutation>(std::shared_ptr<PreparedSource>)>;
using M6SourceAdmission = std::function<std::variant<std::shared_ptr<PreparedSource>, json>()>;

void execute_prepared_m6( // NOLINT(readability-function-cognitive-complexity): closed transaction.
    core::TdClient& client, std::string_view account, config::Store& config_store,
    const std::shared_ptr<IdempotencyFoundation>& foundation,
    const std::function<void()>& audit_fatal_shutdown,
    const std::shared_ptr<const testing::WriteCoordinatorHooks>& coordinator_hooks,
    const proto::Request& request, RequestSession& session, AuthoritySource authority,
    proto::M6Operation operation, ResolverPrincipal principal, M6DeferredPlanner planner,
    const M6SourceAdmission& source_admission = {}) {
    auto state = std::make_shared<std::optional<PreparedM6Mutation>>();
    auto dispatch = std::make_shared<M6DispatchState>();
    M6WriteDefinition definition;
    definition.operation = operation;
    definition.principal = principal;
    definition.dispatch = dispatch;
    const auto* identity = proto::m6_operation_identity(operation);
    if (identity != nullptr && identity->tier == proto::M6Tier::Destructive) {
        definition.confirm = [&session, operation](const write_contract::Plan& plan, bool) {
            const auto rejected = [&] {
                return terminal("CONFIRMATION_REQUIRED", "destructive operation was not confirmed",
                                {{"account", plan.account()},
                                 {"action", plan.operation().name()},
                                 {"target", plan.value()}},
                                kDenied);
            };
            if (session.request().context.yes) {
                return WriteConfirmationOutcome{.status = WriteConfirmationStatus::ConfirmedYes,
                                                .terminal = std::nullopt};
            }
            if (!session.request().context.tty) {
                return WriteConfirmationOutcome{.status = WriteConfirmationStatus::Rejected,
                                                .terminal = rejected()};
            }
            auto answer = session.challenge(
                {proto::ChallengeKind::DestructiveConfirmation,
                 std::nullopt,
                 std::nullopt,
                 std::string(plan.operation().name()) + " " + plan.value().dump() + "? [y/N] ",
                 {{"action", plan.operation().name()}, {"target", plan.value()}},
                 false});
            const auto confirmed = answer.take_boolean();
            if (answer.status() == ChallengeStatus::Answered && confirmed.value_or(false)) {
                return WriteConfirmationOutcome{.status = WriteConfirmationStatus::ConfirmedTty,
                                                .terminal = std::nullopt};
            }
            if (answer.status() == ChallengeStatus::TimedOut) {
                return WriteConfirmationOutcome{.status = WriteConfirmationStatus::TimedOut,
                                                .terminal =
                                                    timeout(operation, "preflight", "not_created")};
            }
            if (answer.status() == ChallengeStatus::Cancelled ||
                answer.status() == ChallengeStatus::Disconnected ||
                answer.status() == ChallengeStatus::Shutdown) {
                return WriteConfirmationOutcome{.status = WriteConfirmationStatus::Cancelled,
                                                .terminal = rejected()};
            }
            return WriteConfirmationOutcome{.status = WriteConfirmationStatus::Rejected,
                                            .terminal = rejected()};
        };
    }
    definition.lookup_admit = [operation, principal, request_args = request.args, source_admission,
                               account_value =
                                   std::string(account)]() mutable -> WriteLookupAdmissionOutcome {
        std::shared_ptr<PreparedSource> source;
        json fingerprint_payload = request_args;
        if (source_admission) {
            auto admitted_source = source_admission();
            if (auto* failure = std::get_if<json>(&admitted_source)) {
                return std::move(*failure);
            }
            source = std::get<std::shared_ptr<PreparedSource>>(std::move(admitted_source));
            if (!source) {
                return internal(operation);
            }
            fingerprint_payload = {{"chat", request_args["chat"]},
                                   {"file", file_snapshot_json(source->snapshot())}};
        }
        auto fingerprint =
            m6_fingerprint(account_value, principal, operation, std::move(fingerprint_payload));
        if (!fingerprint) {
            return internal(operation);
        }
        return WriteLookupAdmission{.request_fingerprint = *fingerprint,
                                    .pass1_source = std::move(source)};
    };
    definition.materialize =
        [state, dispatch, planner = std::move(planner), principal, operation,
         account_value = std::string(account)](
            const WriteLookupAdmission& lookup) mutable -> WriteMaterializationOutcome {
        auto prepared = planner(lookup.pass1_source);
        if (!prepared) {
            return json();
        }
        if (prepared->operation != operation || prepared->principal != principal ||
            prepared->pass1_source != lookup.pass1_source) {
            return internal(operation);
        }
        std::string error;
        auto arguments = write_contract::make_arguments(operation, prepared->arguments, error);
        auto plan = write_contract::make_plan(operation, account_value, prepared->plan, error);
        if (!arguments || !plan) {
            return internal(operation);
        }
        dispatch->request = std::move(prepared->request);
        state->emplace(std::move(*prepared));
        return WriteMaterialization{.admission = {.arguments = std::move(*arguments),
                                                  .request_fingerprint = lookup.request_fingerprint,
                                                  .pass1_source = lookup.pass1_source,
                                                  .invite_redactions = {}},
                                    .plan = std::move(*plan)};
    };
    definition.success =
        [state](
            const write_contract::Plan& plan,
            const core::TdM6Response& response) -> std::optional<write_contract::StoredTerminal> {
        if (!*state || !state->value().result) {
            return std::nullopt;
        }
        auto materialized = state->value().result(response, plan.value());
        return materialized ? stored_result(state->value().operation, std::move(*materialized))
                            : std::nullopt;
    };
    definition.post_intent = [state, dispatch, foundation, coordinator_hooks, &session](
                                 const write_contract::Plan& plan, const WriteAdmission& admission,
                                 std::string_view invocation_id) -> WritePostIntentPreparation {
        if (!*state || !state->value().pass1_source) {
            return {};
        }
        if (!foundation || !admission.pass1_source ||
            state->value().operation != proto::M6Operation::ChatSetPhoto || !dispatch->request) {
            throw std::logic_error("M6 photo spool state is incomplete");
        }
        const FileSpoolControl control{session.deadline().expires_at, session.cancellation_token(),
                                       [&session] { return session.cancellation_requested(); }};
        const auto spool_hooks = coordinator_hooks
                                     ? coordinator_hooks->file_spool
                                     : std::shared_ptr<const testing::FileSpoolHooks>{};
        auto created =
            create_spool_file(*admission.pass1_source, foundation->state_directory(), invocation_id,
                              foundation->expected_uid(), control, spool_hooks);
        if (auto* failure = std::get_if<FileSpoolError>(&created)) {
            if (failure->cleanup_reference) {
                auto cleanup =
                    cleanup_spool_file(foundation->state_directory(), *failure->cleanup_reference,
                                       foundation->expected_uid(), {}, spool_hooks);
                if (std::holds_alternative<FileSpoolError>(cleanup)) {
                    throw std::runtime_error("M6 photo spool cleanup failed");
                }
            }
            json value = internal(state->value().operation);
            bool stopped = false;
            const auto source_path = plan.value()["file"].value("path", std::string{});
            switch (failure->kind) {
            case FileSpoolErrorKind::TimedOut:
                value = timeout(state->value().operation, "preflight", "removed");
                break;
            case FileSpoolErrorKind::Cancelled:
                value = shutdown_terminal();
                stopped = true;
                break;
            case FileSpoolErrorKind::InputChanged:
            case FileSpoolErrorKind::SourceUnavailable:
                value = terminal("INPUT_CHANGED", "input file changed while being read",
                                 {{"operation", operation_name(state->value().operation)},
                                  {"path", source_path}},
                                 kGeneric);
                break;
            case FileSpoolErrorKind::DurabilityFailure:
            case FileSpoolErrorKind::Contradiction:
                value = terminal("SPOOL_UNAVAILABLE", "attachment spool is unavailable",
                                 {{"operation", operation_name(state->value().operation)},
                                  {"path", source_path},
                                  {"reason", spool_reason_name(failure->durability_reason.value_or(
                                                 DurabilityReason::Contradiction))}},
                                 kGeneric);
                break;
            case FileSpoolErrorKind::InvalidInput:
                break;
            }
            return {.spool = std::nullopt,
                    .terminal_without_dispatch =
                        stored_from_terminal(state->value().operation, value),
                    .complete_without_mutation = false,
                    .stop_without_dispatch = stopped};
        }
        auto& spool = std::get<CreatedSpool>(created);
        auto* photo = std::get_if<core::TdM6SetChatPhotoRequest>(&*dispatch->request);
        if (photo == nullptr || photo->local_path) {
            throw std::logic_error("M6 photo request state is invalid");
        }
        photo->local_path = spool.local_path;
        return {.spool = std::move(spool.reference),
                .terminal_without_dispatch = std::nullopt,
                .complete_without_mutation = false,
                .stop_without_dispatch = false};
    };
    execute_m6_write(client, account, config_store, foundation, audit_fatal_shutdown,
                     coordinator_hooks, request, session, authority, std::move(definition));
}

json user_json(const UserIdentity& user) {
    return m6_user_identity_json(user);
}

std::optional<UserIdentity> resolve_user(ResolverConsumer& resolver, std::string selector,
                                         const ResolverCaller& caller, RequestSession& session) {
    if (classify_exact_write_selector(selector) != ExactWriteSelectorStatus::Exact) {
        session.error("USAGE", "mutation requires an exact user selector",
                      {{"argument", "user"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const auto outcome = resolver.resolve_user(std::move(selector));
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, caller);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    return std::get<UserIdentity>(outcome);
}

std::optional<ResolvedChatTarget> resolve_chat(ResolverConsumer& resolver, std::string selector,
                                               const ResolverCaller& caller,
                                               RequestSession& session,
                                               std::string argument = "chat") {
    if (classify_exact_write_selector(selector) != ExactWriteSelectorStatus::Exact) {
        session.error("USAGE", "mutation requires an exact chat selector",
                      {{"argument", std::move(argument)}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const auto outcome = resolver.resolve_exact_chat(std::move(selector), std::move(argument));
    if (const auto* error = std::get_if<ResolverError>(&outcome)) {
        emit_resolver_error(*error, session, caller);
        return std::nullopt;
    }
    if (std::holds_alternative<ResolverStop>(outcome)) {
        return std::nullopt;
    }
    return std::get<ResolvedChatTarget>(outcome);
}

bool m6_read_stopped(const ReadyReadResult& outcome, core::TdClient& client,
                     std::string_view account, const ResolverCaller& caller,
                     RequestSession& session) {
    switch (outcome.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        emit_resolver_error(
            ResolverError{ResolverNotAuthenticatedError{
                .account = std::string(account),
                .state = outcome.snapshot ? outcome.snapshot->data.state : core::AuthState::Unknown,
                .reason = ResolverNotAuthedReason::AuthorizationLost}},
            session, caller);
        return true;
    case ReadyReadStatus::TimedOut: {
        const auto current = client.auth_state();
        emit_resolver_error(
            ResolverError{ResolverTimeoutError{.operation = caller,
                                               .state = current ? std::optional{current->data.state}
                                                                : std::nullopt}},
            session, caller);
        return true;
    }
    case ReadyReadStatus::Cancelled:
        if (session.shutdown_requested()) {
            emit_terminal(session, shutdown_terminal());
        }
        return true;
    case ReadyReadStatus::Failed:
        emit_resolver_error(ResolverError{ResolverInternalError{.operation = caller}}, session,
                            caller);
        return true;
    }
    return true;
}

std::optional<core::TdM6Response>
planning_read(ResolverConsumer& resolver, core::TdClient& client, std::string_view account,
              proto::M6Operation operation, core::TdM6Request request, RequestSession& session) {
    const ResolverCaller caller{operation};
    const auto* member_request = std::get_if<core::TdM6GetChatMemberRequest>(&request);
    const bool member_call = member_request != nullptr;
    const auto member_chat_id = member_request != nullptr ? member_request->chat_id : 0;
    const auto member_user_id = member_request != nullptr ? member_request->user_id : 0;
    auto outcome = resolver.read_target(
        [&](const auto& current) { return client.m6_read(current, std::move(request)); });
    if (m6_read_stopped(outcome, client, account, caller, session)) {
        return std::nullopt;
    }
    if (const auto* error = outcome.value.get_if<core::TdError>()) {
        if (member_call && error->code == 400 && error->message == "Member not found") {
            session.error("NOT_FOUND", "member was not found",
                          {{"operation", operation_name(operation)},
                           {"chat_id", member_chat_id},
                           {"user_id", member_user_id}},
                          kNotFound);
            return std::nullopt;
        }
        emit_terminal(session, td_error_terminal(operation, *error));
        return std::nullopt;
    }
    const auto* response = outcome.value.get_if<core::TdM6Response>();
    if (response == nullptr) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    if (std::get_if<core::TdM6ConversionError>(response) != nullptr) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    return *response;
}

json arguments_from_plan(json plan) {
    plan.erase("operation");
    plan.erase("account");
    plan.erase("tdlib_request");
    return plan;
}

std::optional<core::TdM6FolderIcon> core_folder_icon(const json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto parsed = parse_m6_folder_icon(value.get_ref<const std::string&>());
    return parsed ? std::optional{static_cast<core::TdM6FolderIcon>(*parsed)} : std::nullopt;
}

void folder_not_found(RequestSession& session, proto::M6Operation operation,
                      std::int32_t folder_id) {
    session.error("NOT_FOUND", "folder was not found",
                  {{"operation", operation_name(operation)}, {"folder_id", folder_id}}, kNotFound);
}

void folder_precondition(RequestSession& session, proto::M6Operation operation,
                         std::int32_t folder_id, std::optional<std::int64_t> chat_id,
                         std::string_view reason) {
    session.error("PRECONDITION_FAILED", "folder mutation precondition failed",
                  {{"operation", operation_name(operation)},
                   {"folder_id", folder_id},
                   {"chat_id", chat_id ? json(*chat_id) : json(nullptr)},
                   {"reason", reason}},
                  kGeneric);
}

std::optional<PreparedM6Mutation> prepare_folder_create(const proto::Request& request,
                                                        ResolverConsumer& resolver,
                                                        const ResolverPrincipal& principal,
                                                        std::string_view account,
                                                        RequestSession& session) {
    constexpr auto operation = proto::M6Operation::FolderCreate;
    const ResolverCaller caller{operation};
    if (!exact_fields(request.args, {"name", "chats", "icon", "color_id"}) ||
        !request.args["name"].is_string() ||
        !valid_m6_canonical_text(M6TextKind::FolderName,
                                 request.args["name"].get_ref<const std::string&>()) ||
        !request.args["chats"].is_array() || request.args["chats"].empty() ||
        request.args["chats"].size() > 100 ||
        !(request.args["icon"].is_null() || core_folder_icon(request.args["icon"])) ||
        !request.args["color_id"].is_number_integer() ||
        request.args["color_id"].get<std::int64_t>() < -1 ||
        request.args["color_id"].get<std::int64_t>() > 6) {
        session.error("USAGE", "folder create received malformed arguments",
                      {{"argument", "folder"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    std::vector<std::int64_t> chat_ids;
    chat_ids.reserve(request.args["chats"].size());
    for (const auto& selector : request.args["chats"]) {
        if (!selector.is_string()) {
            session.error("USAGE", "folder chat selector must be a string",
                          {{"argument", "chat"}, {"reason", "invalid_argument"}}, kUsage);
            return std::nullopt;
        }
        auto target = resolve_chat(resolver, selector.get<std::string>(), caller, session);
        if (!target) {
            return std::nullopt;
        }
        if (!target->observed_chat || target->observed_chat->kind == core::TdChatKind::Secret ||
            target->observed_chat->kind == core::TdChatKind::Unknown) {
            session.error("USAGE", "chat does not support folders",
                          {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
            return std::nullopt;
        }
        chat_ids.push_back(target->chat.id);
    }
    std::ranges::sort(chat_ids);
    chat_ids.erase(std::unique(chat_ids.begin(), chat_ids.end()), chat_ids.end());
    if (chat_ids.empty() || chat_ids.size() > 100) {
        session.error("USAGE", "folder requires 1..100 distinct chats",
                      {{"argument", "--chat"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    core::TdM6ChatFolder folder;
    folder.name.text = request.args["name"].get<std::string>();
    folder.name.animate_custom_emoji = false;
    if (!request.args["icon"].is_null()) {
        folder.icon = core_folder_icon(request.args["icon"]);
    }
    folder.color_id = request.args["color_id"].get<std::int32_t>();
    folder.included_chat_ids = chat_ids;
    const json folder_name{{"text", folder.name.text},
                           {"animate_custom_emoji", false},
                           {"custom_emoji_entities", json::array()}};
    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"tdlib_request", "createChatFolder"},
              {"name", folder_name},
              {"icon", request.args["icon"]},
              {"color_id", folder.color_id},
              {"chat_ids", chat_ids}};
    return PreparedM6Mutation{
        .operation = operation,
        .principal = principal,
        .fingerprint_payload = request.args,
        .arguments = arguments_from_plan(plan),
        .plan = std::move(plan),
        .request = core::TdM6CreateChatFolderRequest{folder},
        .pass1_source = nullptr,
        .result = [folder = std::move(folder)](const core::TdM6Response& response,
                                               const json&) -> std::optional<json> {
            const auto* info = std::get_if<core::TdM6FolderInfo>(&response);
            if (info == nullptr || info->name != folder.name || info->color_id != folder.color_id ||
                info->is_shareable != folder.is_shareable || info->has_my_invite_links ||
                (folder.icon && info->icon != *folder.icon)) {
                return std::nullopt;
            }
            auto snapshot = m6_folder_snapshot_json(info->id, folder, *info);
            return snapshot ? std::optional<json>{{{"folder", std::move(*snapshot)}}}
                            : std::nullopt;
        }};
}

std::optional<PreparedM6Mutation>
prepare_existing_folder( // NOLINT(readability-function-cognitive-complexity): exact folder RMW.
    proto::M6Operation operation, const proto::Request& request, ResolverConsumer& resolver,
    const ResolverPrincipal& principal, core::TdClient& client, std::string_view account,
    RequestSession& session) {
    const bool membership = operation == proto::M6Operation::FolderAddChat ||
                            operation == proto::M6Operation::FolderRemoveChat;
    const bool edit = operation == proto::M6Operation::FolderEdit;
    const bool erase = operation == proto::M6Operation::FolderDelete;
    if ((!membership && !edit && !erase) || !request.args.is_object() ||
        !request.args.contains("folder_id") || !request.args["folder_id"].is_number_integer() ||
        request.args["folder_id"].get<std::int64_t>() < 1 ||
        request.args["folder_id"].get<std::int64_t>() > std::numeric_limits<std::int32_t>::max()) {
        session.error("USAGE", "folder mutation requires a positive folder id",
                      {{"argument", "folder_id"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const auto folder_id = request.args["folder_id"].get<std::int32_t>();
    const ResolverCaller caller{operation};
    std::optional<ResolvedChatTarget> chat;
    if (membership) {
        if (!exact_fields(request.args, {"folder_id", "chat"}) ||
            !request.args["chat"].is_string()) {
            session.error("USAGE", "folder membership mutation received malformed arguments",
                          {{"argument", "chat"}, {"reason", "invalid_argument"}}, kUsage);
            return std::nullopt;
        }
        chat = resolve_chat(resolver, request.args["chat"].get<std::string>(), caller, session);
        if (!chat) {
            return std::nullopt;
        }
        if (!chat->observed_chat || chat->observed_chat->kind == core::TdChatKind::Secret ||
            chat->observed_chat->kind == core::TdChatKind::Unknown) {
            session.error("USAGE", "chat does not support folders",
                          {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
            return std::nullopt;
        }
    } else if (erase && !exact_fields(request.args, {"folder_id"})) {
        session.error("USAGE", "folder delete received malformed arguments",
                      {{"argument", "folder_id"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    } else if (edit && !exact_fields(request.args, {"folder_id", "name", "icon", "use_default_icon",
                                                    "color_id"})) {
        session.error("USAGE", "folder edit received malformed arguments",
                      {{"argument", "folder"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }

    const auto folders =
        m6_wait_for_folders(client, resolver.bound_authorization(), caller, session);
    if (!folders) {
        return std::nullopt;
    }
    const auto info = std::ranges::find(folders->folders, folder_id, &core::TdM6FolderInfo::id);
    if (info == folders->folders.end()) {
        folder_not_found(session, operation, folder_id);
        return std::nullopt;
    }
    auto response = planning_read(resolver, client, account, operation,
                                  core::TdM6GetChatFolderRequest{folder_id}, session);
    if (!response) {
        return std::nullopt;
    }
    const auto* maybe = std::get_if<core::TdM6MaybeChatFolder>(&*response);
    if (maybe == nullptr) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    if (!maybe->folder) {
        folder_not_found(session, operation, folder_id);
        return std::nullopt;
    }
    auto before_json = m6_folder_snapshot_json(folder_id, *maybe->folder, *info);
    if (!before_json) {
        emit_terminal(session, internal(operation));
        return std::nullopt;
    }
    auto after = *maybe->folder;
    if (edit) {
        const auto valid_name =
            request.args["name"].is_null() ||
            (request.args["name"].is_string() &&
             valid_m6_canonical_text(M6TextKind::FolderName,
                                     request.args["name"].get_ref<const std::string&>()));
        const auto valid_icon =
            request.args["icon"].is_null() || core_folder_icon(request.args["icon"]).has_value();
        const auto valid_default = request.args["use_default_icon"].is_boolean();
        const auto valid_color = request.args["color_id"].is_null() ||
                                 (request.args["color_id"].is_number_integer() &&
                                  request.args["color_id"].get<std::int64_t>() >= -1 &&
                                  request.args["color_id"].get<std::int64_t>() <= 6);
        const bool has_change =
            !request.args["name"].is_null() || !request.args["icon"].is_null() ||
            request.args["use_default_icon"] == true || !request.args["color_id"].is_null();
        if (!valid_name || !valid_icon || !valid_default || !valid_color || !has_change ||
            (request.args["use_default_icon"] == true && !request.args["icon"].is_null())) {
            session.error("USAGE", "folder edit received malformed arguments",
                          {{"argument", "folder"}, {"reason", "invalid_argument"}}, kUsage);
            return std::nullopt;
        }
        if (!request.args["name"].is_null()) {
            after.name.text = request.args["name"].get<std::string>();
            after.name.custom_emoji_entities.clear();
        }
        if (request.args["use_default_icon"] == true) {
            after.icon.reset();
        } else if (!request.args["icon"].is_null()) {
            after.icon = core_folder_icon(request.args["icon"]);
        }
        if (!request.args["color_id"].is_null()) {
            after.color_id = request.args["color_id"].get<std::int32_t>();
        }
    } else if (membership) {
        const auto chat_id = chat->chat.id;
        const auto contains = [chat_id](const auto& ids) {
            return std::ranges::find(ids, chat_id) != ids.end();
        };
        if (operation == proto::M6Operation::FolderAddChat) {
            if (contains(after.pinned_chat_ids) || contains(after.included_chat_ids)) {
                folder_precondition(session, operation, folder_id, chat_id, "already_in_folder");
                return std::nullopt;
            }
            std::erase(after.excluded_chat_ids, chat_id);
            if (after.pinned_chat_ids.size() + after.included_chat_ids.size() >= 100) {
                folder_precondition(session, operation, folder_id, chat_id, "folder_capacity");
                return std::nullopt;
            }
            after.included_chat_ids.push_back(chat_id);
        } else {
            if (!contains(after.pinned_chat_ids) && !contains(after.included_chat_ids)) {
                folder_precondition(session, operation, folder_id, chat_id, "not_in_folder");
                return std::nullopt;
            }
            std::erase(after.pinned_chat_ids, chat_id);
            std::erase(after.included_chat_ids, chat_id);
            if (!contains(after.excluded_chat_ids)) {
                if (after.excluded_chat_ids.size() >= 100) {
                    folder_precondition(session, operation, folder_id, chat_id, "folder_capacity");
                    return std::nullopt;
                }
                after.excluded_chat_ids.push_back(chat_id);
            }
        }
    }
    json after_json;
    if (!erase) {
        auto snapshot = m6_folder_snapshot_json(folder_id, after, *info);
        if (!snapshot) {
            emit_terminal(session, malformed(operation));
            return std::nullopt;
        }
        after_json = std::move(*snapshot);
    }
    if (edit && after_json == *before_json) {
        folder_precondition(session, operation, folder_id, std::nullopt, "no_change");
        return std::nullopt;
    }

    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"tdlib_request", erase ? "deleteChatFolder" : "editChatFolder"}};
    core::TdM6Request mutation = core::TdM6DeleteChatFolderRequest{folder_id, {}};
    if (erase) {
        plan["folder"] = *before_json;
        plan["leave_chat_ids"] = json::array();
    } else if (edit) {
        plan["folder_id"] = folder_id;
        plan["before"] = *before_json;
        plan["after"] = after_json;
        mutation = core::TdM6EditChatFolderRequest{folder_id, after};
    } else {
        plan["folder_id"] = folder_id;
        plan["chat"] = chat_identity_json(chat->chat);
        plan["before"] = *before_json;
        plan["after"] = after_json;
        mutation = core::TdM6EditChatFolderRequest{folder_id, after};
    }
    return PreparedM6Mutation{
        .operation = operation,
        .principal = principal,
        .fingerprint_payload = request.args,
        .arguments = arguments_from_plan(plan),
        .plan = std::move(plan),
        .request = std::move(mutation),
        .pass1_source = nullptr,
        .result = [operation, folder_id,
                   after = std::move(after)](const core::TdM6Response& response,
                                             const json& plan_value) -> std::optional<json> {
            if (operation == proto::M6Operation::FolderDelete) {
                return std::get_if<core::TdM6Ok>(&response) != nullptr
                           ? std::optional<json>{{{"folder_id", folder_id}, {"deleted", true}}}
                           : std::nullopt;
            }
            const auto* returned = std::get_if<core::TdM6FolderInfo>(&response);
            if (returned == nullptr || returned->id != folder_id || returned->name != after.name ||
                returned->color_id != after.color_id ||
                returned->is_shareable != after.is_shareable ||
                (after.icon && returned->icon != *after.icon)) {
                return std::nullopt;
            }
            auto snapshot = m6_folder_snapshot_json(folder_id, after, *returned);
            if (!snapshot || *snapshot != plan_value["after"]) {
                return std::nullopt;
            }
            if (operation == proto::M6Operation::FolderEdit) {
                return json{{"folder", std::move(*snapshot)}};
            }
            return json{{"folder", std::move(*snapshot)},
                        {"chat", plan_value["chat"]},
                        {"included", operation == proto::M6Operation::FolderAddChat}};
        }};
}

std::optional<M6ChatKind> supergroup_kind(const ResolvedChatTarget& target) {
    if (!target.observed_chat || target.observed_chat->kind != core::TdChatKind::Supergroup ||
        target.observed_chat->related_id == 0 || !target.observed_supergroup ||
        target.observed_supergroup->id != target.observed_chat->related_id ||
        !target.observed_supergroup->is_forum) {
        return std::nullopt;
    }
    return target.observed_supergroup->is_channel ? M6ChatKind::Channel
                                                  : M6ChatKind::ForumSupergroup;
}

std::optional<core::TdM6MemberStatus>
current_member_status(ResolverConsumer& resolver, core::TdClient& client, std::string_view account,
                      proto::M6Operation operation, std::int64_t chat_id, std::int64_t principal_id,
                      const core::TdChat* chat, RequestSession& session) {
    auto response = planning_read(resolver, client, account, operation,
                                  core::TdM6GetChatMemberRequest{chat_id, principal_id}, session);
    if (!response) {
        return std::nullopt;
    }
    const auto* member = std::get_if<core::TdM6ChatMember>(&*response);
    if (member == nullptr || member->member.kind != core::TdM6SenderKind::User ||
        member->member.id != principal_id) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    auto status = member->status;
    if (status.kind == core::TdM6MemberStatusKind::Member) {
        if (chat == nullptr || !chat->permissions) {
            emit_terminal(session, internal(operation));
            return std::nullopt;
        }
        status.permissions = *chat->permissions;
    }
    return status;
}

std::optional<core::TdUserSummary> private_peer(const ResolvedChatTarget& target) {
    if (!target.private_user_id || !target.observed_user ||
        target.observed_user->id != *target.private_user_id) {
        return std::nullopt;
    }
    return target.observed_user;
}

std::optional<core::TdM6TopicColor> core_topic_color(const json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto parsed = parse_m6_topic_color(value.get_ref<const std::string&>());
    return parsed ? std::optional{static_cast<core::TdM6TopicColor>(*parsed)} : std::nullopt;
}

void topic_precondition(RequestSession& session, proto::M6Operation operation, std::int64_t chat_id,
                        std::optional<std::int32_t> topic_id, std::string_view reason) {
    session.error("PRECONDITION_FAILED", "topic mutation precondition failed",
                  {{"operation", operation_name(operation)},
                   {"chat_id", chat_id},
                   {"topic_id", topic_id ? json(*topic_id) : json(nullptr)},
                   {"reason", reason}},
                  kGeneric);
}

std::optional<PreparedM6Mutation>
prepare_topic_mutation( // NOLINT(readability-function-cognitive-complexity): closed topic matrix.
    proto::M6Operation operation, const proto::Request& request, ResolverConsumer& resolver,
    const ResolverPrincipal& principal, core::TdClient& client, std::string_view account,
    RequestSession& session) {
    const bool create = operation == proto::M6Operation::TopicCreate;
    const bool edit = operation == proto::M6Operation::TopicEdit;
    const bool toggle =
        operation == proto::M6Operation::TopicClose || operation == proto::M6Operation::TopicReopen;
    if ((!create && !edit && !toggle) || !request.args.is_object() ||
        !request.args.contains("chat") || !request.args["chat"].is_string() ||
        (create && (!exact_fields(request.args, {"chat", "name", "icon"}) ||
                    !request.args["name"].is_string() ||
                    !valid_m6_canonical_text(M6TextKind::TopicName,
                                             request.args["name"].get_ref<const std::string&>()) ||
                    !core_topic_color(request.args["icon"]))) ||
        (edit && (!exact_fields(request.args, {"chat", "topic_id", "name"}) ||
                  !request.args["name"].is_string() ||
                  !valid_m6_canonical_text(M6TextKind::TopicName,
                                           request.args["name"].get_ref<const std::string&>()))) ||
        (toggle && !exact_fields(request.args, {"chat", "topic_id"})) ||
        (!create && (!request.args["topic_id"].is_number_integer() ||
                     request.args["topic_id"].get<std::int64_t>() < 1 ||
                     request.args["topic_id"].get<std::int64_t>() >
                         std::numeric_limits<std::int32_t>::max()))) {
        session.error("USAGE", "topic mutation received malformed arguments",
                      {{"argument", "topic"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const ResolverCaller caller{operation};
    auto target = resolve_chat(resolver, request.args["chat"].get<std::string>(), caller, session);
    if (!target) {
        return std::nullopt;
    }
    bool private_bot = false;
    std::optional<M6ChatKind> kind;
    if (target->observed_chat && target->observed_chat->kind == core::TdChatKind::Private) {
        auto peer = private_peer(*target);
        if (!peer || !peer->is_bot || !peer->bot_has_topics ||
            (create && !peer->bot_allows_users_to_create_topics) || toggle) {
            session.error("USAGE", "chat does not support this topic operation",
                          {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
            return std::nullopt;
        }
        private_bot = true;
    } else {
        kind = supergroup_kind(*target);
        if (!kind || *kind != M6ChatKind::ForumSupergroup) {
            session.error("USAGE", "chat does not support this topic operation",
                          {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
            return std::nullopt;
        }
    }

    std::optional<core::TdM6ForumTopicInfo> before;
    if (!create) {
        const auto topic_id = request.args["topic_id"].get<std::int32_t>();
        auto response =
            planning_read(resolver, client, account, operation,
                          core::TdM6GetForumTopicRequest{target->chat.id, topic_id}, session);
        if (!response) {
            return std::nullopt;
        }
        const auto* maybe = std::get_if<core::TdM6MaybeForumTopic>(&*response);
        if (maybe == nullptr) {
            emit_terminal(session, internal(operation));
            return std::nullopt;
        }
        if (!maybe->topic) {
            session.error("NOT_FOUND", "topic was not found",
                          {{"operation", operation_name(operation)},
                           {"chat_id", target->chat.id},
                           {"topic_id", topic_id}},
                          kNotFound);
            return std::nullopt;
        }
        if (!m6_topic_info_json(maybe->topic->info)) {
            emit_terminal(session, internal(operation));
            return std::nullopt;
        }
        before = maybe->topic->info;
    }
    if (!private_bot) {
        auto caller_status =
            current_member_status(resolver, client, account, operation, target->chat.id,
                                  principal.id, &*target->observed_chat, session);
        const auto creator = before && before->creator.kind == core::TdM6SenderKind::User
                                 ? std::optional{before->creator.id}
                                 : std::nullopt;
        if (!caller_status ||
            !m6_topic_mutation_allowed(operation, *kind, *caller_status, principal.id, creator)) {
            topic_precondition(session, operation, target->chat.id,
                               before ? std::optional{before->id} : std::nullopt, "missing_right");
            return std::nullopt;
        }
    }

    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"chat", chat_identity_json(target->chat)}};
    core::TdM6Request mutation = core::TdM6CreateForumTopicRequest{};
    if (create) {
        const auto name = request.args["name"].get<std::string>();
        const auto icon = *core_topic_color(request.args["icon"]);
        plan["tdlib_request"] = "createForumTopic";
        plan["name"] = name;
        plan["icon"] = request.args["icon"];
        mutation =
            core::TdM6CreateForumTopicRequest{.chat_id = target->chat.id,
                                              .name = name,
                                              .icon = {.color = icon, .custom_emoji_id = "0"},
                                              .is_name_implicit = false};
    } else {
        auto before_json = m6_topic_info_json(*before);
        if (!before_json) {
            emit_terminal(session, internal(operation));
            return std::nullopt;
        }
        plan["before"] = *before_json;
        if (edit) {
            const auto name = request.args["name"].get<std::string>();
            if (name == before->name) {
                topic_precondition(session, operation, target->chat.id, before->id, "no_change");
                return std::nullopt;
            }
            plan["tdlib_request"] = "editForumTopic";
            plan["name"] = name;
            mutation = core::TdM6EditForumTopicRequest{.chat_id = target->chat.id,
                                                       .topic_id = before->id,
                                                       .name = name,
                                                       .edit_icon_custom_emoji = false,
                                                       .icon_custom_emoji_id = 0};
        } else {
            const bool closed = operation == proto::M6Operation::TopicClose;
            if (before->is_closed == closed) {
                topic_precondition(session, operation, target->chat.id, before->id,
                                   closed ? "already_closed" : "already_open");
                return std::nullopt;
            }
            plan["tdlib_request"] = "toggleForumTopicIsClosed";
            plan["closed"] = closed;
            mutation = core::TdM6ToggleForumTopicRequest{
                .chat_id = target->chat.id, .topic_id = before->id, .is_closed = closed};
        }
    }
    return PreparedM6Mutation{
        .operation = operation,
        .principal = principal,
        .fingerprint_payload = request.args,
        .arguments = arguments_from_plan(plan),
        .plan = std::move(plan),
        .request = std::move(mutation),
        .pass1_source = nullptr,
        .result = [operation](const core::TdM6Response& response,
                              const json& plan_value) -> std::optional<json> {
            if (operation == proto::M6Operation::TopicCreate) {
                const auto* topic = std::get_if<core::TdM6ForumTopicInfo>(&response);
                auto converted = topic != nullptr ? m6_topic_info_json(*topic) : std::nullopt;
                return converted && (*converted)["chat_id"] == plan_value["chat"]["id"] &&
                               (*converted)["name"] == plan_value["name"]
                           ? std::optional<json>{{{"topic", std::move(*converted)}}}
                           : std::nullopt;
            }
            if (std::get_if<core::TdM6Ok>(&response) == nullptr) {
                return std::nullopt;
            }
            if (operation == proto::M6Operation::TopicEdit) {
                return json{{"chat", plan_value["chat"]},
                            {"topic_id", plan_value["before"]["id"]},
                            {"name", plan_value["name"]}};
            }
            return json{{"chat", plan_value["chat"]},
                        {"topic_id", plan_value["before"]["id"]},
                        {"closed", plan_value["closed"]}};
        }};
}

std::optional<PreparedM6Mutation> prepare_storage_optimize(const proto::Request& request,
                                                           const ResolverPrincipal& principal,
                                                           std::string_view account,
                                                           RequestSession& session) {
    constexpr auto operation = proto::M6Operation::StorageOptimize;
    if (!exact_fields(request.args, {})) {
        session.error("USAGE", "storage optimize does not accept arguments",
                      {{"argument", "storage"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"tdlib_request", "optimizeStorage"},
              {"size", -1},
              {"ttl", -1},
              {"count", -1},
              {"immunity_delay", -1},
              {"file_types", json::array()},
              {"chat_ids", json::array()},
              {"exclude_chat_ids", json::array()},
              {"return_deleted_file_statistics", false},
              {"chat_limit", 100}};
    return PreparedM6Mutation{
        .operation = operation,
        .principal = principal,
        .fingerprint_payload = request.args,
        .arguments = arguments_from_plan(plan),
        .plan = std::move(plan),
        .request = core::TdM6OptimizeStorageRequest{},
        .pass1_source = nullptr,
        .result = [](const core::TdM6Response& response, const json&) -> std::optional<json> {
            const auto* statistics = std::get_if<core::TdM6StorageStatistics>(&response);
            auto converted =
                statistics != nullptr ? m6_storage_statistics_json(*statistics) : std::nullopt;
            return converted ? std::optional<json>{{{"optimized", true},
                                                    {"statistics", std::move(*converted)}}}
                             : std::nullopt;
        }};
}

std::optional<M6ChatKind> admin_chat_kind(const ResolvedChatTarget& target) {
    if (!target.observed_chat) {
        return std::nullopt;
    }
    if (target.observed_chat->kind == core::TdChatKind::BasicGroup) {
        return M6ChatKind::BasicGroup;
    }
    if ((target.observed_chat->kind != core::TdChatKind::Supergroup &&
         target.observed_chat->kind != core::TdChatKind::Channel) ||
        !target.observed_supergroup ||
        target.observed_supergroup->id != target.observed_chat->related_id) {
        return std::nullopt;
    }
    if (target.observed_supergroup->is_channel) {
        return M6ChatKind::Channel;
    }
    return target.observed_supergroup->is_forum ? M6ChatKind::ForumSupergroup
                                                : M6ChatKind::NonForumSupergroup;
}

void set_right(core::TdM6AdminRights& rights, M6AdminRight right) {
    switch (right) {
    case M6AdminRight::ChangeInfo:
        rights.can_change_info = true;
        break;
    case M6AdminRight::PostMessages:
        rights.can_post_messages = true;
        break;
    case M6AdminRight::EditMessages:
        rights.can_edit_messages = true;
        break;
    case M6AdminRight::DeleteMessages:
        rights.can_delete_messages = true;
        break;
    case M6AdminRight::InviteUsers:
        rights.can_invite_users = true;
        break;
    case M6AdminRight::RestrictMembers:
        rights.can_restrict_members = true;
        break;
    case M6AdminRight::PinMessages:
        rights.can_pin_messages = true;
        break;
    case M6AdminRight::ManageTopics:
        rights.can_manage_topics = true;
        break;
    case M6AdminRight::PromoteMembers:
        rights.can_promote_members = true;
        break;
    case M6AdminRight::ManageVideoChats:
        rights.can_manage_video_chats = true;
        break;
    case M6AdminRight::PostStories:
        rights.can_post_stories = true;
        break;
    case M6AdminRight::EditStories:
        rights.can_edit_stories = true;
        break;
    case M6AdminRight::DeleteStories:
        rights.can_delete_stories = true;
        break;
    case M6AdminRight::ManageDirectMessages:
        rights.can_manage_direct_messages = true;
        break;
    case M6AdminRight::ManageTags:
        rights.can_manage_tags = true;
        break;
    case M6AdminRight::Anonymous:
        rights.is_anonymous = true;
        break;
    }
}

void set_permission(core::TdM6ChatPermissions& permissions, M6ChatPermission permission) {
    switch (permission) {
    case M6ChatPermission::SendBasicMessages:
        permissions.can_send_basic_messages = true;
        break;
    case M6ChatPermission::SendAudios:
        permissions.can_send_audios = true;
        break;
    case M6ChatPermission::SendDocuments:
        permissions.can_send_documents = true;
        break;
    case M6ChatPermission::SendPhotos:
        permissions.can_send_photos = true;
        break;
    case M6ChatPermission::SendVideos:
        permissions.can_send_videos = true;
        break;
    case M6ChatPermission::SendVideoNotes:
        permissions.can_send_video_notes = true;
        break;
    case M6ChatPermission::SendVoiceNotes:
        permissions.can_send_voice_notes = true;
        break;
    case M6ChatPermission::SendPolls:
        permissions.can_send_polls = true;
        break;
    case M6ChatPermission::SendOtherMessages:
        permissions.can_send_other_messages = true;
        break;
    case M6ChatPermission::AddLinkPreviews:
        permissions.can_add_link_previews = true;
        break;
    case M6ChatPermission::ReactToMessages:
        permissions.can_react_to_messages = true;
        break;
    case M6ChatPermission::EditTag:
        permissions.can_edit_tag = true;
        break;
    case M6ChatPermission::ChangeInfo:
        permissions.can_change_info = true;
        break;
    case M6ChatPermission::InviteUsers:
        permissions.can_invite_users = true;
        break;
    case M6ChatPermission::PinMessages:
        permissions.can_pin_messages = true;
        break;
    case M6ChatPermission::CreateTopics:
        permissions.can_create_topics = true;
        break;
    }
}

template <typename Enum, typename Parser>
std::optional<std::vector<Enum>> parse_closed_array(const json& value, Parser parser,
                                                    bool require_nonempty) {
    if (!value.is_array() || (require_nonempty && value.empty())) {
        return std::nullopt;
    }
    std::vector<Enum> result;
    std::optional<Enum> previous;
    for (const auto& item : value) {
        if (!item.is_string()) {
            return std::nullopt;
        }
        const auto parsed = parser(item.get_ref<const std::string&>());
        if (!parsed || (previous &&
                        static_cast<std::size_t>(*parsed) <= static_cast<std::size_t>(*previous))) {
            return std::nullopt;
        }
        result.push_back(*parsed);
        previous = parsed;
    }
    return result;
}

void admin_precondition(RequestSession& session, proto::M6Operation operation, std::int64_t chat_id,
                        std::string_view right) {
    session.error("PRECONDITION_FAILED", "chat administration right is missing",
                  {{"operation", operation_name(operation)},
                   {"chat_id", chat_id},
                   {"reason", "missing_right"},
                   {"right", right}},
                  kGeneric);
}

std::string_view required_admin_right(proto::M6Operation operation) {
    switch (operation) {
    case proto::M6Operation::ChatSetTitle:
    case proto::M6Operation::ChatSetPhoto:
    case proto::M6Operation::ChatSetDescription:
        return "change-info";
    case proto::M6Operation::ChatInviteLink:
        return "invite-users";
    case proto::M6Operation::ChatPromote:
    case proto::M6Operation::ChatDemote:
        return "promote-members";
    case proto::M6Operation::ChatBan:
    case proto::M6Operation::ChatUnban:
    case proto::M6Operation::ChatKick:
    case proto::M6Operation::ChatSetPermissions:
        return "restrict-members";
    default:
        return {};
    }
}

std::optional<PreparedM6Mutation>
prepare_chat_admin( // NOLINT(readability-function-cognitive-complexity): closed rights matrix.
    proto::M6Operation operation, const proto::Request& request, ResolverConsumer& resolver,
    const ResolverPrincipal& principal, core::TdClient& client, std::string_view account,
    RequestSession& session) {
    const bool simple = operation == proto::M6Operation::ChatSetTitle ||
                        operation == proto::M6Operation::ChatSetDescription;
    const bool member_operation =
        operation == proto::M6Operation::ChatPromote ||
        operation == proto::M6Operation::ChatDemote || operation == proto::M6Operation::ChatBan ||
        operation == proto::M6Operation::ChatUnban || operation == proto::M6Operation::ChatKick;
    const bool permissions = operation == proto::M6Operation::ChatSetPermissions;
    if ((!simple && !member_operation && !permissions) || !request.args.is_object() ||
        !request.args.contains("chat") || !request.args["chat"].is_string()) {
        session.error("USAGE", "chat administration command received malformed arguments",
                      {{"argument", "chat"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    if ((operation == proto::M6Operation::ChatSetTitle &&
         (!exact_fields(request.args, {"chat", "title"}) || !request.args["title"].is_string() ||
          !valid_m6_canonical_text(M6TextKind::ChatTitle,
                                   request.args["title"].get_ref<const std::string&>()))) ||
        (operation == proto::M6Operation::ChatSetDescription &&
         (!exact_fields(request.args, {"chat", "description"}) ||
          !request.args["description"].is_string() ||
          !valid_m6_canonical_text(M6TextKind::ChatDescription,
                                   request.args["description"].get_ref<const std::string&>()))) ||
        (member_operation && (!request.args.contains("user") || !request.args["user"].is_string() ||
                              (operation == proto::M6Operation::ChatPromote
                                   ? !exact_fields(request.args, {"chat", "user", "rights"})
                                   : !exact_fields(request.args, {"chat", "user"})))) ||
        (permissions && !exact_fields(request.args, {"chat", "permissions"}))) {
        session.error("USAGE", "chat administration command received malformed arguments",
                      {{"argument", "chat"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const ResolverCaller caller{operation};
    auto target = resolve_chat(resolver, request.args["chat"].get<std::string>(), caller, session);
    if (!target) {
        return std::nullopt;
    }
    if (!target->observed_chat) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    const auto* observed_chat = &*target->observed_chat;
    const auto kind = admin_chat_kind(*target);
    if (!kind || (permissions && *kind == M6ChatKind::Channel) ||
        (operation == proto::M6Operation::ChatPromote && *kind == M6ChatKind::BasicGroup)) {
        session.error("USAGE", "chat type does not support this administration operation",
                      {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
        return std::nullopt;
    }
    auto caller_status =
        current_member_status(resolver, client, account, operation, target->chat.id, principal.id,
                              observed_chat, session);
    if (!caller_status || !valid_m6_member_status(*caller_status, *kind)) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }

    std::vector<M6AdminRight> requested_rights;
    std::vector<M6ChatPermission> requested_permissions;
    if (operation == proto::M6Operation::ChatPromote) {
        auto parsed =
            parse_closed_array<M6AdminRight>(request.args["rights"], parse_m6_admin_right, true);
        if (!parsed) {
            session.error("USAGE", "promote requires ordered distinct rights",
                          {{"argument", "--rights"}, {"reason", "invalid_argument"}}, kUsage);
            return std::nullopt;
        }
        requested_rights = std::move(*parsed);
    }
    if (permissions) {
        auto parsed = parse_closed_array<M6ChatPermission>(request.args["permissions"],
                                                           parse_m6_chat_permission, false);
        if (!parsed) {
            session.error("USAGE", "permissions must be ordered and distinct",
                          {{"argument", "--permissions"}, {"reason", "invalid_argument"}}, kUsage);
            return std::nullopt;
        }
        requested_permissions = std::move(*parsed);
    }
    if (m6_authorize_caller(operation, *kind, *caller_status, requested_rights) !=
        M6CapabilityStatus::Allowed) {
        admin_precondition(session, operation, target->chat.id, required_admin_right(operation));
        return std::nullopt;
    }

    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"chat", chat_identity_json(target->chat)}};
    core::TdM6Request mutation = core::TdM6SetChatTitleRequest{};
    if (operation == proto::M6Operation::ChatSetTitle) {
        plan["tdlib_request"] = "setChatTitle";
        plan["title"] = request.args["title"];
        mutation = core::TdM6SetChatTitleRequest{target->chat.id,
                                                 request.args["title"].get<std::string>()};
    } else if (operation == proto::M6Operation::ChatSetDescription) {
        plan["tdlib_request"] = "setChatDescription";
        plan["description"] = request.args["description"];
        mutation = core::TdM6SetChatDescriptionRequest{
            target->chat.id, request.args["description"].get<std::string>()};
    } else if (permissions) {
        core::TdM6ChatPermissions converted_permissions;
        for (const auto permission : requested_permissions) {
            set_permission(converted_permissions, permission);
        }
        plan["tdlib_request"] = "setChatPermissions";
        plan["permissions"] = request.args["permissions"];
        mutation = core::TdM6SetChatPermissionsRequest{target->chat.id, converted_permissions};
    } else {
        if (classify_exact_write_selector(request.args["user"].get_ref<const std::string&>()) !=
            ExactWriteSelectorStatus::Exact) {
            session.error("USAGE", "member mutation requires an exact user selector",
                          {{"argument", "user"}, {"reason", "invalid_argument"}}, kUsage);
            return std::nullopt;
        }
        const auto user_outcome =
            resolver.resolve_user(request.args["user"].get<std::string>(), target->observed_chat);
        if (const auto* error = std::get_if<ResolverError>(&user_outcome)) {
            emit_resolver_error(*error, session, caller);
            return std::nullopt;
        }
        if (std::holds_alternative<ResolverStop>(user_outcome)) {
            return std::nullopt;
        }
        const auto user = std::get<UserIdentity>(user_outcome);
        auto member_response =
            planning_read(resolver, client, account, operation,
                          core::TdM6GetChatMemberRequest{target->chat.id, user.id}, session);
        if (!member_response) {
            return std::nullopt;
        }
        const auto* member = std::get_if<core::TdM6ChatMember>(&*member_response);
        if (member == nullptr || member->member.kind != core::TdM6SenderKind::User ||
            member->member.id != user.id || !valid_m6_member_status(member->status, *kind)) {
            emit_terminal(session, malformed(operation));
            return std::nullopt;
        }
        const auto target_decision =
            m6_authorize_target(operation, principal.id, user.id, member->status);
        if (target_decision != M6CapabilityStatus::Allowed) {
            std::string_view reason = "wrong_member_state";
            if (user.id == principal.id) {
                reason = "self_target";
            } else if (member->status.kind == core::TdM6MemberStatusKind::Creator) {
                reason = "creator";
            } else if (member->status.kind == core::TdM6MemberStatusKind::Administrator &&
                       !member->status.can_be_edited) {
                reason = "noneditable_administrator";
            }
            session.error("PRECONDITION_FAILED", "member mutation precondition failed",
                          {{"operation", operation_name(operation)},
                           {"chat_id", target->chat.id},
                           {"user_id", user.id},
                           {"reason", reason}},
                          kGeneric);
            return std::nullopt;
        }
        auto before = m6_member_status_json(member->status);
        if (!before) {
            emit_terminal(session, internal(operation));
            return std::nullopt;
        }
        core::TdM6MemberStatus after;
        std::string_view after_name;
        if (operation == proto::M6Operation::ChatPromote) {
            after.kind = core::TdM6MemberStatusKind::Administrator;
            after.can_be_edited = true;
            after.rights.can_manage_chat = true;
            for (const auto right : requested_rights) {
                set_right(after.rights, right);
            }
            after_name = "administrator";
        } else if (operation == proto::M6Operation::ChatDemote) {
            after.kind = core::TdM6MemberStatusKind::Member;
            after_name = "member";
        } else if (operation == proto::M6Operation::ChatBan) {
            after.kind = core::TdM6MemberStatusKind::Banned;
            after_name = "banned";
        } else {
            after.kind = core::TdM6MemberStatusKind::Left;
            after_name = "left";
        }
        plan["tdlib_request"] = "setChatMemberStatus";
        plan["user"] = user_json(user);
        plan["before"] = *before;
        if (operation == proto::M6Operation::ChatPromote) {
            plan["can_manage_chat"] = true;
            plan["rights"] = request.args["rights"];
        } else {
            plan["after"] = after_name;
        }
        mutation = core::TdM6SetChatMemberStatusRequest{target->chat.id, user.id, after};
    }
    return PreparedM6Mutation{.operation = operation,
                              .principal = principal,
                              .fingerprint_payload = request.args,
                              .arguments = arguments_from_plan(plan),
                              .plan = std::move(plan),
                              .request = std::move(mutation),
                              .pass1_source = nullptr,
                              .result = [operation](const core::TdM6Response& response,
                                                    const json& plan_value) -> std::optional<json> {
                                  if (std::get_if<core::TdM6Ok>(&response) == nullptr) {
                                      return std::nullopt;
                                  }
                                  if (operation == proto::M6Operation::ChatSetTitle) {
                                      return json{{"chat", plan_value["chat"]},
                                                  {"title", plan_value["title"]}};
                                  }
                                  if (operation == proto::M6Operation::ChatSetDescription) {
                                      return json{{"chat", plan_value["chat"]},
                                                  {"description", plan_value["description"]}};
                                  }
                                  if (operation == proto::M6Operation::ChatSetPermissions) {
                                      return json{{"chat", plan_value["chat"]},
                                                  {"permissions", plan_value["permissions"]}};
                                  }
                                  if (operation == proto::M6Operation::ChatPromote) {
                                      return json{{"chat", plan_value["chat"]},
                                                  {"user", plan_value["user"]},
                                                  {"status", "administrator"},
                                                  {"can_manage_chat", true},
                                                  {"rights", plan_value["rights"]}};
                                  }
                                  return json{{"chat", plan_value["chat"]},
                                              {"user", plan_value["user"]},
                                              {"status", plan_value["after"]}};
                              }};
}

std::optional<PreparedM6Mutation>
prepare_invite_link( // NOLINT(readability-function-cognitive-complexity): secret create/revoke.
    const proto::Request& request, ResolverConsumer& resolver, const ResolverPrincipal& principal,
    core::TdClient& client, std::string_view account, RequestSession& session) {
    constexpr auto operation = proto::M6Operation::ChatInviteLink;
    if (!exact_fields(request.args, {"chat", "revoke"}) || !request.args["chat"].is_string() ||
        !(request.args["revoke"].is_null() ||
          (request.args["revoke"].is_string() &&
           valid_m6_invite_link(request.args["revoke"].get_ref<const std::string&>())))) {
        session.error("USAGE", "invite-link received malformed arguments",
                      {{"argument", "invite-link"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const ResolverCaller caller{operation};
    auto target = resolve_chat(resolver, request.args["chat"].get<std::string>(), caller, session);
    if (!target) {
        return std::nullopt;
    }
    if (!target->observed_chat) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    const auto* observed_chat = &*target->observed_chat;
    const auto kind = admin_chat_kind(*target);
    if (!kind) {
        session.error("USAGE", "chat type does not support invite links",
                      {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
        return std::nullopt;
    }
    auto caller_status =
        current_member_status(resolver, client, account, operation, target->chat.id, principal.id,
                              observed_chat, session);
    if (!caller_status || !valid_m6_member_status(*caller_status, *kind)) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    if (m6_authorize_caller(operation, *kind, *caller_status) != M6CapabilityStatus::Allowed) {
        admin_precondition(session, operation, target->chat.id, "invite-users");
        return std::nullopt;
    }
    const bool revoke = !request.args["revoke"].is_null();
    const auto secret = std::make_shared<secure::SensitiveString>(
        revoke ? request.args["revoke"].get_ref<const std::string&>() : std::string_view{},
        request.wipe_observer(), "m6_invite_link");
    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"tdlib_request", revoke ? "revokeChatInviteLink" : "createChatInviteLink"},
              {"chat", chat_identity_json(target->chat)},
              {"action", revoke ? "revoke" : "create"},
              {"invite_link_sha256", revoke ? json(common::domain_separated_sha256(
                                                  "tgcli.m6.invite-link.v1", secret->view()))
                                            : json(nullptr)}};
    json fingerprint_payload{{"chat", request.args["chat"]},
                             {"revoke_sha256", plan["invite_link_sha256"]}};
    core::TdM6Request mutation =
        revoke ? core::TdM6Request{core::TdM6RevokeChatInviteLinkRequest{target->chat.id,
                                                                         secret->value()}}
               : core::TdM6Request{core::TdM6CreateChatInviteLinkRequest{target->chat.id}};
    return PreparedM6Mutation{
        .operation = operation,
        .principal = principal,
        .fingerprint_payload = std::move(fingerprint_payload),
        .arguments = arguments_from_plan(plan),
        .plan = std::move(plan),
        .request = std::move(mutation),
        .pass1_source = nullptr,
        .result = [secret,
                   principal_id = principal.id](const core::TdM6Response& response,
                                                const json& plan_value) -> std::optional<json> {
            if (plan_value["action"] == "create") {
                const auto* link = std::get_if<core::TdM6ChatInviteLink>(&response);
                auto converted = link != nullptr ? m6_invite_link_json(*link) : std::nullopt;
                if (!converted || link->is_primary || link->is_revoked ||
                    link->creator_user_id != principal_id || !link->name.empty() ||
                    link->edit_date != 0 || link->expiration_date != 0 || link->member_limit != 0 ||
                    link->member_count != 0 || link->expired_member_count != 0 ||
                    link->pending_join_request_count != 0 || link->creates_join_request ||
                    link->subscription_pricing) {
                    return std::nullopt;
                }
                return json{{"chat", plan_value["chat"]},
                            {"action", "create"},
                            {"invite_link", link->invite_link}};
            }
            const auto* links = std::get_if<core::TdM6ChatInviteLinks>(&response);
            if (links == nullptr ||
                links->total_count != static_cast<std::int32_t>(links->invite_links.size()) ||
                (links->invite_links.size() != 1 && links->invite_links.size() != 2) ||
                links->invite_links.front().invite_link != secret->view() ||
                !links->invite_links.front().is_revoked ||
                !m6_invite_link_json(links->invite_links.front())) {
                return std::nullopt;
            }
            const auto& revoked = links->invite_links.front();
            json replacement = nullptr;
            if (revoked.is_primary) {
                if (links->invite_links.size() != 2) {
                    return std::nullopt;
                }
                const auto& active = links->invite_links.back();
                if (!active.is_primary || active.is_revoked ||
                    active.invite_link == revoked.invite_link ||
                    active.creator_user_id != revoked.creator_user_id ||
                    !m6_invite_link_json(active)) {
                    return std::nullopt;
                }
                replacement = active.invite_link;
            } else if (links->invite_links.size() != 1) {
                return std::nullopt;
            }
            return json{{"chat", plan_value["chat"]},
                        {"action", "revoke"},
                        {"invite_link", std::move(replacement)}};
        }};
}

std::variant<std::shared_ptr<PreparedSource>, json>
admit_static_photo_source(const proto::Request& request, RequestSession& session,
                          const std::shared_ptr<const testing::WriteCoordinatorHooks>& hooks) {
    constexpr auto operation = proto::M6Operation::ChatSetPhoto;
    if (!exact_fields(request.args, {"chat", "path"}) || !request.args["chat"].is_string() ||
        !request.args["path"].is_string() ||
        request.args["path"].get_ref<const std::string&>().empty()) {
        return terminal("USAGE", "set-photo path received malformed arguments",
                        {{"argument", "photo"}, {"reason", "invalid_argument"}}, kUsage);
    }
    const FileSpoolControl control{session.deadline().expires_at, session.cancellation_token(),
                                   [&session] { return session.cancellation_requested(); }};
    auto prepared = prepare_spool_source(
        request.args["path"].get_ref<const std::string&>(), request.context.cwd, control,
        hooks ? hooks->file_spool : std::shared_ptr<const testing::FileSpoolHooks>{},
        SourceContentPolicy::StaticJpeg);
    if (auto* failure = std::get_if<FileSpoolError>(&prepared)) {
        const auto display_path = canonical_source_display_path(
            request.args["path"].get_ref<const std::string&>(), request.context.cwd);
        switch (failure->kind) {
        case FileSpoolErrorKind::TimedOut:
            return timeout(operation, "preflight", "not_created");
        case FileSpoolErrorKind::Cancelled:
            return json();
        case FileSpoolErrorKind::SourceUnavailable:
            return terminal("NOT_FOUND", "input file is unavailable",
                            {{"operation", operation_name(operation)},
                             {"path", display_path.value_or(std::string{})},
                             {"reason", source_reason_name(failure->source_reason.value_or(
                                            SourceFileReason::Unreadable))}},
                            kNotFound);
        case FileSpoolErrorKind::InputChanged:
            return terminal("INPUT_CHANGED", "input file changed while being read",
                            {{"operation", operation_name(operation)},
                             {"path", display_path.value_or(std::string{})}},
                            kGeneric);
        case FileSpoolErrorKind::DurabilityFailure:
        case FileSpoolErrorKind::Contradiction:
            return terminal("SPOOL_UNAVAILABLE", "attachment spool is unavailable",
                            {{"operation", operation_name(operation)},
                             {"path", display_path.value_or(std::string{})},
                             {"reason", spool_reason_name(failure->durability_reason.value_or(
                                            DurabilityReason::Contradiction))}},
                            kGeneric);
        case FileSpoolErrorKind::InvalidInput:
            return terminal("USAGE", "set-photo requires a static JPEG source",
                            {{"argument", "photo"}, {"reason", "invalid_argument"}}, kUsage);
        }
    }
    return std::make_shared<PreparedSource>(std::get<PreparedSource>(std::move(prepared)));
}

std::optional<PreparedM6Mutation>
prepare_static_photo(const proto::Request& request, ResolverConsumer& resolver,
                     const ResolverPrincipal& principal, core::TdClient& client,
                     std::string_view account, RequestSession& session,
                     std::shared_ptr<PreparedSource> source) {
    constexpr auto operation = proto::M6Operation::ChatSetPhoto;
    if (!exact_fields(request.args, {"chat", "path"}) || !request.args["chat"].is_string() ||
        !request.args["path"].is_string() ||
        request.args["path"].get_ref<const std::string&>().empty()) {
        session.error("USAGE", "set-photo path received malformed arguments",
                      {{"argument", "photo"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const ResolverCaller caller{operation};
    auto target = resolve_chat(resolver, request.args["chat"].get<std::string>(), caller, session);
    if (!target) {
        return std::nullopt;
    }
    if (!target->observed_chat) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    const auto* observed_chat = &*target->observed_chat;
    const auto kind = admin_chat_kind(*target);
    if (!kind) {
        session.error("USAGE", "chat does not support this administration operation",
                      {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
        return std::nullopt;
    }
    auto caller_status =
        current_member_status(resolver, client, account, operation, target->chat.id, principal.id,
                              observed_chat, session);
    if (!caller_status || !valid_m6_member_status(*caller_status, *kind)) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    if (m6_authorize_caller(operation, *kind, *caller_status) != M6CapabilityStatus::Allowed) {
        admin_precondition(session, operation, target->chat.id, "change-info");
        return std::nullopt;
    }
    if (!source) {
        emit_terminal(session, internal(operation));
        return std::nullopt;
    }
    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"tdlib_request", "setChatPhoto"},
              {"chat", chat_identity_json(target->chat)},
              {"delete", false},
              {"file", file_snapshot_json(source->snapshot())}};
    const auto arguments = arguments_from_plan(plan);
    return PreparedM6Mutation{
        .operation = operation,
        .principal = principal,
        .fingerprint_payload = arguments,
        .arguments = arguments,
        .plan = std::move(plan),
        .request = core::TdM6SetChatPhotoRequest{target->chat.id, std::nullopt},
        .pass1_source = std::move(source),
        .result = [](const core::TdM6Response& response,
                     const json& plan_value) -> std::optional<json> {
            return std::get_if<core::TdM6Ok>(&response) != nullptr
                       ? std::optional<json>{{{"chat", plan_value["chat"]}, {"photo", "set"}}}
                       : std::nullopt;
        }};
}

std::optional<PreparedM6Mutation>
prepare_delete_photo(const proto::Request& request, ResolverConsumer& resolver,
                     const ResolverPrincipal& principal, core::TdClient& client,
                     std::string_view account, RequestSession& session) {
    constexpr auto operation = proto::M6Operation::ChatSetPhoto;
    if (!exact_fields(request.args, {"chat", "delete"}) || !request.args["chat"].is_string() ||
        request.args["delete"] != true) {
        session.error("USAGE", "set-photo delete received malformed arguments",
                      {{"argument", "photo"}, {"reason", "invalid_argument"}}, kUsage);
        return std::nullopt;
    }
    const ResolverCaller caller{operation};
    auto target = resolve_chat(resolver, request.args["chat"].get<std::string>(), caller, session);
    if (!target) {
        return std::nullopt;
    }
    if (!target->observed_chat) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    const auto* observed_chat = &*target->observed_chat;
    const auto kind = admin_chat_kind(*target);
    if (!kind) {
        session.error("USAGE", "chat does not support this administration operation",
                      {{"argument", "chat"}, {"reason", "unsupported_chat_type"}}, kUsage);
        return std::nullopt;
    }
    auto caller_status =
        current_member_status(resolver, client, account, operation, target->chat.id, principal.id,
                              observed_chat, session);
    if (!caller_status || !valid_m6_member_status(*caller_status, *kind)) {
        emit_terminal(session, malformed(operation));
        return std::nullopt;
    }
    if (m6_authorize_caller(operation, *kind, *caller_status) != M6CapabilityStatus::Allowed) {
        admin_precondition(session, operation, target->chat.id, "change-info");
        return std::nullopt;
    }
    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"tdlib_request", "setChatPhoto"},
              {"chat", chat_identity_json(target->chat)},
              {"delete", true},
              {"file", nullptr}};
    return PreparedM6Mutation{
        .operation = operation,
        .principal = principal,
        .fingerprint_payload = request.args,
        .arguments = arguments_from_plan(plan),
        .plan = std::move(plan),
        .request = core::TdM6SetChatPhotoRequest{target->chat.id, std::nullopt},
        .pass1_source = nullptr,
        .result = [](const core::TdM6Response& response,
                     const json& plan_value) -> std::optional<json> {
            return std::get_if<core::TdM6Ok>(&response) != nullptr
                       ? std::optional<json>{{{"chat", plan_value["chat"]}, {"photo", "deleted"}}}
                       : std::nullopt;
        }};
}

std::optional<PreparedM6Mutation>
prepare_contact_mutation(proto::M6Operation operation, const proto::Request& request,
                         ResolverConsumer& resolver, const ResolverPrincipal& principal,
                         core::TdClient& client, std::string_view account,
                         RequestSession& session) {
    const ResolverCaller caller{operation};
    auto resolved_user =
        resolve_user(resolver, request.args["user"].get<std::string>(), caller, session);
    if (!resolved_user) {
        return std::nullopt;
    }
    auto record = resolver.read_target(
        [&](const auto& current) { return client.get_user(current, resolved_user->id); });
    if (m6_read_stopped(record, client, account, caller, session)) {
        return std::nullopt;
    }
    if (const auto* error = record.value.get_if<core::TdError>()) {
        emit_terminal(session, td_error_terminal(operation, *error));
        return std::nullopt;
    }
    const auto* user = record.value.get_if<core::TdUserSummary>();
    const auto converted = user != nullptr ? m6_user_identity(*user) : std::nullopt;
    if (user == nullptr || !converted || *converted != *resolved_user) {
        emit_terminal(session, internal(operation));
        return std::nullopt;
    }
    json plan{{"operation", operation_name(operation)},
              {"account", account},
              {"user", user_json(*resolved_user)}};
    core::TdM6Request mutation = core::TdM6SetBlockRequest{};
    if (operation == proto::M6Operation::ContactAdd) {
        plan["tdlib_request"] = "addContact";
        plan["first_name"] = user->first_name;
        plan["last_name"] = user->last_name;
        plan["phone_number_sha256"] =
            common::domain_separated_sha256("tgcli.m6.contact.phone.v1", user->phone_number);
        plan["share_phone_number"] = false;
        mutation = core::TdM6AddContactRequest{.user_id = user->id,
                                               .phone_number = user->phone_number,
                                               .first_name = user->first_name,
                                               .last_name = user->last_name,
                                               .share_phone_number = false};
    } else if (operation == proto::M6Operation::ContactRemove) {
        plan["tdlib_request"] = "removeContacts";
        plan["is_contact"] = false;
        mutation = core::TdM6RemoveContactsRequest{{user->id}};
    } else {
        const bool blocked = operation == proto::M6Operation::ContactBlock;
        plan["tdlib_request"] = "setMessageSenderBlockList";
        plan["blocked"] = blocked;
        mutation = core::TdM6SetBlockRequest{.user_id = user->id, .blocked = blocked};
    }
    return PreparedM6Mutation{.operation = operation,
                              .principal = principal,
                              .fingerprint_payload = request.args,
                              .arguments = arguments_from_plan(plan),
                              .plan = std::move(plan),
                              .request = std::move(mutation),
                              .pass1_source = nullptr,
                              .result = [operation](const core::TdM6Response& response,
                                                    const json& plan_value) -> std::optional<json> {
                                  if (std::get_if<core::TdM6Ok>(&response) == nullptr) {
                                      return std::nullopt;
                                  }
                                  json result{{"user", plan_value["user"]}};
                                  if (operation == proto::M6Operation::ContactAdd ||
                                      operation == proto::M6Operation::ContactRemove) {
                                      result["is_contact"] =
                                          operation == proto::M6Operation::ContactAdd;
                                  } else {
                                      result["blocked"] =
                                          operation == proto::M6Operation::ContactBlock;
                                  }
                                  return result;
                              }};
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed M6 mutation family router.
void WriteCoordinator::m6_mutation(proto::M6Operation operation, const proto::Request& request,
                                   RequestSession& session) {
    const bool contact = operation == proto::M6Operation::ContactAdd ||
                         operation == proto::M6Operation::ContactRemove ||
                         operation == proto::M6Operation::ContactBlock ||
                         operation == proto::M6Operation::ContactUnblock;
    const bool folder = operation == proto::M6Operation::FolderCreate ||
                        operation == proto::M6Operation::FolderEdit ||
                        operation == proto::M6Operation::FolderDelete ||
                        operation == proto::M6Operation::FolderAddChat ||
                        operation == proto::M6Operation::FolderRemoveChat;
    const bool topic = operation == proto::M6Operation::TopicCreate ||
                       operation == proto::M6Operation::TopicEdit ||
                       operation == proto::M6Operation::TopicClose ||
                       operation == proto::M6Operation::TopicReopen;
    const bool storage = operation == proto::M6Operation::StorageOptimize;
    const bool admin =
        operation == proto::M6Operation::ChatSetTitle ||
        operation == proto::M6Operation::ChatSetPhoto ||
        operation == proto::M6Operation::ChatSetDescription ||
        operation == proto::M6Operation::ChatInviteLink ||
        operation == proto::M6Operation::ChatPromote ||
        operation == proto::M6Operation::ChatDemote || operation == proto::M6Operation::ChatBan ||
        operation == proto::M6Operation::ChatUnban || operation == proto::M6Operation::ChatKick ||
        operation == proto::M6Operation::ChatSetPermissions;
    if (!contact && !folder && !topic && !storage && !admin) {
        session.error("INTERNAL", "M6 mutation coordinator is incomplete",
                      {{"operation", operation_name(operation)}, {"reason", "internal_error"}},
                      kGeneric);
        return;
    }
    if (contact && (!exact_fields(request.args, {"user"}) || !request.args["user"].is_string() ||
                    !valid_m6_exact_selector(request.args["user"].get_ref<const std::string&>()))) {
        session.error("USAGE", "contact mutation requires an exact user selector",
                      {{"argument", "user"}, {"reason", "invalid_argument"}}, kUsage);
        return;
    }
    const ResolverCaller caller{operation};
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(caller);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_resolver_error(*error, session, caller);
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if ((contact || folder) && principal.is_bot) {
        session.error("BOT_UNSUPPORTED", "this mutation requires a user account",
                      {{"operation", operation_name(operation)}}, kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    if (folder) {
        execute_prepared_m6(
            client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_, hooks_,
            request, session, *authority, operation, principal,
            [&](const std::shared_ptr<PreparedSource>&) {
                return operation == proto::M6Operation::FolderCreate
                           ? prepare_folder_create(request, resolver, principal, account_, session)
                           : prepare_existing_folder(operation, request, resolver, principal,
                                                     client_.get(), account_, session);
            });
        return;
    }
    if (topic) {
        execute_prepared_m6(client_.get(), account_, config_store_, foundation_,
                            audit_fatal_shutdown_, hooks_, request, session, *authority, operation,
                            principal, [&](const std::shared_ptr<PreparedSource>&) {
                                return prepare_topic_mutation(operation, request, resolver,
                                                              principal, client_.get(), account_,
                                                              session);
                            });
        return;
    }
    if (storage) {
        execute_prepared_m6(client_.get(), account_, config_store_, foundation_,
                            audit_fatal_shutdown_, hooks_, request, session, *authority, operation,
                            principal, [&](const std::shared_ptr<PreparedSource>&) {
                                return prepare_storage_optimize(request, principal, account_,
                                                                session);
                            });
        return;
    }
    if (admin) {
        const bool static_photo =
            operation == proto::M6Operation::ChatSetPhoto && request.args.contains("path");
        execute_prepared_m6(
            client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_, hooks_,
            request, session, *authority, operation, principal,
            [&](std::shared_ptr<PreparedSource> source) {
                if (operation == proto::M6Operation::ChatInviteLink) {
                    return prepare_invite_link(request, resolver, principal, client_.get(),
                                               account_, session);
                }
                if (operation == proto::M6Operation::ChatSetPhoto) {
                    return request.args.contains("delete")
                               ? prepare_delete_photo(request, resolver, principal, client_.get(),
                                                      account_, session)
                               : prepare_static_photo(request, resolver, principal, client_.get(),
                                                      account_, session, std::move(source));
                }
                return prepare_chat_admin(operation, request, resolver, principal, client_.get(),
                                          account_, session);
            },
            static_photo ? M6SourceAdmission{[&] {
                return admit_static_photo_source(request, session, hooks_);
            }}
                         : M6SourceAdmission{});
        return;
    }
    execute_prepared_m6(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                        hooks_, request, session, *authority, operation, principal,
                        [&](const std::shared_ptr<PreparedSource>&) {
                            return prepare_contact_mutation(operation, request, resolver, principal,
                                                            client_.get(), account_, session);
                        });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed session transaction.
void WriteCoordinator::terminate_session(const proto::Request& request, RequestSession& session) {
    if (!exact_fields(request.args, {"session_id"}) || !request.args["session_id"].is_string()) {
        session.error("USAGE", "session terminate requires a canonical session id",
                      {{"argument", "session_id"}, {"reason", "invalid_argument"}}, kUsage);
        return;
    }
    const auto& id_text = request.args["session_id"].get_ref<const std::string&>();
    std::int64_t session_id = 0;
    const auto [end, parse_error] =
        std::from_chars(id_text.data(), id_text.data() + id_text.size(), session_id);
    if (parse_error != std::errc{} || end != id_text.data() + id_text.size() ||
        std::to_string(session_id) != id_text) {
        session.error("USAGE", "session terminate requires a canonical session id",
                      {{"argument", "session_id"}, {"reason", "invalid_argument"}}, kUsage);
        return;
    }
    if (request.context.dry_run &&
        !run_session_recovery_preflight(
            foundation_, proto::SessionOperation::Terminate, session,
            hooks_ ? hooks_->file_spool : std::shared_ptr<const testing::FileSpoolHooks>{})) {
        return;
    }
    const ResolverCaller caller{proto::SessionOperation::Terminate};
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(caller);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_resolver_error(*error, session, caller);
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (principal.is_bot) {
        session.error("BOT_UNSUPPORTED", "session commands require a user account",
                      {{"operation", "session_terminate"}}, kUsage);
        return;
    }
    const auto& admitted = session.admitted_config();
    if (!admitted || admitted->account != account_ || !admitted->account_snapshot) {
        session.error("INTERNAL", "write config admission is missing",
                      {{"operation", "session_terminate"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    AuthoritySource authority = AuthoritySource::Request;
    if (!request.context.dry_run) {
        const auto decision = evaluate_destructive_authority(
            request.context, {.grant_valid = admitted->standing_write_grants_valid,
                              .allow_write = admitted->settings.allow_write});
        if (const auto* denied = std::get_if<DeniedAuthority>(&decision)) {
            session.error(
                "WRITE_DENIED", "write requires explicit authority",
                {{"account", account_}, {"reason", write_denial_reason_name(denied->reason)}},
                kDenied);
            return;
        }
        const auto* granted = std::get_if<GrantedAuthority>(&decision);
        if (granted == nullptr) {
            session.error("INTERNAL", "internal error",
                          {{"operation", "session_terminate"}, {"reason", "internal_error"}},
                          kGeneric);
            return;
        }
        authority = granted->source;
    }
    if (!request.context.dry_run &&
        !run_session_recovery_preflight(
            foundation_, proto::SessionOperation::Terminate, session,
            hooks_ ? hooks_->file_spool : std::shared_ptr<const testing::FileSpoolHooks>{})) {
        return;
    }
    auto active = resolver.read_target(
        [&](const auto& current) { return client_.get().get_active_sessions(current); });
    if (m6_read_stopped(active, client_.get(), account_, caller, session)) {
        return;
    }
    if (const auto* error = active.value.get_if<core::TdError>()) {
        const auto mapped =
            error->code == 429
                ? terminal("RATE_LIMITED", "Telegram rate limit exceeded",
                           {{"operation", "session_terminate"},
                            {"tdlib_code", 429},
                            {"retry_after", parse_retry_after_seconds(error->message)}},
                           kRateLimited)
                : terminal("TDLIB_ERROR", "Telegram request failed",
                           {{"operation", "session_terminate"}, {"tdlib_code", error->code}},
                           kGeneric);
        emit_terminal(session, mapped);
        return;
    }
    const auto* sessions = active.value.get_if<core::TdSessions>();
    if (sessions == nullptr || !m6_session_list_json(*sessions)) {
        session.error("INTERNAL", "TDLib returned malformed session data",
                      {{"operation", "session_terminate"},
                       {"reason", "malformed_tdlib_response"},
                       {"tdlib_type_id", nullptr}},
                      kGeneric);
        return;
    }
    const auto found = std::ranges::find(sessions->items, id_text, &core::TdSession::id);
    if (found == sessions->items.end()) {
        session.error("NOT_FOUND", "session not found",
                      {{"operation", "session_terminate"}, {"session_id", id_text}}, kNotFound);
        return;
    }
    if (found->is_current) {
        session.error("PRECONDITION_FAILED", "current session cannot be terminated",
                      {{"operation", "session_terminate"},
                       {"session_id", id_text},
                       {"reason", "current_session"}},
                      kGeneric);
        return;
    }
    auto target = m6_session_terminate_target_json(*found);
    if (!target) {
        session.error("INTERNAL", "TDLib returned malformed session data",
                      {{"operation", "session_terminate"},
                       {"reason", "malformed_tdlib_response"},
                       {"tdlib_type_id", nullptr}},
                      kGeneric);
        return;
    }
    const json plan{{"operation", "session_terminate"},
                    {"account", account_},
                    {"tdlib_request", "terminateSession"},
                    {"session", *target}};
    const json fingerprint_root{{"version", 1},
                                {"account", account_},
                                {"principal", {{"id", principal.id}, {"is_bot", false}}},
                                {"operation", "session_terminate"},
                                {"payload", request.args}};
    auto canonical = common::canonical_json(fingerprint_root);
    const auto* serialized = std::get_if<std::string>(&canonical);
    auto fingerprint = serialized != nullptr
                           ? parse_idempotency_request_fingerprint(common::domain_separated_sha256(
                                 "tgcli-idempotency-request-v1", *serialized))
                           : std::nullopt;
    std::string contract_error;
    auto arguments = write_contract::make_arguments(session_operation(), {{"session_id", id_text}},
                                                    contract_error);
    auto typed_plan =
        write_contract::make_plan(session_operation(), account_, plan, contract_error);
    if (!fingerprint || !arguments || !typed_plan) {
        session.error("INTERNAL", "session write contract is invalid",
                      {{"operation", "session_terminate"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    if (!request.context.dry_run &&
        session.begin_audited_terminal() != AuditedTerminalStatus::Designated) {
        return;
    }
    auto invocation = request.context.dry_run ? std::string{} : random_hex32();
    if (!request.context.dry_run && invocation.empty()) {
        session.error("AUDIT_UNAVAILABLE", "cannot create audit identity",
                      {{"account", account_},
                       {"path", foundation_ ? foundation_->audit().path() : std::string{}},
                       {"reason", "open_failed"}},
                      kDenied);
        return;
    }
    const WriteKernelRequest input{
        .operation = session_operation(),
        .account = request.account,
        .idempotency_key_hash = std::nullopt,
        .invocation_id = std::move(invocation),
        .intent_timestamp = timestamp(),
        .config_path = config_store_.path(),
        .config_snapshot = admitted->snapshot_identity,
        .authority_source = authority,
        .request_source_bytes = session.request_source_bytes(),
        .sample_now = unix_seconds,
        .dry_run = request.context.dry_run,
        .deadline = session.deadline(),
        .cancellation_token = session.cancellation_token(),
        .cancelled = [&session] { return session.cancellation_requested(); },
        .recovery_preflight_complete = true};
    auto dispatch = std::make_shared<SessionDispatchState>();
    const WriteKernel kernel(foundation_);
    WriteKernelHooks hooks;
    hooks.admit = [arguments = std::move(*arguments), fingerprint]() mutable {
        return WriteAdmission{.arguments = std::move(arguments),
                              .request_fingerprint = *fingerprint,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    hooks.plan = [typed_plan = std::move(*typed_plan)](const WriteAdmission&) mutable {
        return WritePlanningOutcome{std::move(typed_plan)};
    };
    hooks.confirm = [&session](const write_contract::Plan& immutable, bool) {
        const auto rejected = [&] {
            return terminal("CONFIRMATION_REQUIRED", "session termination was not confirmed",
                            {{"account", immutable.account()},
                             {"action", "session_terminate"},
                             {"target", immutable.value()}},
                            kDenied);
        };
        if (session.request().context.yes) {
            return WriteConfirmationOutcome{.status = WriteConfirmationStatus::ConfirmedYes,
                                            .terminal = std::nullopt};
        }
        if (!session.request().context.tty) {
            return WriteConfirmationOutcome{.status = WriteConfirmationStatus::Rejected,
                                            .terminal = rejected()};
        }
        const auto& value = immutable.value()["session"];
        auto answer =
            session.challenge({proto::ChallengeKind::DestructiveConfirmation,
                               std::nullopt,
                               std::nullopt,
                               "terminate session " + value["id"].get<std::string>() +
                                   ": application=" + value["application_name"].dump() +
                                   " version=" + value["application_version"].dump() +
                                   " device=" + value["device_type"].get<std::string>() +
                                   " model=" + value["device_model"].dump() +
                                   " platform=" + value["platform"].dump() +
                                   " last_active=" + value["last_active_date"].dump() + "? [y/N] ",
                               {{"action", "session_terminate"}, {"target", immutable.value()}},
                               false});
        const auto confirmed = answer.take_boolean();
        return WriteConfirmationOutcome{
            .status = answer.status() == ChallengeStatus::Answered && confirmed.value_or(false)
                          ? WriteConfirmationStatus::ConfirmedTty
                          : WriteConfirmationStatus::Rejected,
            .terminal = confirmed.value_or(false) ? std::nullopt : std::optional{rejected()}};
    };
    hooks.verify_config_grant = [this](std::string_view expected, std::string_view account,
                                       const config::MutationControl& control) {
        return config_store_.verify_write_grant(expected, account, control);
    };
    hooks.revalidate_principal = [this, principal]() -> std::optional<json> {
        if (hooks_ && hooks_->before_principal_cas) {
            hooks_->before_principal_cas();
        }
        const auto current = client_.get().auth_state();
        if (current && current->data.state == core::AuthState::Ready &&
            current->client_id == principal.client_id &&
            current->client_generation == principal.client_generation &&
            current->auth_sequence == principal.auth_sequence) {
            return std::nullopt;
        }
        return not_authed(account_,
                          current != nullptr ? current->data.state : core::AuthState::Unknown);
    };
    hooks.post_intent = [](const write_contract::Plan&, const WriteAdmission&) {
        return WritePostIntentPreparation{};
    };
    hooks.revalidate_auth_and_schedule =
        [this, &session, dispatch, session_id,
         principal](const write_contract::Plan& immutable) -> WriteDispatchAdmissionOutcome {
        if (hooks_ && hooks_->before_dispatch_principal_cas) {
            hooks_->before_dispatch_principal_cas();
        }
        if (deadline_expired(session.deadline())) {
            return session_stored_from(terminal("TIMEOUT", "request timed out",
                                                {{"operation", "session_terminate"},
                                                 {"phase", "preflight"},
                                                 {"state", "ready"},
                                                 {"outcome", "not_started"},
                                                 {"idempotency", "not_requested"}},
                                                kTimeout));
        }
        if (session.cancellation_requested()) {
            return WriteDispatchStopped{};
        }
        auto current = client_.get().auth_state();
        if (!current || current->data.state != core::AuthState::Ready ||
            current->client_id != principal.client_id ||
            current->client_generation != principal.client_generation ||
            current->auth_sequence != principal.auth_sequence) {
            return session_stored_from(
                not_authed(immutable.account(),
                           current != nullptr ? current->data.state : core::AuthState::Unknown));
        }
        dispatch->authorization = std::move(current);
        dispatch->request = core::TdTerminateSessionRequest{session_id};
        dispatch->coordinator = std::make_unique<DirectRpcCoordinator>(
            client_.get(), session, hooks_ ? hooks_->direct_rpc : DirectRpcHooks{});
        auto prepared = dispatch->coordinator->prepare(core::TdDirectRequest{*dispatch->request},
                                                       dispatch->authorization);
        dispatch->request.reset();
        if (std::holds_alternative<DirectTimedOut>(prepared)) {
            return session_stored_from(terminal("TIMEOUT", "request timed out",
                                                {{"operation", "session_terminate"},
                                                 {"phase", "preflight"},
                                                 {"state", "ready"},
                                                 {"outcome", "not_started"},
                                                 {"idempotency", "not_requested"}},
                                                kTimeout));
        }
        if (std::holds_alternative<DirectCancelled>(prepared)) {
            return WriteDispatchStopped{};
        }
        if (const auto* lost = std::get_if<DirectAuthorizationLost>(&prepared)) {
            return session_stored_from(not_authed(
                immutable.account(),
                lost->snapshot != nullptr ? lost->snapshot->data.state : core::AuthState::Unknown));
        }
        if (std::holds_alternative<DirectRejected>(prepared)) {
            return session_stored_from(terminal("INTERNAL", "TDLib returned malformed session data",
                                                {{"operation", "session_terminate"},
                                                 {"reason", "malformed_tdlib_response"},
                                                 {"tdlib_type_id", nullptr}},
                                                kGeneric));
        }
        return WriteDispatchPreparation{
            .proof = {{"tdlib_function", "terminateSession"},
                      {"dispatch_token", random_hex32()},
                      {"client_generation", dispatch->authorization->client_generation}}};
    };
    hooks.dispatch = [dispatch, id_text](const write_contract::Plan&,
                                         const WriteDispatchPreparation&,
                                         WriteDurableObservationSink&) -> WriteDispatchOutcome {
        auto outcome = dispatch->coordinator->execute_prepared();
        return std::visit(
            [&](auto&& selected) -> WriteDispatchOutcome {
                using Outcome = std::decay_t<decltype(selected)>;
                if constexpr (std::is_same_v<Outcome, DirectSuccess>) {
                    const auto* result =
                        std::get_if<DirectTerminateSessionResult>(&selected.result);
                    auto stored =
                        result != nullptr && std::to_string(result->session_id) == id_text
                            ? session_stored_result({{"session_id", id_text}, {"terminated", true}})
                            : std::nullopt;
                    return {.terminal =
                                stored ? std::move(*stored)
                                       : session_stored_from(terminal(
                                             "INTERNAL", "TDLib returned malformed session data",
                                             {{"operation", "session_terminate"},
                                              {"reason", "malformed_tdlib_response"},
                                              {"tdlib_type_id", nullptr}},
                                             kGeneric)),
                            .mutation_state = stored ? AccountAuditMutationState::Confirmed
                                                     : AccountAuditMutationState::Possible,
                            .mutation_confirmed = stored.has_value()};
                } else if constexpr (std::is_same_v<Outcome, DirectTdError>) {
                    const auto value =
                        selected.error.code == 429
                            ? terminal("RATE_LIMITED", "Telegram rate limit exceeded",
                                       {{"operation", "session_terminate"},
                                        {"tdlib_code", 429},
                                        {"retry_after",
                                         parse_retry_after_seconds(selected.error.message)}},
                                       kRateLimited)
                            : terminal("TDLIB_ERROR", "Telegram request failed",
                                       {{"operation", "session_terminate"},
                                        {"tdlib_code", selected.error.code}},
                                       kGeneric);
                    return {.terminal = session_stored_from(value),
                            .mutation_state = audit_state(selected.mutation_state)};
                } else {
                    return {.terminal = session_stored_from(
                                terminal("INTERNAL", "TDLib returned malformed session data",
                                         {{"operation", "session_terminate"},
                                          {"reason", "malformed_tdlib_response"},
                                          {"tdlib_type_id", nullptr}},
                                         kGeneric)),
                            .mutation_state = audit_state(selected.mutation_state)};
                }
            },
            std::move(outcome));
    };
    hooks.timestamp = timestamp;
    hooks.audit_fatal_shutdown = [&session, this] {
        session.audit_fatal();
        if (audit_fatal_shutdown_) {
            audit_fatal_shutdown_();
        }
    };
    const auto result = kernel.run(input, hooks);
    if (result.status == WriteKernelStatus::DryRunPlanned && result.plan) {
        session.result({{"dry_run", true}, {"plan", result.plan->value()}});
    } else if (result.terminal) {
        emit_terminal(session, *result.terminal);
    }
}

} // namespace tgcli::daemon
