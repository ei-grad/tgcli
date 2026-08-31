#include "daemon/raw_commands.hpp"

#include "common/deadline.hpp"
#include "common/exit_codes.hpp"
#include "daemon/destructive_contract.hpp"
#include "daemon/dispatch.hpp"
#include "daemon/ready_read.hpp"
#include "daemon/request_session.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <td/telegram/td_api.h>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::string_view kPinnedTdlibSha = "a17f87c4cff7b90b278d12b91ba0614383aaee82";

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields,
                               [&](std::string_view field) { return value.contains(field); });
}

std::string tier_name(Tier tier) {
    switch (tier) {
    case Tier::Read:
        return "read";
    case Tier::Write:
        return "write";
    case Tier::Destructive:
        return "destructive";
    }
    return {};
}

core::DescriptorKind descriptor_tier(Tier tier) {
    switch (tier) {
    case Tier::Read:
        return core::DescriptorKind::Read;
    case Tier::Write:
        return core::DescriptorKind::Write;
    case Tier::Destructive:
        return core::DescriptorKind::Destructive;
    }
    return core::DescriptorKind::Lifecycle;
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
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

void usage(RequestSession& session, std::string message, std::optional<std::string> argument) {
    session.error(
        "USAGE", std::move(message),
        {{"argument", argument ? json(*argument) : json(nullptr)}, {"reason", "invalid_argument"}},
        kUsage);
}

void internal(RequestSession& session, std::string_view reason) {
    session.error("INTERNAL", "raw request failed", {{"operation", "raw"}, {"reason", reason}},
                  kGeneric);
}

void not_authed(RequestSession& session, std::string_view account, core::AuthState state,
                std::string_view reason) {
    session.error(
        "NOT_AUTHED", "raw requires an authenticated account",
        {{"account", account}, {"state", core::auth_state_name(state)}, {"reason", reason}},
        kNotAuthed);
}

void denied(RequestSession& session, std::string_view function, std::string_view reason) {
    session.error("DENIED", "raw function is denied",
                  {{"operation", "raw"}, {"function", function}, {"reason", reason}}, kDenied);
}

void timeout(RequestSession& session, core::TdClient& client) {
    const auto snapshot = client.auth_state();
    session.error(
        "TIMEOUT", "raw request timed out",
        {{"operation", "raw"},
         {"state", snapshot ? json(core::auth_state_name(snapshot->data.state)) : json(nullptr)}},
        kTimeout);
}

bool stop_for_read(const ReadyReadResult& result, core::TdClient& client, std::string_view account,
                   RequestSession& session) {
    switch (result.status) {
    case ReadyReadStatus::Response:
        return false;
    case ReadyReadStatus::AuthorizationLost:
        if (!session.cancellation_requested()) {
            not_authed(session, account,
                       result.snapshot ? result.snapshot->data.state : core::AuthState::Unknown,
                       "authorization_lost");
        }
        return true;
    case ReadyReadStatus::TimedOut:
        if (!session.cancellation_requested()) {
            timeout(session, client);
        }
        return true;
    case ReadyReadStatus::Failed:
        if (!session.cancellation_requested()) {
            internal(session, "internal_error");
        }
        return true;
    case ReadyReadStatus::Cancelled:
        return true;
    }
    return true;
}

bool principal_allowed(raw::RawPrincipal principal, bool is_bot) {
    return principal == raw::RawPrincipal::Both ||
           (principal == raw::RawPrincipal::Bot && is_bot) ||
           (principal == raw::RawPrincipal::User && !is_bot);
}

json intent_record(std::string_view invocation, std::string_view function, Tier tier,
                   const raw::Digest& digest) {
    return {{"schema_version", 3},
            {"record_type", "raw_intent"},
            {"invocation_id", invocation},
            {"function", function},
            {"tier", tier_name(tier)},
            {"tdlib_sha", kPinnedTdlibSha},
            {"request_sha256", digest.sha256},
            {"request_bytes", digest.bytes}};
}

json dispatch_record(std::string_view invocation, std::string_view token,
                     std::uint64_t generation) {
    return {{"schema_version", 3},
            {"record_type", "raw_checkpoint"},
            {"invocation_id", invocation},
            {"stage", "raw_dispatch_started"},
            {"data", {{"dispatch_token", token}, {"generation", std::to_string(generation)}}}};
}

json response_record(std::string_view invocation, std::string_view token, std::uint64_t generation,
                     const raw::MaterializedResponse& response) {
    return {
        {"schema_version", 3},
        {"record_type", "raw_checkpoint"},
        {"invocation_id", invocation},
        {"stage", "raw_response_received"},
        {"data",
         {{"dispatch_token", token},
          {"generation", std::to_string(generation)},
          {"kind", response.td_error_code ? "error" : "result"},
          {"response_type", response.response_type},
          {"td_error_code", response.td_error_code ? json(*response.td_error_code) : json(nullptr)},
          {"response_sha256", response.digest.sha256},
          {"response_bytes", response.digest.bytes}}}};
}

json malformed_record(std::string_view invocation, std::string_view token, std::uint64_t generation,
                      const raw::MalformedResponse& response) {
    return {
        {"schema_version", 3},
        {"record_type", "raw_checkpoint"},
        {"invocation_id", invocation},
        {"stage", "raw_response_received"},
        {"data",
         {{"dispatch_token", token},
          {"generation", std::to_string(generation)},
          {"kind", "malformed"},
          {"response_type", response.response_type ? json(*response.response_type) : json(nullptr)},
          {"td_error_code", nullptr},
          {"response_sha256", response.digest ? json(response.digest->sha256) : json(nullptr)},
          {"response_bytes", response.digest ? json(response.digest->bytes) : json(nullptr)}}}};
}

json oversized_record(std::string_view invocation, std::string_view token, std::uint64_t generation,
                      const raw::OversizedResponse& response) {
    return {{"schema_version", 3},
            {"record_type", "raw_checkpoint"},
            {"invocation_id", invocation},
            {"stage", "raw_response_received"},
            {"data",
             {{"dispatch_token", token},
              {"generation", std::to_string(generation)},
              {"kind", "result_too_large"},
              {"response_type", response.response_type},
              {"td_error_code", nullptr},
              {"response_sha256", nullptr},
              {"response_bytes", nullptr}}}};
}

json success_outcome(std::string_view invocation, const raw::MaterializedResponse& response) {
    return {{"schema_version", 3},
            {"record_type", "raw_outcome"},
            {"invocation_id", invocation},
            {"mutation_state", "confirmed"},
            {"terminal",
             {{"kind", "result_digest"},
              {"response_type", response.response_type},
              {"response_sha256", response.digest.sha256},
              {"response_bytes", response.digest.bytes}}}};
}

json error_outcome(std::string_view invocation, std::int32_t code) {
    return {{"schema_version", 3},
            {"record_type", "raw_outcome"},
            {"invocation_id", invocation},
            {"mutation_state", "possible"},
            {"terminal",
             {{"kind", "error_summary"},
              {"code", code == 429 ? "RATE_LIMITED" : "TDLIB_ERROR"},
              {"td_error_code", code}}}};
}

json malformed_outcome(std::string_view invocation) {
    return {{"schema_version", 3},
            {"record_type", "raw_outcome"},
            {"invocation_id", invocation},
            {"mutation_state", "possible"},
            {"terminal",
             {{"kind", "error_summary"},
              {"code", "INTERNAL"},
              {"reason", "unexpected_response"},
              {"td_error_code", nullptr}}}};
}

json oversized_outcome(std::string_view invocation) {
    auto outcome = malformed_outcome(invocation);
    outcome["terminal"]["reason"] = "result_too_large";
    return outcome;
}

json unconfirmed_outcome(std::string_view invocation) {
    return {{"schema_version", 3},
            {"record_type", "raw_outcome"},
            {"invocation_id", invocation},
            {"mutation_state", "possible"},
            {"terminal",
             {{"kind", "error_summary"},
              {"code", "RAW_OUTCOME_UNCONFIRMED"},
              {"td_error_code", nullptr}}}};
}

void audit_incomplete(RequestSession& session, std::string_view account, std::string_view path,
                      std::string_view mutation, json stages) {
    session.error("AUDIT_INCOMPLETE", "raw audit is incomplete",
                  {{"account", account},
                   {"path", path},
                   {"mutation_state", mutation},
                   {"completed_stages", std::move(stages)}},
                  kGeneric);
}

void unconfirmed(RequestSession& session, std::string_view function, std::string_view hash) {
    session.error("RAW_OUTCOME_UNCONFIRMED", "raw request outcome is unconfirmed",
                  {{"operation", "raw"},
                   {"function", function},
                   {"request_sha256", hash},
                   {"mutation_state", "possible"}},
                  kGeneric);
}

bool append_or_incomplete(raw::audit_v3::Log& audit, const json& record, RequestSession& session,
                          std::string_view account, std::string_view mutation, json stages) {
    if (audit.append(record)) {
        return true;
    }
    audit_incomplete(session, account, audit.path(), mutation, std::move(stages));
    return false;
}

} // namespace

