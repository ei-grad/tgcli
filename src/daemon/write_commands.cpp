#include "daemon/write_commands.hpp"

#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/direct_rpc.hpp"
#include "daemon/message_summary.hpp"
#include "daemon/rate_limit.hpp"
#include "daemon/request_fingerprint.hpp"
#include "daemon/request_session.hpp"
#include "daemon/resolver.hpp"
#include "daemon/single_send.hpp"
#include "daemon/write_contract.hpp"
#include "daemon/write_domain.hpp"
#include "daemon/write_kernel.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;
constexpr std::int64_t kMaximumScheduleWindow = 367LL * 86'400LL;

struct SendInput {
    std::string chat;
    std::string text;
    FingerprintParseMode parse_mode = FingerprintParseMode::Plain;
    std::optional<std::int64_t> reply_to;
    std::optional<TopicRef> requested_topic;
    bool silent = false;
    std::optional<SendSchedule> schedule;
};

struct DeleteInput {
    std::string chat;
    std::vector<std::int64_t> message_ids;
    bool for_all = false;
};

struct SendState {
    SendInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::optional<core::TdFormattedText> formatted_text;
    std::shared_ptr<const core::AuthStateSnapshot> dispatch_authorization;
};

struct DeleteState {
    DeleteInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::shared_ptr<const core::AuthStateSnapshot> dispatch_authorization;
};

bool exact_fields(const json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    return std::ranges::all_of(expected,
                               [&](const std::string& name) { return value.contains(name); });
}

std::optional<std::int64_t> integer64(const json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    if (value.is_number_unsigned()) {
        const auto parsed = value.get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(parsed);
    }
    return value.get<std::int64_t>();
}

bool nonzero_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

json terminal(std::string code, std::string message, json details, int exit_code) {
    return {{"kind", "error"},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"details", std::move(details)},
            {"exit_code", exit_code}};
}

json usage(std::string_view message, const json& argument,
           std::string_view reason = "invalid_argument") {
    return terminal("USAGE", std::string(message), {{"argument", argument}, {"reason", reason}},
                    kUsage);
}

json internal(proto::M3Operation operation, std::string_view message = "internal error") {
    const auto* identity = proto::m3_operation_identity(operation);
    return terminal("INTERNAL", std::string(message),
                    {{"operation", identity->canonical_name}, {"reason", "internal_error"}},
                    kGeneric);
}

json timeout(proto::M3Operation operation, std::string_view phase, std::string_view idempotency,
             std::string_view outcome = "not_started", std::optional<std::int64_t> temporary = {}) {
    const auto* identity = proto::m3_operation_identity(operation);
    json details{{"operation", identity->canonical_name},
                 {"phase", phase},
                 {"state", "ready"},
                 {"outcome", outcome},
                 {"idempotency", idempotency}};
    if (phase == "confirmation" && operation == proto::M3Operation::Send) {
        details["temporary_message_id"] = temporary ? json(*temporary) : json(nullptr);
    }
    return terminal("TIMEOUT", "request timed out", std::move(details), kTimeout);
}

std::string_view pre_intent_idempotency(const proto::Request& request) {
    return request.context.idempotency_key ? "not_created" : "not_requested";
}

std::string_view post_intent_idempotency(const proto::Request& request) {
    return request.context.idempotency_key ? "pending" : "not_requested";
}

json td_error_terminal(proto::M3Operation operation, const core::TdError& error) {
    const auto* identity = proto::m3_operation_identity(operation);
    if (error.code == 429) {
        return terminal("RATE_LIMITED", "Telegram rate limit exceeded",
                        {{"operation", identity->canonical_name},
                         {"tdlib_code", 429},
                         {"retry_after", parse_retry_after_seconds(error.message)}},
                        kRateLimited);
    }
    return terminal("TDLIB_ERROR", "Telegram request failed",
                    {{"operation", identity->canonical_name}, {"tdlib_code", error.code}},
                    kGeneric);
}

json not_authed_terminal(std::string_view account, core::AuthState state) {
    return terminal("NOT_AUTHED", "authorization was lost",
                    {{"account", account},
                     {"state", core::auth_state_name(state)},
                     {"reason", "authorization_lost"}},
                    kNotAuthed);
}

json precondition(proto::M3Operation operation, std::optional<std::int64_t> chat_id,
                  std::optional<std::int64_t> message_id, std::string_view reason) {
    const auto* identity = proto::m3_operation_identity(operation);
    return terminal("PRECONDITION_FAILED", "operation precondition failed",
                    {{"operation", identity->canonical_name},
                     {"chat_id", chat_id ? json(*chat_id) : json(nullptr)},
                     {"message_id", message_id ? json(*message_id) : json(nullptr)},
                     {"reason", reason}},
                    kGeneric);
}

std::string parse_mode_name(FingerprintParseMode mode) {
    switch (mode) {
    case FingerprintParseMode::Plain:
        return "plain";
    case FingerprintParseMode::MarkdownV2:
        return "markdown_v2";
    case FingerprintParseMode::Html:
        return "html";
    }
    return {};
}

json topic_json(const std::optional<TopicRef>& topic) {
    return topic ? topic_ref_json(*topic) : json(nullptr);
}

json schedule_json(const std::optional<SendSchedule>& schedule) {
    if (!schedule) {
        return nullptr;
    }
    if (schedule->kind == SendScheduleKind::Online) {
        return {{"kind", "online"}};
    }
    return {{"kind", "at"}, {"send_date", schedule->send_date}};
}

M3ScheduleKind admission_schedule_kind(const std::optional<SendSchedule>& schedule) {
    if (!schedule) {
        return M3ScheduleKind::None;
    }
    return schedule->kind == SendScheduleKind::Online ? M3ScheduleKind::Online : M3ScheduleKind::At;
}