RawCoordinator::RawCoordinator(core::TdClient& client, std::string account,
                               std::string state_directory, uid_t expected_uid)
    : client_(client), account_(std::move(account)),
      audit_(std::move(state_directory), expected_uid) {}

// This is the single closed fail-closed raw admission pipeline.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RawCoordinator::execute(const proto::Request& request, RequestSession& session) {
    if (!exact_fields(request.args, {"input"}) || !request.args["input"].is_string() ||
        request.context.idempotency_key || request.context.media_dir) {
        usage(session, "raw request arguments are invalid", std::nullopt);
        return;
    }
    auto parsed = raw::parse(std::string(request.args["input"].get_ref<const std::string&>()),
                             request.wipe_observer());
    auto* function = std::get_if<raw::TypedFunction>(&parsed);
    if (function == nullptr) {
        usage(session, "raw stdin is not a valid pinned TDLib function", "-");
        return;
    }
    if (!raw::policy_activation_ready()) {
        internal(session, "policy_table_invalid");
        return;
    }
    const auto metadata = raw::policy_metadata(*function);
    if (!metadata) {
        internal(session, "policy_table_invalid");
        return;
    }
    ReadyReadSession reads(client_, session);
    const auto snapshot = reads.current();
    if (!snapshot || snapshot->data.state != core::AuthState::Ready) {
        not_authed(session, account_, snapshot ? snapshot->data.state : core::AuthState::Unknown,
                   "not_ready");
        return;
    }
    auto principal_result = reads.read_exact(
        [this](const auto& authorization) { return client_.get_me(authorization); }, snapshot);
    if (stop_for_read(principal_result, client_, account_, session)) {
        return;
    }
    const auto* principal = principal_result.value.get_if<core::TdUserSummary>();
    if (principal == nullptr || principal->id <= 0) {
        internal(session, "unexpected_response");
        return;
    }
    if (!principal_allowed(metadata->principal, principal->is_bot)) {
        denied(session, function->name(), "principal_unsupported");
        return;
    }
    const auto policy = raw::evaluate_body_policy(*function);
    if (!policy.effective_tier) {
        denied(session, function->name(), "function_denied");
        return;
    }
    for (std::size_t index = 0; index < policy.preflight.non_secret_chat_count; ++index) {
        const auto chat_id = policy.preflight.non_secret_chat_ids.at(index);
        auto chat_result = reads.read_exact(
            [this, chat_id](const auto& authorization) {
                return client_.get_chat(authorization, chat_id);
            },
            snapshot);
        if (stop_for_read(chat_result, client_, account_, session)) {
            return;
        }
        const auto* chat = chat_result.value.get_if<core::TdChat>();
        if (chat == nullptr || chat->id != chat_id || chat->kind == core::TdChatKind::Secret ||
            chat->kind == core::TdChatKind::Unknown) {
            denied(session, function->name(), "secret_chat_unsupported");
            return;
        }
    }
    const auto tier = *policy.effective_tier;
    auto digest_value = function->request_digest(kPinnedTdlibSha, tier_name(tier));
    const auto* digest = std::get_if<raw::Digest>(&digest_value);
    if (digest == nullptr) {
        internal(session, "canonicalization_failed");
        return;
    }
    if (request.context.dry_run) {
        if (tier == Tier::Read) {
            usage(session, "--dry-run is unsupported for read-tier raw functions", "--dry-run");
            return;
        }
        session.result({{"dry_run", true},
                        {"plan",
                         {{"operation", "raw"},
                          {"function", function->name()},
                          {"tier", tier_name(tier)},
                          {"tdlib_sha", kPinnedTdlibSha},
                          {"request_sha256", digest->sha256},
                          {"request_bytes", digest->bytes}}}});
        return;
    }
    if (tier != Tier::Read) {
        const auto& admitted = session.admitted_config();
        const auto authority = evaluate_destructive_authority(
            request.context, {.grant_valid = admitted && admitted->standing_write_grants_valid,
                              .allow_write = admitted && admitted->settings.allow_write});
        if (!std::holds_alternative<GrantedAuthority>(authority)) {
            denied(session, function->name(), "write_grant_required");
            return;
        }
    }
    if (tier == Tier::Destructive && !request.context.yes) {
        const json target{{"function", function->name()}, {"request_sha256", digest->sha256}};
        if (!request.context.tty) {
            session.error("CONFIRMATION_REQUIRED", "raw request was not confirmed",
                          {{"account", account_}, {"action", "raw"}, {"target", target}}, kDenied);
            return;
        }
        std::string plan_error;
        auto plan = proto::make_raw_destructive_plan(account_, std::string(function->name()),
                                                     digest->sha256, plan_error);
        if (!plan) {
            internal(session, "internal_error");
            return;
        }
        auto answer = session.challenge(
            {proto::ChallengeKind::DestructiveConfirmation,
             snapshot->client_generation,
             snapshot->auth_sequence,
             "raw " + std::string(function->name()) + " " + digest->sha256 + "? [y/N] ",
             {{"action", "raw"}, {"target", proto::serialize(*plan)}},
             false});
        const auto confirmed = answer.take_boolean();
        if (answer.status() != ChallengeStatus::Answered || !confirmed.value_or(false)) {
            if (answer.status() == ChallengeStatus::TimedOut) {
                timeout(session, client_);
            } else if (!session.cancellation_requested()) {
                session.error("CONFIRMATION_REQUIRED", "raw request was not confirmed",
                              {{"account", account_}, {"action", "raw"}, {"target", target}},
                              kDenied);
            }
            return;
        }
    }
    const bool audited = tier != Tier::Read;
    std::unique_lock<std::mutex> audit_lock;
    if (audited) {
        audit_lock = std::unique_lock(audit_mutex_);
        const auto recovery = audit_.recover();
        if (recovery.status == raw::audit_v3::LogStatus::Contradiction ||
            recovery.status == raw::audit_v3::LogStatus::Unavailable) {
            audit_incomplete(session, account_, audit_.path(), "none", json::array());
            return;
        }
        if (recovery.status == raw::audit_v3::LogStatus::Unconfirmed && recovery.unconfirmed) {
            unconfirmed(session, recovery.unconfirmed->function,
                        recovery.unconfirmed->request_sha256);
            return;
        }
    }
    auto request_value = function->release_for_dispatch(tier);
    if (!request_value) {
        internal(session, "internal_error");
        return;
    }
    auto prepared = client_.prepare_raw(snapshot, descriptor_tier(tier), std::move(*request_value));
    if (!prepared) {
        const auto current = client_.auth_state();
        if (!current || current->data.state != core::AuthState::Ready ||
            current->client_generation != snapshot->client_generation ||
            current->auth_sequence != snapshot->auth_sequence) {
            not_authed(session, account_, current ? current->data.state : core::AuthState::Unknown,
                       "authorization_lost");
        } else {
            internal(session, "internal_error");
        }
        return;
    }
    const auto invocation = audited ? random_hex32() : std::string{};
    const auto token = audited ? random_hex32() : std::string{};
    if (audited && (invocation.empty() || token.empty())) {
        session.error("AUDIT_UNAVAILABLE", "cannot create raw audit identity",
                      {{"account", account_}, {"path", audit_.path()}, {"reason", "open_failed"}},
                      kGeneric);
        return;
    }
    if (audited &&
        !append_or_incomplete(audit_, intent_record(invocation, function->name(), tier, *digest),
                              session, account_, "none", json::array())) {
        return;
    }
    if (audited && !append_or_incomplete(
                       audit_, dispatch_record(invocation, token, snapshot->client_generation),
                       session, account_, "none", json::array())) {
        return;
    }
    auto prepared_state =
        std::make_shared<std::optional<core::TdPreparedWrite>>(std::move(prepared));
    auto response = reads.read_exact(
        [&client = client_, prepared_state](const auto&) mutable {
            auto value = std::move(prepared_state->value());
            prepared_state->reset();
            return client.send(std::move(value));
        },
        snapshot);
    if (response.status != ReadyReadStatus::Response) {
        if (audited) {
            if (!append_or_incomplete(audit_, unconfirmed_outcome(invocation), session, account_,
                                      "possible", json::array({"raw_dispatch_started"}))) {
                return;
            }
            unconfirmed(session, function->name(), digest->sha256);
        } else {
            static_cast<void>(stop_for_read(response, client_, account_, session));
        }
        return;
    }
    auto* native = response.value.get_if<core::TdRawObjectPtr>();
    auto classified = native != nullptr ? raw::classify_response(*function, std::move(*native),
                                                                 request.wipe_observer())
                                        : raw::ClassifiedResponse{raw::MalformedResponse{}};
    if (auto* malformed = std::get_if<raw::MalformedResponse>(&classified)) {
        if (audited &&
            (!append_or_incomplete(
                 audit_,
                 malformed_record(invocation, token, snapshot->client_generation, *malformed),
                 session, account_, "possible", json::array({"raw_dispatch_started"})) ||
             !append_or_incomplete(
                 audit_, malformed_outcome(invocation), session, account_, "possible",
                 json::array({"raw_dispatch_started", "raw_response_received"})))) {
            return;
        }
        internal(session, "unexpected_response");
        return;
    }
    if (auto* oversized = std::get_if<raw::OversizedResponse>(&classified)) {
        if (audited &&
            (!append_or_incomplete(
                 audit_,
                 oversized_record(invocation, token, snapshot->client_generation, *oversized),
                 session, account_, "possible", json::array({"raw_dispatch_started"})) ||
             !append_or_incomplete(
                 audit_, oversized_outcome(invocation), session, account_, "possible",
                 json::array({"raw_dispatch_started", "raw_response_received"})))) {
            return;
        }
        internal(session, "result_too_large");
        return;
    }
    if (const auto* failure = std::get_if<raw::Failure>(&classified)) {
        internal(session, failure->error == raw::Error::CanonicalTooLarge
                              ? "result_too_large"
                              : "canonicalization_failed");
        return;
    }
    auto materialized = std::get<raw::MaterializedResponse>(std::move(classified));
    if (audited &&
        !append_or_incomplete(
            audit_, response_record(invocation, token, snapshot->client_generation, materialized),
            session, account_, "possible", json::array({"raw_dispatch_started"}))) {
        return;
    }
    if (materialized.td_error_code) {
        if (audited &&
            !append_or_incomplete(audit_, error_outcome(invocation, *materialized.td_error_code),
                                  session, account_, "possible",
                                  json::array({"raw_dispatch_started", "raw_response_received"}))) {
            return;
        }
        if (*materialized.td_error_code == 429) {
            session.error("RATE_LIMITED", "Telegram rate limit",
                          {{"operation", "raw"},
                           {"function", function->name()},
                           {"tdlib_code", 429},
                           {"retry_after", materialized.retry_after.value_or(0)}},
                          kRateLimited);
        } else {
            session.error("TDLIB_ERROR", "raw TDLib request failed",
                          {{"operation", "raw"},
                           {"function", function->name()},
                           {"tdlib_code", *materialized.td_error_code}},
                          kGeneric);
        }
        return;
    }
    if (audited &&
        !append_or_incomplete(audit_, success_outcome(invocation, materialized), session, account_,
                              "confirmed",
                              json::array({"raw_dispatch_started", "raw_response_received"}))) {
        return;
    }
    session.raw_result(std::move(materialized.canonical));
}

void register_raw_command(Dispatcher& dispatcher, RawCoordinator& coordinator) {
    dispatcher.register_command(
        "raw", {.tier = Tier::Read,
                .handler =
                    [&coordinator](const proto::Request& request, RequestSession& session) {
                        coordinator.execute(request, session);
                    },
                .config_admission = true,
                .dynamic_raw_policy = true});
}

} // namespace tgcli::daemon