std::optional<FingerprintSchedule>
fingerprint_schedule(const std::optional<SendSchedule>& schedule) {
    if (!schedule) {
        return std::nullopt;
    }
    if (schedule->kind == SendScheduleKind::Online) {
        return FingerprintScheduleOnline{};
    }
    return FingerprintScheduleAt{schedule->send_date};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed normalized send grammar.
std::optional<SendInput> parse_send_input(const json& args, json& failure) {
    static const std::set<std::string> fields{"chat",  "text",   "parse_mode", "reply_to",
                                              "topic", "silent", "schedule"};
    if (!exact_fields(args, fields) || !args["chat"].is_string() || !args["text"].is_string() ||
        !args["parse_mode"].is_string() || !args["silent"].is_boolean()) {
        failure = usage("send received malformed arguments", nullptr);
        return std::nullopt;
    }
    SendInput result;
    result.chat = args["chat"].get<std::string>();
    result.text = args["text"].get<std::string>();
    if (!valid_send_text(result.text)) {
        failure = usage("send text must contain between 1 and 4096 Unicode scalars", "TEXT");
        return std::nullopt;
    }
    const auto& mode = args["parse_mode"].get_ref<const std::string&>();
    if (mode == "plain") {
        result.parse_mode = FingerprintParseMode::Plain;
    } else if (mode == "markdown_v2") {
        result.parse_mode = FingerprintParseMode::MarkdownV2;
    } else if (mode == "html") {
        result.parse_mode = FingerprintParseMode::Html;
    } else {
        failure = usage("send parse mode is invalid", "--md/--html");
        return std::nullopt;
    }
    if (!args["reply_to"].is_null()) {
        const auto reply = integer64(args["reply_to"]);
        if (!reply || !nonzero_int53(*reply)) {
            failure = usage("--reply-to must be a nonzero int53 message id", "--reply-to");
            return std::nullopt;
        }
        result.reply_to = reply;
    }
    if (!args["topic"].is_null()) {
        if (!exact_fields(args["topic"], {"kind", "id"}) || args["topic"]["kind"] != "forum") {
            failure = usage("unsupported send topic kind", "--topic", "unsupported_topic_kind");
            return std::nullopt;
        }
        const auto id = integer64(args["topic"]["id"]);
        if (!id || *id <= 0 || *id > std::numeric_limits<std::int32_t>::max()) {
            failure = usage("send topic must be a positive int32", "--topic");
            return std::nullopt;
        }
        result.requested_topic = TopicRef{.kind = TopicKind::Forum, .id = *id};
    }
    result.silent = args["silent"].get<bool>();
    if (!args["schedule"].is_null()) {
        if (!args["schedule"].is_object() || !args["schedule"].contains("kind") ||
            !args["schedule"]["kind"].is_string()) {
            failure = usage("send schedule is invalid", "--schedule");
            return std::nullopt;
        }
        if (args["schedule"]["kind"] == "online" && exact_fields(args["schedule"], {"kind"})) {
            result.schedule = SendSchedule{.kind = SendScheduleKind::Online, .send_date = 0};
        } else if (args["schedule"]["kind"] == "at" &&
                   exact_fields(args["schedule"], {"kind", "send_date"})) {
            const auto date = integer64(args["schedule"]["send_date"]);
            if (!date || *date <= 0 || *date > std::numeric_limits<std::int32_t>::max()) {
                failure = usage("send schedule is invalid", "--schedule");
                return std::nullopt;
            }
            result.schedule = SendSchedule{.kind = SendScheduleKind::At,
                                           .send_date = static_cast<std::int32_t>(*date)};
        } else {
            failure = usage("send schedule is invalid", "--schedule");
            return std::nullopt;
        }
    }
    return result;
}

std::optional<DeleteInput> parse_delete_input(const json& args, json& failure) {
    if (!exact_fields(args, {"chat", "message_ids", "for_all"}) || !args["chat"].is_string() ||
        !args["message_ids"].is_array() || !args["for_all"].is_boolean()) {
        failure = usage("msg delete received malformed arguments", nullptr);
        return std::nullopt;
    }
    if (args["message_ids"].empty() || args["message_ids"].size() > 100) {
        failure = usage("msg delete requires between 1 and 100 message ids", "id");
        return std::nullopt;
    }
    DeleteInput result;
    result.chat = args["chat"].get<std::string>();
    result.for_all = args["for_all"].get<bool>();
    std::optional<std::int64_t> previous;
    for (const auto& item : args["message_ids"]) {
        const auto id = integer64(item);
        if (!id || !nonzero_int53(*id) || (previous && *id <= *previous)) {
            failure = usage("msg delete ids must be unique ascending nonzero int53 values", "id");
            return std::nullopt;
        }
        result.message_ids.push_back(*id);
        previous = id;
    }
    return result;
}

template <std::size_t Size> bool fill_random(std::array<unsigned char, Size>& bytes) {
    const int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(descriptor);
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
    return true;
}

std::string random_hex32() {
    std::array<unsigned char, 16> bytes{};
    if (!fill_random(bytes)) {
        return {};
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::int32_t random_sending_id() {
    std::array<unsigned char, 4> bytes{};
    if (!fill_random(bytes)) {
        return 0;
    }
    const auto value = static_cast<std::uint32_t>(bytes[0]) |
                       (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                       (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                       (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return static_cast<std::int32_t>((value & 0x7fffffffU) == 0 ? 1 : value & 0x7fffffffU);
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

std::uint64_t unix_seconds() {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds < 0 ? std::numeric_limits<std::uint64_t>::max()
                       : static_cast<std::uint64_t>(seconds);
}

std::optional<IdempotencyKeyHash> key_hash(const proto::Request& request) {
    if (!request.context.idempotency_key) {
        return std::nullopt;
    }
    return parse_idempotency_key_hash(idempotency_key_hash(*request.context.idempotency_key));
}

std::optional<IdempotencyRequestFingerprint> fingerprint(std::string_view account,
                                                         const ResolverPrincipal& principal,
                                                         const FingerprintPayload& payload) {
    const auto made = request_fingerprint(account, principal, payload);
    const auto* value = std::get_if<std::string>(&made);
    return value == nullptr ? std::nullopt
                            : parse_idempotency_request_fingerprint(std::string(*value));
}

write_contract::StoredTerminal stored_error(proto::M3Operation operation, std::string code,
                                            std::string message, json details, int exit_code) {
    std::string error;
    auto value = write_contract::make_error_terminal(operation, std::move(code), std::move(message),
                                                     std::move(details), exit_code, error);
    if (!value) {
        throw std::logic_error(error);
    }
    return std::move(*value);
}

write_contract::StoredTerminal stored_from_terminal(proto::M3Operation operation,
                                                    const json& value) {
    return stored_error(operation, value.at("code").get<std::string>(),
                        value.at("message").get<std::string>(), value.at("details"),
                        value.at("exit_code").get<int>());
}

write_contract::StoredTerminal stored_result(proto::M3Operation operation, json data) {
    std::string error;
    auto result = write_contract::make_result(operation, std::move(data), error);
    if (!result) {
        throw std::logic_error(error);
    }
    auto terminal_value = write_contract::make_result_terminal(*result, error);
    if (!terminal_value) {
        throw std::logic_error(error);
    }
    return std::move(*terminal_value);
}

json resolver_terminal_for_write(const ResolverError& error, proto::M3Operation operation,
                                 const proto::Request& request) {
    if (std::holds_alternative<ResolverTimeoutError>(error)) {
        return timeout(operation, "preflight", pre_intent_idempotency(request));
    }
    return resolver_error_terminal(error, M2Operation::Resolve);
}

using ReadOutcome = std::variant<core::TdValue, json>;

ReadOutcome read_value(ResolverConsumer& resolver, core::TdClient& client,
                       [[maybe_unused]] RequestSession& session, proto::M3Operation operation,
                       const proto::Request& request, const ReadyReadStart& start) {
    auto result = resolver.read_target(start);
    switch (result.status) {
    case ReadyReadStatus::Response:
        return std::move(result.value);
    case ReadyReadStatus::AuthorizationLost:
        return not_authed_terminal(request.account, result.snapshot ? result.snapshot->data.state
                                                                    : core::AuthState::Unknown);
    case ReadyReadStatus::TimedOut:
        return timeout(operation, "preflight", pre_intent_idempotency(request));
    case ReadyReadStatus::Cancelled:
        return json(nullptr);
    case ReadyReadStatus::Failed:
        static_cast<void>(client);
        return internal(operation);
    }
    return internal(operation);
}

json message_write_result_json(const core::TdMessageWriteResult& value) {
    const core::TdMessageSummary summary{.id = value.id,
                                         .chat_id = value.chat_id,
                                         .date = value.date.value_or(0),
                                         .sender = value.sender,
                                         .is_outgoing = value.is_outgoing,
                                         .topic = value.topic,
                                         .content_kind = value.content_kind,
                                         .text = value.text};
    auto materialized = materialize_message_summary(summary);
    if (!materialized || !persistable_message_summary(*materialized) ||
        value.scheduled != !value.date.has_value()) {
        return nullptr;
    }
    auto result = message_summary_json(*materialized);
    result["scheduled"] = value.scheduled;
    return result;
}

AccountAuditMutationState audit_state(SingleSendMutationState state) {
    switch (state) {
    case SingleSendMutationState::None:
        return AccountAuditMutationState::None;
    case SingleSendMutationState::Possible:
        return AccountAuditMutationState::Possible;
    case SingleSendMutationState::Confirmed:
        return AccountAuditMutationState::Confirmed;
    }
    return AccountAuditMutationState::Possible;
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

WriteConfirmationOutcome confirm_delete(const write_contract::Plan& plan, RequestSession& session) {
    const auto required = [&] {
        return terminal(
            "CONFIRMATION_REQUIRED", "message deletion was not confirmed",
            {{"account", plan.account()}, {"action", "msg_delete"}, {"target", plan.value()}},
            kDenied);
    };
    if (session.request().context.yes) {
        return {.status = WriteConfirmationStatus::ConfirmedYes, .terminal = std::nullopt};
    }
    if (!session.request().context.tty) {
        return {.status = WriteConfirmationStatus::Rejected, .terminal = required()};
    }
    const auto& chat = plan.value().at("chat");
    const auto count = plan.value().at("message_ids").size();
    auto answer = session.challenge({proto::ChallengeKind::DestructiveConfirmation,
                                     std::nullopt,
                                     std::nullopt,
                                     "Delete " + std::to_string(count) + " message(s) from \"" +
                                         chat.at("title").get<std::string>() + "\" (" +
                                         chat.at("id").dump() + ")? [y/N] ",
                                     {{"action", "msg_delete"}, {"target", plan.value()}},
                                     false});
    const auto confirmed = answer.take_boolean();
    if (answer.status() == ChallengeStatus::Answered && confirmed.value_or(false)) {
        return {.status = WriteConfirmationStatus::ConfirmedTty, .terminal = std::nullopt};
    }
    if (answer.status() == ChallengeStatus::TimedOut) {
        return {.status = WriteConfirmationStatus::TimedOut, .terminal = std::nullopt};
    }
    if (answer.status() == ChallengeStatus::Cancelled ||
        answer.status() == ChallengeStatus::Disconnected ||
        answer.status() == ChallengeStatus::Shutdown) {
        return {.status = WriteConfirmationStatus::Cancelled, .terminal = required()};
    }
    return {.status = WriteConfirmationStatus::Rejected, .terminal = required()};
}

std::optional<AuthoritySource> authorize(const proto::Request& request, RequestSession& session,
                                         std::string_view account, proto::M3Operation operation) {
    const auto* identity = proto::m3_operation_identity(operation);
    const auto& admitted = session.admitted_config();
    if (!admitted || admitted->account != account || !admitted->account_snapshot) {
        session.error("INTERNAL", "write config admission is missing",
                      {{"operation", identity->canonical_name}, {"reason", "internal_error"}},
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
                      {{"operation", identity->canonical_name}, {"reason", "internal_error"}},
                      kGeneric);
        return std::nullopt;
    }
    return granted->source;
}

WriteKernelRequest kernel_request(const proto::Request& request, RequestSession& session,
                                  proto::M3Operation operation, AuthoritySource source,
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

} // namespace

WriteCoordinator::WriteCoordinator(core::TdClient& client, std::string account,
                                   std::string config_path, uid_t expected_uid,
                                   std::shared_ptr<IdempotencyFoundation> foundation,
                                   std::function<void()> audit_fatal_shutdown)
    : client_(client), account_(std::move(account)),
      config_store_(std::move(config_path), expected_uid), foundation_(std::move(foundation)),
      audit_fatal_shutdown_(std::move(audit_fatal_shutdown)) {}

// NOLINTBEGIN(readability-function-cognitive-complexity): exact two-epoch send transaction.
void WriteCoordinator::send(const proto::Request& request, RequestSession& session) {
    json parse_failure;
    auto input = parse_send_input(request.args, parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(proto::M3Operation::Send);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session,
                      resolver_terminal_for_write(*error, proto::M3Operation::Send, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    const auto schedule_policy = admission_schedule_kind(input->schedule);
    if (evaluate_m3_bot_admission(proto::M3Operation::Send, principal.is_bot, schedule_policy) !=
        M3BotAdmission::Allowed) {
        session.error("BOT_UNSUPPORTED", "scheduled send requires a user account",
                      {{"operation", "send"}}, kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, proto::M3Operation::Send);
    if (!authority) {
        return;
    }
    if (!request.context.dry_run &&
        session.begin_audited_terminal() != AuditedTerminalStatus::Designated) {
        return;
    }
    auto hash = key_hash(request);
    if (request.context.idempotency_key && !hash) {
        session.error("INTERNAL", "cannot hash idempotency key",
                      {{"operation", "send"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    auto state = std::make_shared<SendState>(SendState{.input = std::move(*input),
                                                       .principal = principal,
                                                       .target = std::nullopt,
                                                       .formatted_text = std::nullopt,
                                                       .dispatch_authorization = nullptr});
    auto invocation = request.context.dry_run ? std::string{} : random_hex32();
    if (!request.context.dry_run && invocation.empty()) {
        session.error("AUDIT_UNAVAILABLE", "cannot create audit identity",
                      {{"account", account_},
                       {"path", foundation_ ? foundation_->audit().path() : std::string{}},
                       {"reason", "open_failed"}},
                      kDenied);
        return;
    }
    const WriteKernel kernel(foundation_);
    auto kernel_input =
        kernel_request(request, session, proto::M3Operation::Send, *authority, std::move(hash),
                       std::move(invocation), config_store_.path());
    WriteKernelHooks hooks;
    hooks.admit = [state, this]() -> WriteAdmissionOutcome {
        const auto fingerprint_value = fingerprint(
            account_, state->principal,
            SendFingerprintPayload{.chat_selector = state->input.chat,
                                   .text = state->input.text,
                                   .parse_mode = state->input.parse_mode,
                                   .reply_to = state->input.reply_to,
                                   .requested_topic = state->input.requested_topic,
                                   .silent = state->input.silent,
                                   .schedule = fingerprint_schedule(state->input.schedule)});
        std::string error;
        auto arguments = write_contract::make_arguments(
            proto::M3Operation::Send,
            {{"chat", state->input.chat},
             {"text", state->input.text},
             {"parse_mode", parse_mode_name(state->input.parse_mode)},
             {"reply_to", state->input.reply_to ? json(*state->input.reply_to) : json(nullptr)},
             {"topic", topic_json(state->input.requested_topic)},
             {"silent", state->input.silent},
             {"schedule", schedule_json(state->input.schedule)}},
            error);
        if (!fingerprint_value || !arguments) {
            return internal(proto::M3Operation::Send);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    hooks.plan = [state, &resolver, &session, &request,
                  this](const WriteAdmission&) -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, proto::M3Operation::Send, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        std::optional<TopicRef> effective_topic = state->input.requested_topic;
        if (state->input.reply_to) {
            auto message_read =
                read_value(resolver, client_.get(), session, proto::M3Operation::Send, request,
                           [&](const auto& current) {
                               return client_.get().get_message(current, state->target->chat.id,
                                                                *state->input.reply_to);
                           });
            if (auto* read_failure = std::get_if<json>(&message_read)) {
                return std::move(*read_failure);
            }
            auto& message_value = std::get<core::TdValue>(message_read);
            if (const auto* error = message_value.get_if<core::TdError>()) {
                if (error->code == 404) {
                    return terminal("NOT_FOUND", "reply message was not found",
                                    {{"chat_id", state->target->chat.id},
                                     {"message_id", *state->input.reply_to}},
                                    kNotFound);
                }
                return td_error_terminal(proto::M3Operation::Send, *error);
            }
            const auto* message = message_value.get_if<core::TdPlanningMessage>();
            if (message == nullptr || message->chat_id != state->target->chat.id ||
                message->id != *state->input.reply_to) {
                return internal(proto::M3Operation::Send);
            }
            auto properties_read =
                read_value(resolver, client_.get(), session, proto::M3Operation::Send, request,
                           [&](const auto& current) {
                               return client_.get().get_message_properties(
                                   current, state->target->chat.id, *state->input.reply_to);
                           });
            if (auto* read_failure = std::get_if<json>(&properties_read)) {
                return std::move(*read_failure);
            }
            auto& properties_value = std::get<core::TdValue>(properties_read);
            if (const auto* error = properties_value.get_if<core::TdError>()) {
                return td_error_terminal(proto::M3Operation::Send, *error);
            }
            const auto* properties = properties_value.get_if<core::TdMessageProperties>();
            if (properties == nullptr) {
                return internal(proto::M3Operation::Send);
            }
            if (!properties->can_be_replied) {
                return precondition(proto::M3Operation::Send, state->target->chat.id,
                                    state->input.reply_to, "not_replyable");
            }
            std::optional<TopicRef> reply_topic;
            if (message->topic) {
                reply_topic = materialize_topic_ref(*message->topic);
                if (!reply_topic || reply_topic->kind != TopicKind::Forum) {
                    return precondition(proto::M3Operation::Send, state->target->chat.id,
                                        state->input.reply_to, "wrong_topic");
                }
            }
            if (state->input.requested_topic) {
                if (!reply_topic || *reply_topic != *state->input.requested_topic) {
                    return precondition(proto::M3Operation::Send, state->target->chat.id,
                                        state->input.reply_to, "wrong_topic");
                }
            } else {
                effective_topic = reply_topic;
            }
        }
        if (state->input.parse_mode == FingerprintParseMode::Plain) {
            state->formatted_text =
                core::TdFormattedText{.text = state->input.text, .entities = {}, .capability = {}};
        } else {
            const auto mode = state->input.parse_mode == FingerprintParseMode::MarkdownV2
                                  ? core::TdTextParseMode::MarkdownV2
                                  : core::TdTextParseMode::Html;
            auto parsed_read = read_value(
                resolver, client_.get(), session, proto::M3Operation::Send, request,
                [&](const auto& current) {
                    return client_.get().parse_text_entities(current, state->input.text, mode);
                });
            if (auto* read_failure = std::get_if<json>(&parsed_read)) {
                return std::move(*read_failure);
            }
            auto& parsed_value = std::get<core::TdValue>(parsed_read);
            if (const auto* error = parsed_value.get_if<core::TdError>()) {
                return td_error_terminal(proto::M3Operation::Send, *error);
            }
            auto* formatted = parsed_value.get_if<core::TdFormattedText>();
            if (formatted == nullptr || !core::valid_td_formatted_text_facts(*formatted)) {
                return internal(proto::M3Operation::Send);
            }
            state->formatted_text = std::move(*formatted);
        }
        std::optional<std::int64_t> observed_server_time;
        if (state->input.schedule && state->input.schedule->kind == SendScheduleKind::Online) {
            if (state->target->chat.type != "private" || state->target->chat.is_bot ||
                !state->target->private_user_id ||
                *state->target->private_user_id == state->principal.id ||
                !state->target->private_user_presence ||
                *state->target->private_user_presence == core::TdUserPresence::Hidden) {
                return precondition(proto::M3Operation::Send, state->target->chat.id, std::nullopt,
                                    "online_schedule_unsupported");
            }
        } else if (state->input.schedule) {
            auto time_read = read_value(
                resolver, client_.get(), session, proto::M3Operation::Send, request,
                [&](const auto& current) { return client_.get().get_unix_time(current); });
            if (auto* read_failure = std::get_if<json>(&time_read)) {
                return std::move(*read_failure);
            }
            auto& time_value = std::get<core::TdValue>(time_read);
            if (const auto* error = time_value.get_if<core::TdError>()) {
                return td_error_terminal(proto::M3Operation::Send, *error);
            }
            const auto* server = time_value.get_if<core::TdOptionInteger>();
            if (server == nullptr) {
                return internal(proto::M3Operation::Send);
            }
            observed_server_time = server->value;
            const auto delta =
                static_cast<std::int64_t>(state->input.schedule->send_date) - server->value;
            if (delta <= 10) {
                return precondition(proto::M3Operation::Send, state->target->chat.id, std::nullopt,
                                    "schedule_window_elapsed");
            }
            if (delta > kMaximumScheduleWindow) {
                return precondition(proto::M3Operation::Send, state->target->chat.id, std::nullopt,
                                    "schedule_too_far");
            }
        }
        std::string error;
        auto plan = write_contract::make_plan(
            proto::M3Operation::Send, account_,
            {{"operation", "send"},
             {"account", account_},
             {"tdlib_request", "sendMessage"},
             {"chat", chat_identity_json(state->target->chat)},
             {"text", state->input.text},
             {"parse_mode", parse_mode_name(state->input.parse_mode)},
             {"reply_to", state->input.reply_to ? json(*state->input.reply_to) : json(nullptr)},
             {"requested_topic", topic_json(state->input.requested_topic)},
             {"effective_topic", topic_json(effective_topic)},
             {"silent", state->input.silent},
             {"schedule", schedule_json(state->input.schedule)},
             {"observed_server_unix_time",
              observed_server_time ? json(*observed_server_time) : json(nullptr)}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(proto::M3Operation::Send)};
    };
    hooks.verify_config_grant = [this](std::string_view expected, std::string_view account,
                                       const config::MutationControl& control) {
        return config_store_.verify_write_grant(expected, account, control);
    };
    hooks.revalidate_auth_and_schedule =
        [state, &resolver, &session, &request,
         this](const write_contract::Plan& plan) -> WriteDispatchAdmissionOutcome {
        auto current = client_.get().auth_state();
        if (!current || current->data.state != core::AuthState::Ready) {
            const auto stopped = not_authed_terminal(account_, current ? current->data.state
                                                                       : core::AuthState::Unknown);
            return stored_from_terminal(proto::M3Operation::Send, stopped);
        }
        if (deadline_expired(session.deadline())) {
            return stored_from_terminal(
                proto::M3Operation::Send,
                timeout(proto::M3Operation::Send, "preflight",
                        request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (state->input.schedule && state->input.schedule->kind == SendScheduleKind::At) {
            auto time_read = read_value(resolver, client_.get(), session, proto::M3Operation::Send,
                                        request, [&](const auto& authorization) {
                                            return client_.get().get_unix_time(authorization);
                                        });
            if (auto* failure = std::get_if<json>(&time_read)) {
                if (!failure->is_object()) {
                    return WriteDispatchStopped{};
                }
                return stored_from_terminal(proto::M3Operation::Send, *failure);
            }
            auto& value = std::get<core::TdValue>(time_read);
            if (const auto* error = value.get_if<core::TdError>()) {
                return stored_from_terminal(proto::M3Operation::Send,
                                            td_error_terminal(proto::M3Operation::Send, *error));
            }
            const auto* server = value.get_if<core::TdOptionInteger>();
            if (server == nullptr) {
                throw std::runtime_error("malformed unix_time option during schedule recheck");
            }
            const auto delta =
                static_cast<std::int64_t>(state->input.schedule->send_date) - server->value;
            if (delta <= 10 || delta > kMaximumScheduleWindow) {
                const char* const reason =
                    delta <= 10 ? "schedule_window_elapsed" : "schedule_too_far";
                return stored_from_terminal(proto::M3Operation::Send,
                                            precondition(proto::M3Operation::Send,
                                                         plan.value()["chat"]["id"], std::nullopt,
                                                         reason));
            }
        }
        current = client_.get().auth_state();
        if (!current || current->data.state != core::AuthState::Ready) {
            const auto stopped = not_authed_terminal(account_, current ? current->data.state
                                                                       : core::AuthState::Unknown);
            return stored_from_terminal(proto::M3Operation::Send, stopped);
        }
        if (deadline_expired(session.deadline())) {
            return stored_from_terminal(
                proto::M3Operation::Send,
                timeout(proto::M3Operation::Send, "preflight",
                        request.context.idempotency_key ? "removed" : "not_requested"));
        }
        state->dispatch_authorization = std::move(current);
        const auto token = random_hex32();
        if (token.empty()) {
            throw std::runtime_error("cannot create dispatch token");
        }
        return WriteDispatchPreparation{
            .proof = {{"tdlib_function", "sendMessage"},
                      {"dispatch_token", token},
                      {"client_generation", state->dispatch_authorization->client_generation}}};
    };
    hooks.dispatch = [state, &session, &request,
                      this](const write_contract::Plan& plan, const WriteDispatchPreparation&,
                            WriteDurableObservationSink& observations) -> WriteDispatchOutcome {
        if (!state->target || !state->formatted_text || !state->dispatch_authorization) {
            throw std::logic_error("send dispatch state is incomplete");
        }
        const auto sending_id = random_sending_id();
        if (sending_id == 0) {
            throw std::runtime_error("cannot create sending id");
        }
        std::optional<core::TdTopic> topic;
        if (!plan.value()["effective_topic"].is_null()) {
            topic = core::TdTopic{.kind = core::TdTopicKind::Forum,
                                  .id = plan.value()["effective_topic"]["id"].get<std::int64_t>(),
                                  .tdlib_type_id = 0};
        }
        core::TdSendSchedule schedule;
        if (state->input.schedule) {
            schedule = state->input.schedule->kind == SendScheduleKind::Online
                           ? core::TdSendSchedule{.kind = core::TdSendScheduleKind::WhenOnline,
                                                  .send_date = 0}
                           : core::TdSendSchedule{.kind = core::TdSendScheduleKind::AtDate,
                                                  .send_date = state->input.schedule->send_date};
        }
        core::TdSendMessageRequest td_request{
            .chat_id = state->target->chat.id,
            .topic = topic,
            .reply_to_message_id = state->input.reply_to,
            .options = {.disable_notification = state->input.silent,
                        .schedule = schedule,
                        .sending_id = sending_id},
            .content = {.formatted_text = std::move(*state->formatted_text),
                        .parsed = state->input.parse_mode != FingerprintParseMode::Plain}};
        SingleSendHooks send_hooks{};
        send_hooks.on_temporary_id = [&](const SingleSendTemporaryId& temporary) {
            if (!observations.temporary_message_ids(
                    json::array({temporary.temporary_message_id}))) {
                throw std::runtime_error("temporary id was not durable");
            }
        };
        SingleSendCoordinator coordinator(client_.get(), session, std::move(send_hooks));
        auto selected = coordinator.execute(std::move(td_request), state->dispatch_authorization);
        return std::visit(
            [&](auto&& outcome) -> WriteDispatchOutcome {
                using Outcome = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<Outcome, SingleSendSucceeded>) {
                    auto result = message_write_result_json(outcome.result);
                    if (!result.is_object() || result["chat_id"] != state->target->chat.id) {
                        return {.terminal =
                                    stored_from_terminal(proto::M3Operation::Send,
                                                         internal(proto::M3Operation::Send,
                                                                  "TDLib returned data outside the "
                                                                  "supported persistence bounds")),
                                .mutation_state = AccountAuditMutationState::Confirmed,
                                .mutation_confirmed = true};
                    }
                    return {.terminal = stored_result(proto::M3Operation::Send, std::move(result)),
                            .mutation_state = AccountAuditMutationState::Confirmed,
                            .mutation_confirmed = true};
                } else if constexpr (std::is_same_v<Outcome, SingleSendFailed>) {
                    return {.terminal = stored_from_terminal(
                                proto::M3Operation::Send,
                                td_error_terminal(proto::M3Operation::Send, outcome.error)),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendRateLimited>) {
                    auto value = terminal("RATE_LIMITED", "Telegram rate limit exceeded",
                                          {{"operation", "send"},
                                           {"tdlib_code", 429},
                                           {"retry_after", outcome.retry_after}},
                                          kRateLimited);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendDeletedBeforeConfirmation>) {
                    auto value =
                        terminal("SEND_FAILED", "message was deleted before confirmation",
                                 {{"operation", "send"},
                                  {"chat_id", outcome.temporary.chat_id},
                                  {"temporary_message_id", outcome.temporary.temporary_message_id},
                                  {"reason", "deleted_before_confirmation"}},
                                 kGeneric);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendTimedOut>) {
                    auto value = timeout(
                        proto::M3Operation::Send, "confirmation", post_intent_idempotency(request),
                        "unknown",
                        outcome.temporary
                            ? std::optional<std::int64_t>{outcome.temporary->temporary_message_id}
                            : std::nullopt);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendAuthorizationLost>) {
                    auto value = not_authed_terminal(account_, outcome.state);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendGenerationClosed>) {
                    auto value = not_authed_terminal(account_, core::AuthState::Closed);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendCancelled>) {
                    auto value = not_authed_terminal(account_, core::AuthState::Ready);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendRejected>) {
                    auto value = not_authed_terminal(account_, core::AuthState::Ready);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else {
                    return {
                        .terminal = stored_from_terminal(
                            proto::M3Operation::Send,
                            internal(
                                proto::M3Operation::Send,
                                "TDLib returned data outside the supported persistence bounds")),
                        .mutation_state = AccountAuditMutationState::Possible,
                        .mutation_confirmed = false};
                }
            },
            std::move(selected));
    };
    hooks.timestamp = timestamp;
    hooks.audit_fatal_shutdown = [&session, this] {
        session.audit_fatal();
        if (audit_fatal_shutdown_) {
            audit_fatal_shutdown_();
        }
    };
    const auto result = kernel.run(kernel_input, hooks);
    if (result.status == WriteKernelStatus::DryRunPlanned && result.plan) {
        session.result({{"dry_run", true}, {"plan", result.plan->value()}});
    } else if (result.terminal) {
        emit_terminal(session, *result.terminal);
        if (result.status == WriteKernelStatus::DurabilityFatal && audit_fatal_shutdown_) {
            audit_fatal_shutdown_();
        }
    }
}
// NOLINTEND(readability-function-cognitive-complexity)

// NOLINTBEGIN(readability-function-cognitive-complexity): exact two-epoch delete transaction.
void WriteCoordinator::delete_messages(const proto::Request& request, RequestSession& session) {
    json parse_failure;
    auto input = parse_delete_input(request.args, parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(proto::M3Operation::MsgDelete);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session,
                      resolver_terminal_for_write(*error, proto::M3Operation::MsgDelete, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    const auto authority = authorize(request, session, account_, proto::M3Operation::MsgDelete);
    if (!authority) {
        return;
    }
    if (!request.context.dry_run &&
        session.begin_audited_terminal() != AuditedTerminalStatus::Designated) {
        return;
    }
    auto hash = key_hash(request);
    if (request.context.idempotency_key && !hash) {
        session.error("INTERNAL", "cannot hash idempotency key",
                      {{"operation", "msg_delete"}, {"reason", "internal_error"}}, kGeneric);
        return;
    }
    auto state = std::make_shared<DeleteState>(DeleteState{.input = std::move(*input),
                                                           .principal = principal,
                                                           .target = std::nullopt,
                                                           .dispatch_authorization = nullptr});
    auto invocation = request.context.dry_run ? std::string{} : random_hex32();
    if (!request.context.dry_run && invocation.empty()) {
        session.error("AUDIT_UNAVAILABLE", "cannot create audit identity",
                      {{"account", account_},
                       {"path", foundation_ ? foundation_->audit().path() : std::string{}},
                       {"reason", "open_failed"}},
                      kDenied);
        return;
    }
    const WriteKernel kernel(foundation_);
    auto kernel_input =
        kernel_request(request, session, proto::M3Operation::MsgDelete, *authority, std::move(hash),
                       std::move(invocation), config_store_.path());
    WriteKernelHooks hooks;
    hooks.admit = [state, this]() -> WriteAdmissionOutcome {
        const auto fingerprint_value =
            fingerprint(account_, state->principal,
                        MsgDeleteFingerprintPayload{.chat_selector = state->input.chat,
                                                    .message_ids = state->input.message_ids,
                                                    .for_all = state->input.for_all});
        std::string error;
        auto arguments = write_contract::make_arguments(proto::M3Operation::MsgDelete,
                                                        {{"chat", state->input.chat},
                                                         {"message_ids", state->input.message_ids},
                                                         {"for_all", state->input.for_all}},
                                                        error);
        if (!fingerprint_value || !arguments) {
            return internal(proto::M3Operation::MsgDelete);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    hooks.plan = [state, &resolver, &session, &request,
                  this](const WriteAdmission&) -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, proto::M3Operation::MsgDelete, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        const bool forced_revoke =
            state->target->chat.type == "supergroup" || state->target->chat.type == "channel";
        if (forced_revoke && !state->input.for_all) {
            return precondition(proto::M3Operation::MsgDelete, state->target->chat.id, std::nullopt,
                                "not_deletable_for_all");
        }
        const bool effective_for_all = forced_revoke || state->input.for_all;
        for (const auto message_id : state->input.message_ids) {
            auto message_read = read_value(
                resolver, client_.get(), session, proto::M3Operation::MsgDelete, request,
                [&](const auto& current) {
                    return client_.get().get_message(current, state->target->chat.id, message_id);
                });
            if (auto* failure = std::get_if<json>(&message_read)) {
                return std::move(*failure);
            }
            auto& message_value = std::get<core::TdValue>(message_read);
            if (const auto* error = message_value.get_if<core::TdError>()) {
                if (error->code == 404) {
                    return terminal(
                        "NOT_FOUND", "message was not found",
                        {{"chat_id", state->target->chat.id}, {"message_id", message_id}},
                        kNotFound);
                }
                return td_error_terminal(proto::M3Operation::MsgDelete, *error);
            }
            const auto* message = message_value.get_if<core::TdPlanningMessage>();
            if (message == nullptr || message->chat_id != state->target->chat.id ||
                message->id != message_id) {
                return internal(proto::M3Operation::MsgDelete);
            }
            auto properties_read =
                read_value(resolver, client_.get(), session, proto::M3Operation::MsgDelete, request,
                           [&](const auto& current) {
                               return client_.get().get_message_properties(
                                   current, state->target->chat.id, message_id);
                           });
            if (auto* failure = std::get_if<json>(&properties_read)) {
                return std::move(*failure);
            }
            auto& properties_value = std::get<core::TdValue>(properties_read);
            if (const auto* error = properties_value.get_if<core::TdError>()) {
                return td_error_terminal(proto::M3Operation::MsgDelete, *error);
            }
            const auto* properties = properties_value.get_if<core::TdMessageProperties>();
            if (properties == nullptr) {
                return internal(proto::M3Operation::MsgDelete);
            }
            if (effective_for_all ? !properties->can_be_deleted_for_all_users
                                  : !properties->can_be_deleted_only_for_self) {
                return precondition(
                    proto::M3Operation::MsgDelete, state->target->chat.id, message_id,
                    effective_for_all ? "not_deletable_for_all" : "not_deletable_for_self");
            }
        }
        std::string error;
        auto plan = write_contract::make_plan(proto::M3Operation::MsgDelete, account_,
                                              {{"operation", "msg_delete"},
                                               {"account", account_},
                                               {"tdlib_request", "deleteMessages"},
                                               {"chat", chat_identity_json(state->target->chat)},
                                               {"message_ids", state->input.message_ids},
                                               {"requested_for_all", state->input.for_all},
                                               {"effective_for_all", effective_for_all}},
                                              error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(proto::M3Operation::MsgDelete)};
    };
    hooks.confirm = [&session](const write_contract::Plan& plan, bool) {
        return confirm_delete(plan, session);
    };
    hooks.verify_config_grant = [this](std::string_view expected, std::string_view account,
                                       const config::MutationControl& control) {
        return config_store_.verify_write_grant(expected, account, control);
    };
    hooks.revalidate_auth_and_schedule =
        [state, this](const write_contract::Plan&) -> WriteDispatchAdmissionOutcome {
        auto current = client_.get().auth_state();
        if (!current || current->data.state != core::AuthState::Ready) {
            return stored_from_terminal(
                proto::M3Operation::MsgDelete,
                not_authed_terminal(account_,
                                    current ? current->data.state : core::AuthState::Unknown));
        }
        state->dispatch_authorization = std::move(current);
        const auto token = random_hex32();
        if (token.empty()) {
            throw std::runtime_error("cannot create dispatch token");
        }
        return WriteDispatchPreparation{
            .proof = {{"tdlib_function", "deleteMessages"},
                      {"dispatch_token", token},
                      {"client_generation", state->dispatch_authorization->client_generation}}};
    };
    hooks.dispatch = [state, &session, &request,
                      this](const write_contract::Plan& plan, const WriteDispatchPreparation&,
                            WriteDurableObservationSink&) -> WriteDispatchOutcome {
        if (!state->target || !state->dispatch_authorization) {
            throw std::logic_error("delete dispatch state is incomplete");
        }
        const core::TdDeleteMessagesRequest td_request{
            .chat_id = state->target->chat.id,
            .message_ids = state->input.message_ids,
            .revoke = plan.value()["effective_for_all"].get<bool>()};
        DirectRpcCoordinator coordinator(client_.get(), session);
        auto selected =
            coordinator.execute(core::TdDirectRequest{td_request}, state->dispatch_authorization);
        return std::visit(
            [&](auto&& outcome) -> WriteDispatchOutcome {
                using Outcome = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<Outcome, DirectSuccess>) {
                    const auto* result = std::get_if<DirectDeleteResult>(&outcome.result);
                    if (result == nullptr || result->chat_id != state->target->chat.id ||
                        result->message_ids != state->input.message_ids ||
                        result->for_all != plan.value()["effective_for_all"].get<bool>()) {
                        return {.terminal =
                                    stored_from_terminal(proto::M3Operation::MsgDelete,
                                                         internal(proto::M3Operation::MsgDelete)),
                                .mutation_state = AccountAuditMutationState::Possible,
                                .mutation_confirmed = false};
                    }
                    return {.terminal = stored_result(proto::M3Operation::MsgDelete,
                                                      {{"chat_id", result->chat_id},
                                                       {"message_ids", result->message_ids},
                                                       {"for_all", result->for_all},
                                                       {"deleted", true}}),
                            .mutation_state = AccountAuditMutationState::Confirmed,
                            .mutation_confirmed = true};
                } else if constexpr (std::is_same_v<Outcome, DirectTdError>) {
                    return {.terminal = stored_from_terminal(
                                proto::M3Operation::MsgDelete,
                                td_error_terminal(proto::M3Operation::MsgDelete, outcome.error)),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectAuthorizationLost>) {
                    const auto state_value =
                        outcome.snapshot ? outcome.snapshot->data.state : core::AuthState::Unknown;
                    return {.terminal =
                                stored_from_terminal(proto::M3Operation::MsgDelete,
                                                     not_authed_terminal(account_, state_value)),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectTimedOut>) {
                    return {.terminal = stored_from_terminal(
                                proto::M3Operation::MsgDelete,
                                timeout(proto::M3Operation::MsgDelete, "dispatch",
                                        post_intent_idempotency(request), "unknown")),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectMalformed>) {
                    return {.terminal =
                                stored_from_terminal(proto::M3Operation::MsgDelete,
                                                     internal(proto::M3Operation::MsgDelete)),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                } else {
                    return {.terminal = stored_from_terminal(
                                proto::M3Operation::MsgDelete,
                                not_authed_terminal(account_, core::AuthState::Ready)),
                            .mutation_state = AccountAuditMutationState::Possible,
                            .mutation_confirmed = false};
                }
            },
            std::move(selected));
    };
    hooks.timestamp = timestamp;
    hooks.audit_fatal_shutdown = [&session, this] {
        session.audit_fatal();
        if (audit_fatal_shutdown_) {
            audit_fatal_shutdown_();
        }
    };
    const auto result = kernel.run(kernel_input, hooks);
    if (result.status == WriteKernelStatus::DryRunPlanned && result.plan) {
        session.result({{"dry_run", true}, {"plan", result.plan->value()}});
    } else if (result.terminal) {
        emit_terminal(session, *result.terminal);
        if (result.status == WriteKernelStatus::DurabilityFatal && audit_fatal_shutdown_) {
            audit_fatal_shutdown_();
        }
    }
}
// NOLINTEND(readability-function-cognitive-complexity)

void register_write_commands(Dispatcher& dispatcher, WriteCoordinator& coordinator) {
    dispatcher.register_command(
        "send", {Tier::Write,
                 [&coordinator](const proto::Request& request, RequestSession& session) {
                     coordinator.send(request, session);
                 },
                 false, proto::M3Operation::Send});
    dispatcher.register_command(
        "msg delete", {Tier::Destructive,
                       [&coordinator](const proto::Request& request, RequestSession& session) {
                           coordinator.delete_messages(request, session);
                       },
                       false, proto::M3Operation::MsgDelete});
}

} // namespace tgcli::daemon
