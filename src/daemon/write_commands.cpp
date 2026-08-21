#include "daemon/write_commands.hpp"

#include "common/exit_codes.hpp"
#include "common/invite_redaction.hpp"
#include "common/secure_wipe.hpp"
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

enum class ScheduleWindowStatus { Allowed, Elapsed, TooFar };

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

struct EditInput {
    std::string chat;
    std::int64_t message_id = 0;
    std::string text;
};

struct ReactInput {
    std::string chat;
    std::int64_t message_id = 0;
    std::string reaction;
    bool remove = false;
    bool big = false;
};

struct MessagePinInput {
    std::string chat;
    std::int64_t message_id = 0;
    bool pinned = false;
};

struct ChatTargetInput {
    std::string chat;
};

struct ChatMuteInput {
    std::string chat;
    std::int32_t duration_seconds = 0;
    bool muted = false;
};

struct ChatJoinInput {
    std::string target;
    bool invite = false;
    std::string invite_hash;
};

struct SendState {
    SendInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::optional<core::TdFormattedText> formatted_text;
    std::shared_ptr<const core::AuthStateSnapshot> dispatch_authorization;
    std::unique_ptr<SingleSendCoordinator> coordinator;
    WriteDurableObservationSink* observations = nullptr;
};

struct DeleteState {
    DeleteInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::shared_ptr<const core::AuthStateSnapshot> dispatch_authorization;
    std::unique_ptr<DirectRpcCoordinator> coordinator;
};

struct DirectDispatchState {
    std::optional<core::TdDirectRequest> request;
    std::shared_ptr<const core::AuthStateSnapshot> authorization;
    std::unique_ptr<DirectRpcCoordinator> coordinator;
};

struct EditState {
    EditInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::shared_ptr<DirectDispatchState> dispatch;
};

struct ReactState {
    ReactInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::shared_ptr<DirectDispatchState> dispatch;
};

struct MessagePinState {
    MessagePinInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::shared_ptr<DirectDispatchState> dispatch;
};

struct ChatTargetState {
    ChatTargetInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::shared_ptr<DirectDispatchState> dispatch;
};

struct ChatMuteState {
    ChatMuteInput input;
    ResolverPrincipal principal;
    std::optional<ResolvedChatTarget> target;
    std::shared_ptr<DirectDispatchState> dispatch;
};

struct ChatJoinState {
    ChatJoinInput input;
    ResolverPrincipal principal;
    std::optional<ChatIdentity> chat;
    std::shared_ptr<DirectDispatchState> dispatch;
};

struct DirectWriteDefinition {
    proto::M3Operation operation = proto::M3Operation::MsgEdit;
    std::function<WriteAdmissionOutcome()> admit;
    std::function<WritePlanningOutcome()> plan;
    std::function<std::optional<write_contract::StoredTerminal>(const write_contract::Plan&,
                                                                const DirectResult&)>
        success;
    std::function<WriteConfirmationOutcome(const write_contract::Plan&, bool)> confirm;
    std::function<WritePostIntentPreparation(const write_contract::Plan&, const WriteAdmission&)>
        post_intent;
    std::shared_ptr<DirectDispatchState> dispatch;
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

json shutdown_terminal() {
    return terminal("DAEMON_SHUTDOWN", "daemon is shutting down", {{"reason", "daemon_shutdown"}},
                    kGeneric);
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

ScheduleWindowStatus schedule_window_status(std::int32_t send_date, std::int64_t server_time) {
    const auto date = static_cast<std::int64_t>(send_date);
    if (server_time >= date - 10) {
        return ScheduleWindowStatus::Elapsed;
    }
    if (server_time < date - kMaximumScheduleWindow) {
        return ScheduleWindowStatus::TooFar;
    }
    return ScheduleWindowStatus::Allowed;
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

std::optional<EditInput> parse_edit_input(const json& args, json& failure) {
    if (!exact_fields(args, {"chat", "message_id", "text"}) || !args["chat"].is_string() ||
        !args["text"].is_string()) {
        failure = usage("msg edit received malformed arguments", nullptr);
        return std::nullopt;
    }
    const auto message_id = integer64(args["message_id"]);
    if (!message_id || !nonzero_int53(*message_id)) {
        failure = usage("msg edit message id must be a nonzero int53 value", "id");
        return std::nullopt;
    }
    EditInput input{.chat = args["chat"].get<std::string>(),
                    .message_id = *message_id,
                    .text = args["text"].get<std::string>()};
    if (!valid_send_text(input.text)) {
        failure = usage("msg edit text must contain between 1 and 4096 Unicode scalars", "TEXT");
        return std::nullopt;
    }
    return input;
}

std::optional<ReactInput> parse_react_input(const json& args, json& failure) {
    if (!exact_fields(args, {"chat", "message_id", "reaction", "remove", "big"}) ||
        !args["chat"].is_string() || !args["reaction"].is_string() ||
        !args["remove"].is_boolean() || !args["big"].is_boolean()) {
        failure = usage("msg react received malformed arguments", nullptr);
        return std::nullopt;
    }
    const auto message_id = integer64(args["message_id"]);
    ReactInput input{.chat = args["chat"].get<std::string>(),
                     .message_id = message_id.value_or(0),
                     .reaction = args["reaction"].get<std::string>(),
                     .remove = args["remove"].get<bool>(),
                     .big = args["big"].get<bool>()};
    if (!message_id || !nonzero_int53(*message_id)) {
        failure = usage("msg react message id must be a nonzero int53 value", "id");
        return std::nullopt;
    }
    if (!valid_message_reaction(input.reaction)) {
        failure = usage("msg react emoji must be valid UTF-8 between 1 and 64 bytes", "emoji");
        return std::nullopt;
    }
    if (input.remove && input.big) {
        failure = usage("--remove and --big are mutually exclusive", "--remove/--big",
                        "mutually_exclusive");
        return std::nullopt;
    }
    return input;
}

std::optional<MessagePinInput> parse_message_pin_input(const json& args, bool pinned,
                                                       json& failure) {
    if (!exact_fields(args, {"chat", "message_id"}) || !args["chat"].is_string()) {
        failure = usage(pinned ? "msg pin received malformed arguments"
                               : "msg unpin received malformed arguments",
                        nullptr);
        return std::nullopt;
    }
    const auto message_id = integer64(args["message_id"]);
    if (!message_id || !nonzero_int53(*message_id)) {
        failure = usage("message id must be a nonzero int53 value", "id");
        return std::nullopt;
    }
    return MessagePinInput{
        .chat = args["chat"].get<std::string>(), .message_id = *message_id, .pinned = pinned};
}

std::optional<ChatTargetInput> parse_chat_target_input(const json& args, std::string_view command,
                                                       json& failure) {
    if (!exact_fields(args, {"chat"}) || !args["chat"].is_string()) {
        failure = usage(std::string(command) + " received malformed arguments", nullptr);
        return std::nullopt;
    }
    return ChatTargetInput{.chat = args["chat"].get<std::string>()};
}

std::optional<ChatMuteInput> parse_chat_mute_input(const json& args, bool muted, json& failure) {
    if (!exact_fields(args, {"chat", "duration_seconds"}) || !args["chat"].is_string()) {
        failure = usage(muted ? "chat mute received malformed arguments"
                              : "chat unmute received malformed arguments",
                        nullptr);
        return std::nullopt;
    }
    const auto duration = integer64(args["duration_seconds"]);
    const bool valid_muted = duration && ((*duration >= 1 && *duration <= 31'622'400) ||
                                          *duration == std::numeric_limits<std::int32_t>::max());
    if ((!muted && duration != std::optional<std::int64_t>{0}) || (muted && !valid_muted)) {
        failure = usage("chat mute duration is invalid", "--for");
        return std::nullopt;
    }
    return ChatMuteInput{.chat = args["chat"].get<std::string>(),
                         .duration_seconds = static_cast<std::int32_t>(*duration),
                         .muted = muted};
}

std::optional<ChatJoinInput> parse_chat_join_input(const json& args, json& failure) {
    if (!exact_fields(args, {"target"}) || !args["target"].is_string()) {
        failure = usage("chat join received malformed arguments", nullptr);
        return std::nullopt;
    }
    ChatJoinInput input{
        .target = args["target"].get<std::string>(), .invite = false, .invite_hash = {}};
    const auto canonical = canonical_write_selector(input.target);
    if (!canonical) {
        failure = usage("chat join target is invalid", "invite-link|@username");
        return std::nullopt;
    }
    if (input.target.starts_with('@')) {
        if (*canonical != input.target) {
            failure = usage("chat join username is invalid", "@username");
            return std::nullopt;
        }
        return input;
    }
    if (!canonical->starts_with("sha256:")) {
        failure = usage("chat join requires an invite link or @username", "invite-link|@username");
        return std::nullopt;
    }
    input.invite = true;
    input.invite_hash = *canonical;
    return input;
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

struct MessagePlanningFacts {
    core::TdPlanningMessage message;
    core::TdMessageProperties properties;
};

using MessagePlanningOutcome = std::variant<MessagePlanningFacts, json>;

using MessagePropertiesOutcome = std::variant<core::TdMessageProperties, json>;

MessagePropertiesOutcome read_message_properties(ResolverConsumer& resolver, core::TdClient& client,
                                                 RequestSession& session,
                                                 proto::M3Operation operation,
                                                 const proto::Request& request,
                                                 std::int64_t chat_id, std::int64_t message_id) {
    auto properties_read =
        read_value(resolver, client, session, operation, request, [&](const auto& current) {
            return client.get_message_properties(current, chat_id, message_id);
        });
    if (auto* failure = std::get_if<json>(&properties_read)) {
        return std::move(*failure);
    }
    auto& properties_value = std::get<core::TdValue>(properties_read);
    if (const auto* error = properties_value.get_if<core::TdError>()) {
        if (error->code == 404) {
            return terminal("NOT_FOUND", "message was not found",
                            {{"chat_id", chat_id}, {"message_id", message_id}}, kNotFound);
        }
        return td_error_terminal(operation, *error);
    }
    const auto* properties = properties_value.get_if<core::TdMessageProperties>();
    return properties == nullptr ? MessagePropertiesOutcome{internal(operation)}
                                 : MessagePropertiesOutcome{*properties};
}

MessagePlanningOutcome read_message_planning_facts(ResolverConsumer& resolver,
                                                   core::TdClient& client, RequestSession& session,
                                                   proto::M3Operation operation,
                                                   const proto::Request& request,
                                                   std::int64_t chat_id, std::int64_t message_id) {
    auto message_read =
        read_value(resolver, client, session, operation, request, [&](const auto& current) {
            return client.get_message(current, chat_id, message_id);
        });
    if (auto* failure = std::get_if<json>(&message_read)) {
        return std::move(*failure);
    }
    auto& message_value = std::get<core::TdValue>(message_read);
    if (const auto* error = message_value.get_if<core::TdError>()) {
        if (error->code == 404) {
            return terminal("NOT_FOUND", "message was not found",
                            {{"chat_id", chat_id}, {"message_id", message_id}}, kNotFound);
        }
        return td_error_terminal(operation, *error);
    }
    const auto* message = message_value.get_if<core::TdPlanningMessage>();
    if (message == nullptr || message->chat_id != chat_id || message->id != message_id) {
        return internal(operation);
    }
    const core::TdMessageSummary summary{.id = message->id,
                                         .chat_id = message->chat_id,
                                         .date = message->date,
                                         .sender = message->sender,
                                         .is_outgoing = message->is_outgoing,
                                         .topic = message->topic,
                                         .content_kind = message->content_kind,
                                         .text = message->text};
    const auto materialized = materialize_message_summary(summary);
    if (!materialized || !persistable_message_summary(*materialized)) {
        return internal(operation, "TDLib returned data outside the supported persistence bounds");
    }

    auto properties_read =
        read_message_properties(resolver, client, session, operation, request, chat_id, message_id);
    if (auto* failure = std::get_if<json>(&properties_read)) {
        return std::move(*failure);
    }
    return MessagePlanningFacts{
        .message = *message,
        .properties = std::get<core::TdMessageProperties>(std::move(properties_read))};
}

bool valid_available_reaction(const core::TdAvailableReaction& reaction) {
    switch (reaction.type.kind) {
    case core::TdReactionKind::Emoji:
        return valid_message_reaction(reaction.type.emoji) && reaction.type.custom_emoji_id == 0;
    case core::TdReactionKind::CustomEmoji:
        return reaction.type.emoji.empty() && reaction.type.custom_emoji_id > 0;
    case core::TdReactionKind::Paid:
        return reaction.type.emoji.empty() && reaction.type.custom_emoji_id == 0;
    case core::TdReactionKind::Unknown:
        return false;
    }
    return false;
}

std::optional<bool> reaction_is_available(const core::TdMessageAvailableReactions& available,
                                          std::string_view wanted) {
    if (available.unavailability_reason == core::TdReactionUnavailabilityReason::Unknown ||
        available.unsupported_unavailability_tdlib_type_id) {
        return std::nullopt;
    }
    bool matched = false;
    for (const auto* collection : {&available.top, &available.recent, &available.popular}) {
        for (const auto& reaction : *collection) {
            if (!valid_available_reaction(reaction)) {
                return std::nullopt;
            }
            matched = matched || (reaction.type.kind == core::TdReactionKind::Emoji &&
                                  reaction.type.emoji == wanted);
        }
    }
    return matched;
}

std::optional<core::TdDirectChatList> chat_pin_list(const core::TdChat& chat, bool& malformed) {
    malformed = false;
    bool main = false;
    bool archive = false;
    for (const auto& list : chat.chat_lists) {
        switch (list.kind) {
        case core::TdChatListKind::Main:
            malformed = malformed || list.folder_id != 0;
            main = true;
            break;
        case core::TdChatListKind::Archive:
            malformed = malformed || list.folder_id != 0;
            archive = true;
            break;
        case core::TdChatListKind::Folder:
            malformed = malformed || list.folder_id <= 0;
            break;
        case core::TdChatListKind::Unknown:
            malformed = true;
            break;
        }
    }
    if (malformed) {
        return std::nullopt;
    }
    if (archive) {
        return core::TdDirectChatList::Archive;
    }
    return main ? std::optional{core::TdDirectChatList::Main} : std::nullopt;
}

std::string_view direct_chat_list_name(core::TdDirectChatList list) {
    return list == core::TdDirectChatList::Archive ? std::string_view{"archive"}
                                                   : std::string_view{"main"};
}

using JoinChatIdentityOutcome = std::variant<ChatIdentity, json>;

JoinChatIdentityOutcome materialize_join_chat_identity(ResolverConsumer& resolver,
                                                       core::TdClient& client,
                                                       RequestSession& session,
                                                       const proto::Request& request,
                                                       const core::TdChat& chat) {
    std::optional<json> stopped;
    auto materialized = materialize_chat_identity(
        client, chat, [&](const auto& start) -> std::optional<ReadyReadResult> {
            auto result = resolver.read_target(start);
            if (result.status == ReadyReadStatus::Response) {
                return result;
            }
            switch (result.status) {
            case ReadyReadStatus::AuthorizationLost:
                stopped = not_authed_terminal(request.account, result.snapshot
                                                                   ? result.snapshot->data.state
                                                                   : core::AuthState::Unknown);
                break;
            case ReadyReadStatus::TimedOut:
                stopped = timeout(proto::M3Operation::ChatJoin, "preflight",
                                  pre_intent_idempotency(request));
                break;
            case ReadyReadStatus::Cancelled:
                stopped = json(nullptr);
                break;
            case ReadyReadStatus::Failed:
                stopped = internal(proto::M3Operation::ChatJoin);
                break;
            case ReadyReadStatus::Response:
                break;
            }
            static_cast<void>(session);
            return std::nullopt;
        });
    if (stopped) {
        return std::move(*stopped);
    }
    if (materialized.status == ChatIdentityStatus::TdError && materialized.error) {
        const ResolverError error =
            materialized.error->code == 429
                ? ResolverError{ResolverRateLimitedError{
                      .operation = M2Operation::Resolve,
                      .retry_after = parse_retry_after_seconds(materialized.error->message)}}
                : ResolverError{ResolverTdlibError{.operation = M2Operation::Resolve,
                                                   .tdlib_code = materialized.error->code}};
        return resolver_error_terminal(error, M2Operation::Resolve);
    }
    if (materialized.status == ChatIdentityStatus::Secret) {
        return resolver_error_terminal(
            ResolverError{ResolverUsageError{.argument = "invite-link|@username",
                                             .reason = ResolverUsageReason::UnsupportedChatType}},
            M2Operation::Resolve);
    }
    if (materialized.status != ChatIdentityStatus::Success || !materialized.identity ||
        !persistable_chat_identity(*materialized.identity)) {
        return resolver_error_terminal(
            ResolverError{ResolverInternalError{.operation = M2Operation::Resolve}},
            M2Operation::Resolve);
    }
    return std::move(*materialized.identity);
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

WriteConfirmationOutcome confirm_leave(const write_contract::Plan& plan, RequestSession& session) {
    const auto required = [&] {
        return terminal(
            "CONFIRMATION_REQUIRED", "chat leave was not confirmed",
            {{"account", plan.account()}, {"action", "chat_leave"}, {"target", plan.value()}},
            kDenied);
    };
    if (session.request().context.yes) {
        return {.status = WriteConfirmationStatus::ConfirmedYes, .terminal = std::nullopt};
    }
    if (!session.request().context.tty) {
        return {.status = WriteConfirmationStatus::Rejected, .terminal = required()};
    }
    const auto& chat = plan.value().at("chat");
    auto answer = session.challenge({proto::ChallengeKind::DestructiveConfirmation,
                                     std::nullopt,
                                     std::nullopt,
                                     "Leave \"" + chat.at("title").get<std::string>() + "\" (" +
                                         chat.at("id").dump() + ")? [y/N] ",
                                     {{"action", "chat_leave"}, {"target", plan.value()}},
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed direct-RPC outcome matrix.
void execute_direct_write(
    core::TdClient& client, std::string_view account, config::Store& config_store,
    const std::shared_ptr<IdempotencyFoundation>& foundation,
    const std::function<void()>& audit_fatal_shutdown,
    const std::shared_ptr<const testing::WriteCoordinatorHooks>& coordinator_hooks,
    const proto::Request& request, RequestSession& session, AuthoritySource authority,
    DirectWriteDefinition definition) {
    if (!request.context.dry_run &&
        session.begin_audited_terminal() != AuditedTerminalStatus::Designated) {
        return;
    }
    auto hash = key_hash(request);
    const auto* identity = proto::m3_operation_identity(definition.operation);
    if (request.context.idempotency_key && !hash) {
        session.error("INTERNAL", "cannot hash idempotency key",
                      {{"operation", identity->canonical_name}, {"reason", "internal_error"}},
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
    if (!definition.dispatch || !definition.admit || !definition.plan || !definition.success) {
        session.error("INTERNAL", "write operation is incomplete",
                      {{"operation", identity->canonical_name}, {"reason", "internal_error"}},
                      kGeneric);
        return;
    }

    const WriteKernel kernel(foundation);
    auto kernel_input = kernel_request(request, session, definition.operation, authority,
                                       std::move(hash), std::move(invocation), config_store.path());
    WriteKernelHooks hooks;
    hooks.admit = std::move(definition.admit);
    hooks.plan = [plan = std::move(definition.plan)](const WriteAdmission&) { return plan(); };
    hooks.confirm = std::move(definition.confirm);
    hooks.verify_config_grant = [&config_store](std::string_view expected,
                                                std::string_view expected_account,
                                                const config::MutationControl& control) {
        return config_store.verify_write_grant(expected, expected_account, control);
    };
    hooks.post_intent = std::move(definition.post_intent);
    const auto operation = definition.operation;
    const auto dispatch = std::move(definition.dispatch);
    hooks.revalidate_auth_and_schedule =
        [&client, &session, &request, dispatch, operation, account = std::string(account),
         coordinator_hooks](const write_contract::Plan& plan) -> WriteDispatchAdmissionOutcome {
        if (deadline_expired(session.deadline())) {
            return stored_from_terminal(
                operation, timeout(operation, "preflight",
                                   request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (session.cancellation_requested()) {
            return WriteDispatchStopped{};
        }
        auto current = client.auth_state();
        if (!current || current->data.state != core::AuthState::Ready) {
            return stored_from_terminal(
                operation, not_authed_terminal(account, current ? current->data.state
                                                                : core::AuthState::Unknown));
        }
        dispatch->authorization = std::move(current);
        if (!dispatch->request || plan.value()["tdlib_request"].is_null() ||
            !plan.value()["tdlib_request"].is_string()) {
            return stored_from_terminal(operation, internal(operation));
        }
        auto direct_hooks = coordinator_hooks ? coordinator_hooks->direct_rpc : DirectRpcHooks{};
        dispatch->coordinator =
            std::make_unique<DirectRpcCoordinator>(client, session, std::move(direct_hooks));
        auto preparation =
            dispatch->coordinator->prepare(std::move(*dispatch->request), dispatch->authorization);
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
            return stored_from_terminal(
                operation, not_authed_terminal(account, lost->snapshot ? lost->snapshot->data.state
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
    hooks.dispatch = [dispatch, success = std::move(definition.success), operation, &request,
                      account = std::string(account)](
                         const write_contract::Plan& plan, const WriteDispatchPreparation&,
                         WriteDurableObservationSink&) -> WriteDispatchOutcome {
        if (!dispatch->authorization || !dispatch->coordinator) {
            throw std::logic_error("direct dispatch state is incomplete");
        }
        auto selected = dispatch->coordinator->execute_prepared();
        return std::visit(
            [&](auto&& outcome) -> WriteDispatchOutcome {
                using Outcome = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<Outcome, DirectSuccess>) {
                    auto terminal_value = success(plan, outcome.result);
                    if (!terminal_value) {
                        return {.terminal = stored_from_terminal(operation, internal(operation)),
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
                    const auto state_value =
                        outcome.snapshot ? outcome.snapshot->data.state : core::AuthState::Unknown;
                    return {.terminal = stored_from_terminal(
                                operation, not_authed_terminal(account, state_value)),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectTimedOut>) {
                    return {.terminal = stored_from_terminal(
                                operation, timeout(operation, "dispatch",
                                                   post_intent_idempotency(request), "unknown")),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectCancelled>) {
                    return {.terminal = stored_from_terminal(operation, shutdown_terminal()),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectJoinGuardRequired>) {
                    return {.terminal = stored_error(operation, "JOIN_APPROVAL_REQUIRED",
                                                     "join request requires approval",
                                                     {{"operation", "chat_join"},
                                                      {"bot_user_id", outcome.bot_user_id},
                                                      {"query_id", outcome.query_id}},
                                                     kGeneric),
                            .mutation_state = AccountAuditMutationState::None,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectJoinDeclined>) {
                    return {.terminal = stored_error(operation, "JOIN_DECLINED",
                                                     "join request was declined",
                                                     {{"operation", "chat_join"}}, kGeneric),
                            .mutation_state = AccountAuditMutationState::None,
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectOversizedMessage>) {
                    return {
                        .terminal = stored_from_terminal(
                            operation,
                            internal(
                                operation,
                                "TDLib returned data outside the supported persistence bounds")),
                        .mutation_state = AccountAuditMutationState::Confirmed,
                        .mutation_confirmed = true};
                } else {
                    return {.terminal = stored_from_terminal(operation, internal(operation)),
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
    const auto result = kernel.run(kernel_input, hooks);
    if (result.status == WriteKernelStatus::DryRunPlanned && result.plan) {
        session.result({{"dry_run", true}, {"plan", result.plan->value()}});
    } else if (result.terminal) {
        emit_terminal(session, *result.terminal);
        if (result.status == WriteKernelStatus::DurabilityFatal && audit_fatal_shutdown) {
            audit_fatal_shutdown();
        }
    }
}

} // namespace

WriteCoordinator::WriteCoordinator(core::TdClient& client, std::string account,
                                   std::string config_path, uid_t expected_uid,
                                   std::shared_ptr<IdempotencyFoundation> foundation,
                                   std::function<void()> audit_fatal_shutdown,
                                   std::shared_ptr<const testing::WriteCoordinatorHooks> hooks)
    : client_(client), account_(std::move(account)),
      config_store_(std::move(config_path), expected_uid), foundation_(std::move(foundation)),
      audit_fatal_shutdown_(std::move(audit_fatal_shutdown)), hooks_(std::move(hooks)) {}

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
                                                       .dispatch_authorization = nullptr,
                                                       .coordinator = nullptr,
                                                       .observations = nullptr});
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
            if (formatted == nullptr || !core::valid_td_formatted_text_facts(*formatted) ||
                !valid_send_text(formatted->text)) {
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
            const auto status =
                schedule_window_status(state->input.schedule->send_date, server->value);
            if (status == ScheduleWindowStatus::Elapsed) {
                return precondition(proto::M3Operation::Send, state->target->chat.id, std::nullopt,
                                    "schedule_window_elapsed");
            }
            if (status == ScheduleWindowStatus::TooFar) {
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
                if (failure->value("code", std::string{}) == "TIMEOUT") {
                    return stored_from_terminal(
                        proto::M3Operation::Send,
                        timeout(proto::M3Operation::Send, "preflight",
                                request.context.idempotency_key ? "removed" : "not_requested"));
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
                return stored_from_terminal(proto::M3Operation::Send,
                                            internal(proto::M3Operation::Send));
            }
            const auto status =
                schedule_window_status(state->input.schedule->send_date, server->value);
            if (status != ScheduleWindowStatus::Allowed) {
                const char* const reason = status == ScheduleWindowStatus::Elapsed
                                               ? "schedule_window_elapsed"
                                               : "schedule_too_far";
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
        if (!state->target || !state->formatted_text) {
            throw std::logic_error("send preparation state is incomplete");
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
        SingleSendHooks send_hooks = hooks_ ? hooks_->single_send : SingleSendHooks{};
        auto* state_pointer = state.get();
        send_hooks.on_temporary_id = [state_pointer](const SingleSendTemporaryId& temporary) {
            if (state_pointer->observations == nullptr ||
                !state_pointer->observations->temporary_message_ids(
                    json::array({temporary.temporary_message_id}))) {
                throw std::runtime_error("temporary id was not durable");
            }
        };
        state->coordinator =
            std::make_unique<SingleSendCoordinator>(client_.get(), session, std::move(send_hooks));
        auto preparation =
            state->coordinator->prepare(std::move(td_request), state->dispatch_authorization);
        if (std::holds_alternative<SingleSendTimedOut>(preparation)) {
            return stored_from_terminal(
                proto::M3Operation::Send,
                timeout(proto::M3Operation::Send, "preflight",
                        request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (std::holds_alternative<SingleSendCancelled>(preparation)) {
            return WriteDispatchStopped{};
        }
        if (const auto* lost = std::get_if<SingleSendAuthorizationLost>(&preparation)) {
            return stored_from_terminal(proto::M3Operation::Send,
                                        not_authed_terminal(account_, lost->state));
        }
        if (std::holds_alternative<SingleSendGenerationClosed>(preparation)) {
            return stored_from_terminal(proto::M3Operation::Send,
                                        not_authed_terminal(account_, core::AuthState::Closed));
        }
        if (std::holds_alternative<SingleSendRejected>(preparation)) {
            return stored_from_terminal(proto::M3Operation::Send,
                                        internal(proto::M3Operation::Send));
        }
        const auto token = random_hex32();
        if (token.empty()) {
            throw std::runtime_error("cannot create dispatch token");
        }
        return WriteDispatchPreparation{
            .proof = {{"tdlib_function", "sendMessage"},
                      {"dispatch_token", token},
                      {"client_generation", state->dispatch_authorization->client_generation}}};
    };
    hooks.dispatch = [state, &request,
                      this](const write_contract::Plan&, const WriteDispatchPreparation&,
                            WriteDurableObservationSink& observations) -> WriteDispatchOutcome {
        if (!state->target || !state->dispatch_authorization || !state->coordinator) {
            throw std::logic_error("send dispatch state is incomplete");
        }
        state->observations = &observations;
        auto selected = state->coordinator->execute_prepared();
        state->observations = nullptr;
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
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendAuthorizationLost>) {
                    auto value = not_authed_terminal(account_, outcome.state);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendGenerationClosed>) {
                    auto value = not_authed_terminal(account_, core::AuthState::Closed);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendCancelled>) {
                    auto value = shutdown_terminal();
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, SingleSendRejected>) {
                    auto value = internal(proto::M3Operation::Send);
                    return {.terminal = stored_from_terminal(proto::M3Operation::Send, value),
                            .mutation_state = audit_state(outcome.mutation_state),
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact edit planning matrix.
void WriteCoordinator::edit_message(const proto::Request& request, RequestSession& session) {
    json parse_failure;
    auto input = parse_edit_input(request.args, parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(proto::M3Operation::MsgEdit);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session,
                      resolver_terminal_for_write(*error, proto::M3Operation::MsgEdit, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto authority = authorize(request, session, account_, proto::M3Operation::MsgEdit);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<EditState>(
        EditState{.input = std::move(*input),
                  .principal = std::get<ResolverPrincipal>(principal_outcome),
                  .target = std::nullopt,
                  .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = proto::M3Operation::MsgEdit;
    definition.dispatch = state->dispatch;
    definition.admit = [state, this]() -> WriteAdmissionOutcome {
        const auto fingerprint_value =
            fingerprint(account_, state->principal,
                        MsgEditFingerprintPayload{state->input.chat, state->input.message_id,
                                                  state->input.text});
        std::string error;
        auto arguments = write_contract::make_arguments(proto::M3Operation::MsgEdit,
                                                        {{"chat", state->input.chat},
                                                         {"message_id", state->input.message_id},
                                                         {"text", state->input.text}},
                                                        error);
        if (!fingerprint_value || !arguments) {
            return internal(proto::M3Operation::MsgEdit);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, &resolver, &session, &request, this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, proto::M3Operation::MsgEdit, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        auto facts = read_message_planning_facts(resolver, client_.get(), session,
                                                 proto::M3Operation::MsgEdit, request,
                                                 state->target->chat.id, state->input.message_id);
        if (auto* failure = std::get_if<json>(&facts)) {
            return std::move(*failure);
        }
        const auto& message = std::get<MessagePlanningFacts>(facts);
        if (message.message.content_kind != core::TdMessageContentKind::Text) {
            return precondition(proto::M3Operation::MsgEdit, state->target->chat.id,
                                state->input.message_id, "wrong_content_type");
        }
        if (!message.properties.can_be_edited) {
            return precondition(proto::M3Operation::MsgEdit, state->target->chat.id,
                                state->input.message_id, "not_editable");
        }
        if (message.message.has_reply_markup) {
            return precondition(proto::M3Operation::MsgEdit, state->target->chat.id,
                                state->input.message_id, "reply_markup_preservation_unsupported");
        }
        state->dispatch->request =
            core::TdEditMessageTextRequest{.chat_id = state->target->chat.id,
                                           .message_id = state->input.message_id,
                                           .text = state->input.text};
        std::string error;
        auto plan = write_contract::make_plan(proto::M3Operation::MsgEdit, account_,
                                              {{"operation", "msg_edit"},
                                               {"account", account_},
                                               {"tdlib_request", "editMessageText"},
                                               {"chat", chat_identity_json(state->target->chat)},
                                               {"message_id", state->input.message_id},
                                               {"text", state->input.text}},
                                              error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(proto::M3Operation::MsgEdit)};
    };
    definition.success =
        [](const write_contract::Plan& plan,
           const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* message = std::get_if<core::TdMessageWriteResult>(&outcome);
        if (message == nullptr || message->chat_id != plan.value()["chat"]["id"] ||
            message->id != plan.value()["message_id"] ||
            message->content_kind != core::TdMessageContentKind::Text ||
            message->text != plan.value()["text"].get<std::string>()) {
            return std::nullopt;
        }
        auto result = message_write_result_json(*message);
        if (result.is_null()) {
            return std::nullopt;
        }
        return stored_result(proto::M3Operation::MsgEdit, std::move(result));
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact reaction planning matrix.
void WriteCoordinator::react_to_message(const proto::Request& request, RequestSession& session) {
    json parse_failure;
    auto input = parse_react_input(request.args, parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(proto::M3Operation::MsgReact);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session,
                      resolver_terminal_for_write(*error, proto::M3Operation::MsgReact, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (evaluate_m3_bot_admission(proto::M3Operation::MsgReact, principal.is_bot,
                                  M3ScheduleKind::None) != M3BotAdmission::Allowed) {
        session.error("BOT_UNSUPPORTED", "message reactions require a user account",
                      {{"operation", "msg_react"}}, kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, proto::M3Operation::MsgReact);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<ReactState>(
        ReactState{.input = std::move(*input),
                   .principal = principal,
                   .target = std::nullopt,
                   .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = proto::M3Operation::MsgReact;
    definition.dispatch = state->dispatch;
    definition.admit = [state, this]() -> WriteAdmissionOutcome {
        const auto fingerprint_value =
            fingerprint(account_, state->principal,
                        MsgReactFingerprintPayload{state->input.chat, state->input.message_id,
                                                   state->input.reaction, state->input.remove,
                                                   state->input.big});
        std::string error;
        auto arguments = write_contract::make_arguments(proto::M3Operation::MsgReact,
                                                        {{"chat", state->input.chat},
                                                         {"message_id", state->input.message_id},
                                                         {"reaction", state->input.reaction},
                                                         {"remove", state->input.remove},
                                                         {"big", state->input.big}},
                                                        error);
        if (!fingerprint_value || !arguments) {
            return internal(proto::M3Operation::MsgReact);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, &resolver, &session, &request, this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, proto::M3Operation::MsgReact, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        auto available_read =
            read_value(resolver, client_.get(), session, proto::M3Operation::MsgReact, request,
                       [&](const auto& current) {
                           return client_.get().get_message_available_reactions(
                               current, state->target->chat.id, state->input.message_id);
                       });
        if (auto* failure = std::get_if<json>(&available_read)) {
            return std::move(*failure);
        }
        auto& available_value = std::get<core::TdValue>(available_read);
        if (const auto* error = available_value.get_if<core::TdError>()) {
            if (error->code == 404) {
                return terminal(
                    "NOT_FOUND", "message was not found",
                    {{"chat_id", state->target->chat.id}, {"message_id", state->input.message_id}},
                    kNotFound);
            }
            return td_error_terminal(proto::M3Operation::MsgReact, *error);
        }
        const auto* available = available_value.get_if<core::TdMessageAvailableReactions>();
        if (available == nullptr) {
            return internal(proto::M3Operation::MsgReact);
        }
        const auto availability = reaction_is_available(*available, state->input.reaction);
        if (!availability) {
            return internal(proto::M3Operation::MsgReact);
        }
        auto properties =
            read_message_properties(resolver, client_.get(), session, proto::M3Operation::MsgReact,
                                    request, state->target->chat.id, state->input.message_id);
        if (auto* failure = std::get_if<json>(&properties)) {
            return std::move(*failure);
        }
        if (!*availability) {
            return precondition(proto::M3Operation::MsgReact, state->target->chat.id,
                                state->input.message_id, "reaction_unavailable");
        }
        state->dispatch->request =
            core::TdMessageReactionRequest{.chat_id = state->target->chat.id,
                                           .message_id = state->input.message_id,
                                           .reaction = state->input.reaction,
                                           .remove = state->input.remove,
                                           .big = state->input.big};
        std::string error;
        auto plan = write_contract::make_plan(
            proto::M3Operation::MsgReact, account_,
            {{"operation", "msg_react"},
             {"account", account_},
             {"tdlib_request",
              state->input.remove ? "removeMessageReaction" : "addMessageReaction"},
             {"chat", chat_identity_json(state->target->chat)},
             {"message_id", state->input.message_id},
             {"reaction", state->input.reaction},
             {"remove", state->input.remove},
             {"big", state->input.big}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(proto::M3Operation::MsgReact)};
    };
    definition.success =
        [](const write_contract::Plan& plan,
           const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectReactionResult>(&outcome);
        if (result == nullptr || result->chat_id != plan.value()["chat"]["id"] ||
            result->message_id != plan.value()["message_id"] ||
            result->reaction != plan.value()["reaction"].get<std::string>() ||
            result->removed != plan.value()["remove"] || result->big != plan.value()["big"]) {
            return std::nullopt;
        }
        return stored_result(proto::M3Operation::MsgReact, {{"chat_id", result->chat_id},
                                                            {"message_id", result->message_id},
                                                            {"reaction", result->reaction},
                                                            {"removed", result->removed},
                                                            {"big", result->big}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact message-pin planning matrix.
void WriteCoordinator::pin_message(const proto::Request& request, RequestSession& session,
                                   bool pinned) {
    const auto operation = pinned ? proto::M3Operation::MsgPin : proto::M3Operation::MsgUnpin;
    json parse_failure;
    auto input = parse_message_pin_input(request.args, pinned, parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session, resolver_terminal_for_write(*error, operation, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<MessagePinState>(
        MessagePinState{.input = std::move(*input),
                        .principal = std::get<ResolverPrincipal>(principal_outcome),
                        .target = std::nullopt,
                        .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = operation;
    definition.dispatch = state->dispatch;
    definition.admit = [state, operation, this]() -> WriteAdmissionOutcome {
        const auto payload = state->input.pinned ? FingerprintPayload{MsgPinFingerprintPayload{
                                                       state->input.chat, state->input.message_id}}
                                                 : FingerprintPayload{MsgUnpinFingerprintPayload{
                                                       state->input.chat, state->input.message_id}};
        const auto fingerprint_value = fingerprint(account_, state->principal, payload);
        std::string error;
        auto arguments = write_contract::make_arguments(
            operation, {{"chat", state->input.chat}, {"message_id", state->input.message_id}},
            error);
        if (!fingerprint_value || !arguments) {
            return internal(operation);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, operation, &resolver, &session, &request,
                       this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, operation, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        auto properties =
            read_message_properties(resolver, client_.get(), session, operation, request,
                                    state->target->chat.id, state->input.message_id);
        if (auto* failure = std::get_if<json>(&properties)) {
            return std::move(*failure);
        }
        if (state->input.pinned && !std::get<core::TdMessageProperties>(properties).can_be_pinned) {
            return precondition(operation, state->target->chat.id, state->input.message_id,
                                "not_pinnable");
        }
        state->dispatch->request = core::TdPinMessageRequest{.chat_id = state->target->chat.id,
                                                             .message_id = state->input.message_id,
                                                             .pinned = state->input.pinned};
        std::string error;
        auto plan = write_contract::make_plan(
            operation, account_,
            {{"operation", state->input.pinned ? "msg_pin" : "msg_unpin"},
             {"account", account_},
             {"tdlib_request", state->input.pinned ? "pinChatMessage" : "unpinChatMessage"},
             {"chat", chat_identity_json(state->target->chat)},
             {"message_id", state->input.message_id},
             {"pinned", state->input.pinned}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(operation)};
    };
    definition.success =
        [operation](const write_contract::Plan& plan,
                    const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectMessagePinResult>(&outcome);
        if (result == nullptr || result->chat_id != plan.value()["chat"]["id"] ||
            result->message_id != plan.value()["message_id"] ||
            result->pinned != plan.value()["pinned"]) {
            return std::nullopt;
        }
        return stored_result(operation, {{"chat_id", result->chat_id},
                                         {"message_id", result->message_id},
                                         {"pinned", result->pinned}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact mark-read/no-op matrix.
void WriteCoordinator::mark_chat_read(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::M3Operation::ChatMarkRead;
    json parse_failure;
    auto input = parse_chat_target_input(request.args, "chat mark-read", parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session, resolver_terminal_for_write(*error, operation, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (evaluate_m3_bot_admission(operation, principal.is_bot, M3ScheduleKind::None) !=
        M3BotAdmission::Allowed) {
        session.error("BOT_UNSUPPORTED", "chat mark-read requires a user account",
                      {{"operation", "chat_mark_read"}}, kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<ChatTargetState>(
        ChatTargetState{.input = std::move(*input),
                        .principal = principal,
                        .target = std::nullopt,
                        .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = operation;
    definition.dispatch = state->dispatch;
    definition.admit = [state, this]() -> WriteAdmissionOutcome {
        const auto fingerprint_value =
            fingerprint(account_, state->principal,
                        FingerprintPayload{ChatMarkReadFingerprintPayload{state->input.chat}});
        std::string error;
        auto arguments = write_contract::make_arguments(proto::M3Operation::ChatMarkRead,
                                                        {{"chat", state->input.chat}}, error);
        if (!fingerprint_value || !arguments) {
            return internal(proto::M3Operation::ChatMarkRead);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, &resolver, &request, this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, proto::M3Operation::ChatMarkRead, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        if (!state->target->observed_chat ||
            state->target->observed_chat->id != state->target->chat.id) {
            return internal(proto::M3Operation::ChatMarkRead);
        }
        std::optional<std::int64_t> last_message_id;
        if (state->target->observed_chat->last_message) {
            const auto materialized =
                materialize_message_summary(*state->target->observed_chat->last_message);
            if (!materialized || !persistable_message_summary(*materialized) ||
                materialized->chat_id != state->target->chat.id) {
                return internal(proto::M3Operation::ChatMarkRead);
            }
            last_message_id = materialized->id;
            state->dispatch->request = core::TdViewMessagesRequest{
                .chat_id = state->target->chat.id, .message_ids = {*last_message_id}};
        }
        std::string error;
        auto plan = write_contract::make_plan(
            proto::M3Operation::ChatMarkRead, account_,
            {{"operation", "chat_mark_read"},
             {"account", account_},
             {"tdlib_request", last_message_id ? json("viewMessages") : json(nullptr)},
             {"chat", chat_identity_json(state->target->chat)},
             {"last_message_id", last_message_id ? json(*last_message_id) : json(nullptr)}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(proto::M3Operation::ChatMarkRead)};
    };
    definition.post_intent = [](const write_contract::Plan& plan,
                                const WriteAdmission&) -> WritePostIntentPreparation {
        if (!plan.value()["last_message_id"].is_null()) {
            return {};
        }
        return {.spool = std::nullopt,
                .terminal_without_dispatch = stored_result(proto::M3Operation::ChatMarkRead,
                                                           {{"chat_id", plan.value()["chat"]["id"]},
                                                            {"last_read_message_id", nullptr},
                                                            {"marked_read", true}}),
                .complete_without_mutation = true};
    };
    definition.success =
        [](const write_contract::Plan& plan,
           const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectMarkReadResult>(&outcome);
        if (result == nullptr || result->chat_id != plan.value()["chat"]["id"] ||
            !result->last_read_message_id ||
            *result->last_read_message_id != plan.value()["last_message_id"] ||
            !result->marked_read) {
            return std::nullopt;
        }
        return stored_result(proto::M3Operation::ChatMarkRead,
                             {{"chat_id", result->chat_id},
                              {"last_read_message_id", *result->last_read_message_id},
                              {"marked_read", true}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact notification plan matrix.
void WriteCoordinator::mute_chat(const proto::Request& request, RequestSession& session,
                                 bool muted) {
    const auto operation = muted ? proto::M3Operation::ChatMute : proto::M3Operation::ChatUnmute;
    json parse_failure;
    auto input = parse_chat_mute_input(request.args, muted, parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session, resolver_terminal_for_write(*error, operation, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (evaluate_m3_bot_admission(operation, principal.is_bot, M3ScheduleKind::None) !=
        M3BotAdmission::Allowed) {
        session.error("BOT_UNSUPPORTED", "chat notification settings require a user account",
                      {{"operation", proto::m3_operation_identity(operation)->canonical_name}},
                      kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<ChatMuteState>(
        ChatMuteState{.input = std::move(*input),
                      .principal = principal,
                      .target = std::nullopt,
                      .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = operation;
    definition.dispatch = state->dispatch;
    definition.admit = [state, operation, this]() -> WriteAdmissionOutcome {
        const auto payload = state->input.muted
                                 ? FingerprintPayload{ChatMuteFingerprintPayload{
                                       state->input.chat, state->input.duration_seconds}}
                                 : FingerprintPayload{ChatUnmuteFingerprintPayload{
                                       state->input.chat, state->input.duration_seconds}};
        const auto fingerprint_value = fingerprint(account_, state->principal, payload);
        std::string error;
        auto arguments = write_contract::make_arguments(
            operation,
            {{"chat", state->input.chat}, {"duration_seconds", state->input.duration_seconds}},
            error);
        if (!fingerprint_value || !arguments) {
            return internal(operation);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, operation, &resolver, &request, this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, operation, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        if (!state->target->observed_chat ||
            state->target->observed_chat->id != state->target->chat.id ||
            !state->target->observed_chat->notification_settings) {
            return internal(operation);
        }
        if (state->target->chat.type == "private" &&
            state->target->chat.id == state->principal.id) {
            return precondition(operation, state->target->chat.id, std::nullopt,
                                "saved_notifications_unsupported");
        }
        auto settings = *state->target->observed_chat->notification_settings;
        settings.use_default_mute_for = false;
        settings.mute_for = state->input.duration_seconds;
        state->dispatch->request = core::TdSetChatNotificationSettingsRequest{
            .chat_id = state->target->chat.id, .settings = settings};
        std::string error;
        auto plan = write_contract::make_plan(
            operation, account_,
            {{"operation", proto::m3_operation_identity(operation)->canonical_name},
             {"account", account_},
             {"tdlib_request", "setChatNotificationSettings"},
             {"chat", chat_identity_json(state->target->chat)},
             {"muted", state->input.muted},
             {"duration_seconds", state->input.duration_seconds}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(operation)};
    };
    definition.success =
        [operation](const write_contract::Plan& plan,
                    const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectMuteResult>(&outcome);
        if (result == nullptr || result->chat_id != plan.value()["chat"]["id"] ||
            result->muted != plan.value()["muted"] ||
            result->duration_seconds != plan.value()["duration_seconds"]) {
            return std::nullopt;
        }
        return stored_result(operation, {{"chat_id", result->chat_id},
                                         {"muted", result->muted},
                                         {"duration_seconds", result->duration_seconds}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact chat-list pin matrix.
void WriteCoordinator::pin_chat(const proto::Request& request, RequestSession& session,
                                bool pinned) {
    const auto operation = pinned ? proto::M3Operation::ChatPin : proto::M3Operation::ChatUnpin;
    json parse_failure;
    auto input =
        parse_chat_target_input(request.args, pinned ? "chat pin" : "chat unpin", parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session, resolver_terminal_for_write(*error, operation, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (evaluate_m3_bot_admission(operation, principal.is_bot, M3ScheduleKind::None) !=
        M3BotAdmission::Allowed) {
        session.error("BOT_UNSUPPORTED", "chat pinning requires a user account",
                      {{"operation", proto::m3_operation_identity(operation)->canonical_name}},
                      kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<ChatTargetState>(
        ChatTargetState{.input = std::move(*input),
                        .principal = principal,
                        .target = std::nullopt,
                        .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = operation;
    definition.dispatch = state->dispatch;
    definition.admit = [state, operation, pinned, this]() -> WriteAdmissionOutcome {
        const auto payload =
            pinned ? FingerprintPayload{ChatPinFingerprintPayload{state->input.chat}}
                   : FingerprintPayload{ChatUnpinFingerprintPayload{state->input.chat}};
        const auto fingerprint_value = fingerprint(account_, state->principal, payload);
        std::string error;
        auto arguments =
            write_contract::make_arguments(operation, {{"chat", state->input.chat}}, error);
        if (!fingerprint_value || !arguments) {
            return internal(operation);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, operation, pinned, &resolver, &request,
                       this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, operation, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        if (!state->target->observed_chat ||
            state->target->observed_chat->id != state->target->chat.id) {
            return internal(operation);
        }
        bool malformed = false;
        const auto list = chat_pin_list(*state->target->observed_chat, malformed);
        if (malformed) {
            return internal(operation);
        }
        if (!list) {
            return precondition(operation, state->target->chat.id, std::nullopt, "chat_not_listed");
        }
        state->dispatch->request = core::TdToggleChatIsPinnedRequest{
            .chat_id = state->target->chat.id, .list = *list, .pinned = pinned};
        std::string error;
        auto plan = write_contract::make_plan(
            operation, account_,
            {{"operation", proto::m3_operation_identity(operation)->canonical_name},
             {"account", account_},
             {"tdlib_request", "toggleChatIsPinned"},
             {"chat", chat_identity_json(state->target->chat)},
             {"chat_list", direct_chat_list_name(*list)},
             {"pinned", pinned}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(operation)};
    };
    definition.success =
        [operation](const write_contract::Plan& plan,
                    const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectChatPinResult>(&outcome);
        if (result == nullptr || result->chat_id != plan.value()["chat"]["id"] ||
            direct_chat_list_name(result->chat_list) !=
                plan.value()["chat_list"].get<std::string>() ||
            result->pinned != plan.value()["pinned"]) {
            return std::nullopt;
        }
        return stored_result(operation, {{"chat_id", result->chat_id},
                                         {"chat_list", direct_chat_list_name(result->chat_list)},
                                         {"pinned", result->pinned}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

void WriteCoordinator::archive_chat(const proto::Request& request, RequestSession& session,
                                    bool archived) {
    const auto operation =
        archived ? proto::M3Operation::ChatArchive : proto::M3Operation::ChatUnarchive;
    json parse_failure;
    auto input = parse_chat_target_input(request.args, archived ? "chat archive" : "chat unarchive",
                                         parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session, resolver_terminal_for_write(*error, operation, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (evaluate_m3_bot_admission(operation, principal.is_bot, M3ScheduleKind::None) !=
        M3BotAdmission::Allowed) {
        session.error("BOT_UNSUPPORTED", "chat archiving requires a user account",
                      {{"operation", proto::m3_operation_identity(operation)->canonical_name}},
                      kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<ChatTargetState>(
        ChatTargetState{.input = std::move(*input),
                        .principal = principal,
                        .target = std::nullopt,
                        .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = operation;
    definition.dispatch = state->dispatch;
    definition.admit = [state, operation, archived, this]() -> WriteAdmissionOutcome {
        const auto payload =
            archived ? FingerprintPayload{ChatArchiveFingerprintPayload{state->input.chat}}
                     : FingerprintPayload{ChatUnarchiveFingerprintPayload{state->input.chat}};
        const auto fingerprint_value = fingerprint(account_, state->principal, payload);
        std::string error;
        auto arguments =
            write_contract::make_arguments(operation, {{"chat", state->input.chat}}, error);
        if (!fingerprint_value || !arguments) {
            return internal(operation);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, operation, archived, &resolver, &request,
                       this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, operation, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        const auto list = archived ? core::TdDirectChatList::Archive : core::TdDirectChatList::Main;
        state->dispatch->request =
            core::TdAddChatToListRequest{.chat_id = state->target->chat.id, .list = list};
        std::string error;
        auto plan = write_contract::make_plan(
            operation, account_,
            {{"operation", proto::m3_operation_identity(operation)->canonical_name},
             {"account", account_},
             {"tdlib_request", "addChatToList"},
             {"chat", chat_identity_json(state->target->chat)},
             {"archived", archived}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(operation)};
    };
    definition.success =
        [operation](const write_contract::Plan& plan,
                    const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectArchiveResult>(&outcome);
        if (result == nullptr || result->chat_id != plan.value()["chat"]["id"] ||
            result->archived != plan.value()["archived"]) {
            return std::nullopt;
        }
        return stored_result(operation,
                             {{"chat_id", result->chat_id}, {"archived", result->archived}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): exact username/invite join matrix.
void WriteCoordinator::join_chat(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::M3Operation::ChatJoin;
    json parse_failure;
    auto input = parse_chat_join_input(request.args, parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session, resolver_terminal_for_write(*error, operation, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto principal = std::get<ResolverPrincipal>(principal_outcome);
    if (evaluate_m3_bot_admission(operation, principal.is_bot, M3ScheduleKind::None) !=
        M3BotAdmission::Allowed) {
        session.error("BOT_UNSUPPORTED", "chat join requires a user account",
                      {{"operation", "chat_join"}}, kUsage);
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<ChatJoinState>(
        ChatJoinState{.input = std::move(*input),
                      .principal = principal,
                      .chat = std::nullopt,
                      .dispatch = std::make_shared<DirectDispatchState>()});
    const secure::StringWiper invite_wiper(state->input.target);
    DirectWriteDefinition definition;
    definition.operation = operation;
    definition.dispatch = state->dispatch;
    definition.admit = [state, this]() -> WriteAdmissionOutcome {
        const auto payload = state->input.invite
                                 ? FingerprintPayload{ChatJoinFingerprintPayload{
                                       ChatJoinInviteFingerprint{state->input.invite_hash}}}
                                 : FingerprintPayload{ChatJoinFingerprintPayload{
                                       ChatJoinUsernameFingerprint{state->input.target}}};
        const auto fingerprint_value = fingerprint(account_, state->principal, payload);
        std::string error;
        auto arguments = write_contract::make_arguments(
            proto::M3Operation::ChatJoin,
            state->input.invite
                ? json{{"source", "invite_link"}, {"invite_link_sha256", state->input.invite_hash}}
                : json{{"source", "username"}, {"username", state->input.target}},
            error);
        if (!fingerprint_value || !arguments) {
            return internal(proto::M3Operation::ChatJoin);
        }
        std::vector<redaction::CorrelatedInviteLink> redactions;
        if (state->input.invite) {
            auto lease =
                redaction::InviteLinkRegistry::instance().register_link(state->input.target);
            if (!lease.valid()) {
                return internal(proto::M3Operation::ChatJoin);
            }
            redactions.push_back(std::move(lease));
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = std::move(redactions)};
    };
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): closed invite metadata matrix.
    definition.plan = [state, &resolver, &session, &request, this]() -> WritePlanningOutcome {
        if (!state->input.invite) {
            auto resolved = resolver.resolve_exact_chat(state->input.target);
            if (const auto* error = std::get_if<ResolverError>(&resolved)) {
                return resolver_terminal_for_write(*error, proto::M3Operation::ChatJoin, request);
            }
            if (std::holds_alternative<ResolverStop>(resolved)) {
                return json(nullptr);
            }
            auto target = std::get<ResolvedChatTarget>(std::move(resolved));
            state->chat = target.chat;
            state->dispatch->request =
                core::TdJoinChatRequest{.chat_id = target.chat.id,
                                        .invite_link = std::nullopt,
                                        .expected_invite_chat_id = std::nullopt};
        } else {
            const auto read_error = [state](const core::TdError& error) -> json {
                if (error.code == 404 || error.code == 400) {
                    return resolver_error_terminal(
                        ResolverError{ResolverNotFoundError{.selector = state->input.invite_hash,
                                                            .scope = std::nullopt}},
                        M2Operation::Resolve);
                }
                if (error.code == 429) {
                    return resolver_error_terminal(
                        ResolverError{ResolverRateLimitedError{
                            .operation = M2Operation::Resolve,
                            .retry_after = parse_retry_after_seconds(error.message)}},
                        M2Operation::Resolve);
                }
                return resolver_error_terminal(
                    ResolverError{ResolverTdlibError{.operation = M2Operation::Resolve,
                                                     .tdlib_code = error.code}},
                    M2Operation::Resolve);
            };
            auto classified = read_value(
                resolver, client_.get(), session, proto::M3Operation::ChatJoin, request,
                [&](const auto& current) {
                    return client_.get().get_internal_link_type(current, state->input.target);
                });
            if (auto* failure = std::get_if<json>(&classified)) {
                return std::move(*failure);
            }
            auto& classified_value = std::get<core::TdValue>(classified);
            if (const auto* error = classified_value.get_if<core::TdError>()) {
                return read_error(*error);
            }
            const auto* link = classified_value.get_if<core::TdInternalLink>();
            if (link == nullptr) {
                return resolver_error_terminal(
                    ResolverError{ResolverInternalError{.operation = M2Operation::Resolve}},
                    M2Operation::Resolve);
            }
            if (link->kind != core::TdInternalLinkKind::ChatInvite) {
                return resolver_error_terminal(
                    ResolverError{
                        ResolverUsageError{.argument = "invite-link|@username",
                                           .reason = ResolverUsageReason::UnsupportedLinkType}},
                    M2Operation::Resolve);
            }
            auto checked = read_value(
                resolver, client_.get(), session, proto::M3Operation::ChatJoin, request,
                [&](const auto& current) {
                    return client_.get().check_chat_invite_link(current, state->input.target);
                });
            if (auto* failure = std::get_if<json>(&checked)) {
                return std::move(*failure);
            }
            auto& checked_value = std::get<core::TdValue>(checked);
            if (const auto* error = checked_value.get_if<core::TdError>()) {
                return read_error(*error);
            }
            const auto* info = checked_value.get_if<core::TdChatInviteLinkInfo>();
            if (info == nullptr || (info->chat_id != 0 && !core::valid_td_chat_id(info->chat_id))) {
                return resolver_error_terminal(
                    ResolverError{ResolverInternalError{.operation = M2Operation::Resolve}},
                    M2Operation::Resolve);
            }
            if (info->chat_id != 0) {
                auto chat_read =
                    read_value(resolver, client_.get(), session, proto::M3Operation::ChatJoin,
                               request, [&](const auto& current) {
                                   return client_.get().get_chat(current, info->chat_id);
                               });
                if (auto* failure = std::get_if<json>(&chat_read)) {
                    return std::move(*failure);
                }
                auto& chat_value = std::get<core::TdValue>(chat_read);
                if (const auto* error = chat_value.get_if<core::TdError>()) {
                    return read_error(*error);
                }
                const auto* chat = chat_value.get_if<core::TdChat>();
                if (chat == nullptr || chat->id != info->chat_id) {
                    return resolver_error_terminal(
                        ResolverError{ResolverInternalError{.operation = M2Operation::Resolve}},
                        M2Operation::Resolve);
                }
                auto identity = materialize_join_chat_identity(resolver, client_.get(), session,
                                                               request, *chat);
                if (auto* failure = std::get_if<json>(&identity)) {
                    return std::move(*failure);
                }
                state->chat = std::get<ChatIdentity>(std::move(identity));
            }
            state->dispatch->request = core::TdJoinChatRequest{
                .chat_id = std::nullopt,
                .invite_link = state->input.target,
                .expected_invite_chat_id =
                    state->chat ? std::optional{state->chat->id} : std::nullopt};
        }
        std::string error;
        auto plan = write_contract::make_plan(
            proto::M3Operation::ChatJoin, account_,
            {{"operation", "chat_join"},
             {"account", account_},
             {"tdlib_request", state->input.invite ? "joinChatByInviteLink" : "joinChat"},
             {"source", state->input.invite ? "invite_link" : "username"},
             {"chat", state->chat ? chat_identity_json(*state->chat) : json(nullptr)},
             {"invite_link_sha256",
              state->input.invite ? json(state->input.invite_hash) : json(nullptr)}},
            error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(proto::M3Operation::ChatJoin)};
    };
    definition.success =
        [](const write_contract::Plan& plan,
           const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectJoinResult>(&outcome);
        if (result == nullptr) {
            return std::nullopt;
        }
        const auto planned_chat =
            plan.value()["chat"].is_object()
                ? std::optional<std::int64_t>{plan.value()["chat"]["id"].get<std::int64_t>()}
                : std::nullopt;
        if (result->status == DirectJoinStatus::Joined) {
            if (!result->chat_id || (planned_chat && result->chat_id != planned_chat)) {
                return std::nullopt;
            }
            return stored_result(proto::M3Operation::ChatJoin,
                                 {{"status", "joined"}, {"chat_id", *result->chat_id}});
        }
        if (result->chat_id != planned_chat) {
            return std::nullopt;
        }
        return stored_result(
            proto::M3Operation::ChatJoin,
            {{"status", "request_sent"},
             {"chat_id", result->chat_id ? json(*result->chat_id) : json(nullptr)}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

void WriteCoordinator::leave_chat(const proto::Request& request, RequestSession& session) {
    constexpr auto operation = proto::M3Operation::ChatLeave;
    json parse_failure;
    auto input = parse_chat_target_input(request.args, "chat leave", parse_failure);
    if (!input) {
        emit_terminal(session, parse_failure);
        return;
    }
    ResolverConsumer resolver(client_.get(), account_, session);
    const auto principal_outcome = resolver.bind_principal(operation);
    if (const auto* error = std::get_if<ResolverError>(&principal_outcome)) {
        emit_terminal(session, resolver_terminal_for_write(*error, operation, request));
        return;
    }
    if (std::holds_alternative<ResolverStop>(principal_outcome)) {
        return;
    }
    const auto authority = authorize(request, session, account_, operation);
    if (!authority) {
        return;
    }
    auto state = std::make_shared<ChatTargetState>(
        ChatTargetState{.input = std::move(*input),
                        .principal = std::get<ResolverPrincipal>(principal_outcome),
                        .target = std::nullopt,
                        .dispatch = std::make_shared<DirectDispatchState>()});
    DirectWriteDefinition definition;
    definition.operation = operation;
    definition.dispatch = state->dispatch;
    definition.admit = [state, this]() -> WriteAdmissionOutcome {
        const auto fingerprint_value =
            fingerprint(account_, state->principal,
                        FingerprintPayload{ChatLeaveFingerprintPayload{state->input.chat}});
        std::string error;
        auto arguments = write_contract::make_arguments(proto::M3Operation::ChatLeave,
                                                        {{"chat", state->input.chat}}, error);
        if (!fingerprint_value || !arguments) {
            return internal(proto::M3Operation::ChatLeave);
        }
        return WriteAdmission{.arguments = std::move(*arguments),
                              .request_fingerprint = *fingerprint_value,
                              .pass1_source = nullptr,
                              .invite_redactions = {}};
    };
    definition.plan = [state, &resolver, &request, this]() -> WritePlanningOutcome {
        auto resolved = resolver.resolve_exact_chat(state->input.chat);
        if (const auto* error = std::get_if<ResolverError>(&resolved)) {
            return resolver_terminal_for_write(*error, proto::M3Operation::ChatLeave, request);
        }
        if (std::holds_alternative<ResolverStop>(resolved)) {
            return json(nullptr);
        }
        state->target = std::get<ResolvedChatTarget>(std::move(resolved));
        if (state->target->chat.type != "basic_group" && state->target->chat.type != "supergroup" &&
            state->target->chat.type != "channel") {
            return precondition(proto::M3Operation::ChatLeave, state->target->chat.id, std::nullopt,
                                "wrong_chat_type");
        }
        state->dispatch->request = core::TdLeaveChatRequest{.chat_id = state->target->chat.id};
        std::string error;
        auto plan = write_contract::make_plan(proto::M3Operation::ChatLeave, account_,
                                              {{"operation", "chat_leave"},
                                               {"account", account_},
                                               {"tdlib_request", "leaveChat"},
                                               {"chat", chat_identity_json(state->target->chat)}},
                                              error);
        return plan ? WritePlanningOutcome{std::move(*plan)}
                    : WritePlanningOutcome{internal(proto::M3Operation::ChatLeave)};
    };
    definition.confirm = [&session](const write_contract::Plan& plan, bool) {
        return confirm_leave(plan, session);
    };
    definition.success =
        [](const write_contract::Plan& plan,
           const DirectResult& outcome) -> std::optional<write_contract::StoredTerminal> {
        const auto* result = std::get_if<DirectLeaveResult>(&outcome);
        if (result == nullptr || result->chat_id != plan.value()["chat"]["id"] || !result->left) {
            return std::nullopt;
        }
        return stored_result(proto::M3Operation::ChatLeave,
                             {{"chat_id", result->chat_id}, {"left", true}});
    };
    execute_direct_write(client_.get(), account_, config_store_, foundation_, audit_fatal_shutdown_,
                         hooks_, request, session, *authority, std::move(definition));
}

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
                                                           .dispatch_authorization = nullptr,
                                                           .coordinator = nullptr});
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
        [state, &session, &request,
         this](const write_contract::Plan& plan) -> WriteDispatchAdmissionOutcome {
        if (deadline_expired(session.deadline())) {
            return stored_from_terminal(
                proto::M3Operation::MsgDelete,
                timeout(proto::M3Operation::MsgDelete, "preflight",
                        request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (session.cancellation_requested()) {
            return WriteDispatchStopped{};
        }
        auto current = client_.get().auth_state();
        if (!current || current->data.state != core::AuthState::Ready) {
            return stored_from_terminal(
                proto::M3Operation::MsgDelete,
                not_authed_terminal(account_,
                                    current ? current->data.state : core::AuthState::Unknown));
        }
        state->dispatch_authorization = std::move(current);
        if (deadline_expired(session.deadline())) {
            return stored_from_terminal(
                proto::M3Operation::MsgDelete,
                timeout(proto::M3Operation::MsgDelete, "preflight",
                        request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (session.cancellation_requested()) {
            return WriteDispatchStopped{};
        }
        if (!state->target) {
            throw std::logic_error("delete preparation state is incomplete");
        }
        const core::TdDeleteMessagesRequest td_request{
            .chat_id = state->target->chat.id,
            .message_ids = state->input.message_ids,
            .revoke = plan.value()["effective_for_all"].get<bool>()};
        auto direct_hooks = hooks_ ? hooks_->direct_rpc : DirectRpcHooks{};
        state->coordinator =
            std::make_unique<DirectRpcCoordinator>(client_.get(), session, std::move(direct_hooks));
        auto preparation = state->coordinator->prepare(core::TdDirectRequest{td_request},
                                                       state->dispatch_authorization);
        if (std::holds_alternative<DirectTimedOut>(preparation)) {
            return stored_from_terminal(
                proto::M3Operation::MsgDelete,
                timeout(proto::M3Operation::MsgDelete, "preflight",
                        request.context.idempotency_key ? "removed" : "not_requested"));
        }
        if (std::holds_alternative<DirectCancelled>(preparation)) {
            return WriteDispatchStopped{};
        }
        if (const auto* lost = std::get_if<DirectAuthorizationLost>(&preparation)) {
            return stored_from_terminal(
                proto::M3Operation::MsgDelete,
                not_authed_terminal(account_, lost->snapshot ? lost->snapshot->data.state
                                                             : core::AuthState::Unknown));
        }
        if (std::holds_alternative<DirectRejected>(preparation)) {
            return stored_from_terminal(proto::M3Operation::MsgDelete,
                                        internal(proto::M3Operation::MsgDelete));
        }
        const auto token = random_hex32();
        if (token.empty()) {
            throw std::runtime_error("cannot create dispatch token");
        }
        return WriteDispatchPreparation{
            .proof = {{"tdlib_function", "deleteMessages"},
                      {"dispatch_token", token},
                      {"client_generation", state->dispatch_authorization->client_generation}}};
    };
    hooks.dispatch = [state, &request, this](const write_contract::Plan& plan,
                                             const WriteDispatchPreparation&,
                                             WriteDurableObservationSink&) -> WriteDispatchOutcome {
        if (!state->target || !state->dispatch_authorization || !state->coordinator) {
            throw std::logic_error("delete dispatch state is incomplete");
        }
        auto selected = state->coordinator->execute_prepared();
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
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectAuthorizationLost>) {
                    const auto state_value =
                        outcome.snapshot ? outcome.snapshot->data.state : core::AuthState::Unknown;
                    return {.terminal =
                                stored_from_terminal(proto::M3Operation::MsgDelete,
                                                     not_authed_terminal(account_, state_value)),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectTimedOut>) {
                    return {.terminal = stored_from_terminal(
                                proto::M3Operation::MsgDelete,
                                timeout(proto::M3Operation::MsgDelete, "dispatch",
                                        post_intent_idempotency(request), "unknown")),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectCancelled>) {
                    auto value = shutdown_terminal();
                    return {.terminal = stored_from_terminal(proto::M3Operation::MsgDelete, value),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectRejected>) {
                    auto value = internal(proto::M3Operation::MsgDelete);
                    return {.terminal = stored_from_terminal(proto::M3Operation::MsgDelete, value),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else if constexpr (std::is_same_v<Outcome, DirectMalformed>) {
                    return {.terminal =
                                stored_from_terminal(proto::M3Operation::MsgDelete,
                                                     internal(proto::M3Operation::MsgDelete)),
                            .mutation_state = audit_state(outcome.mutation_state),
                            .mutation_confirmed = false};
                } else {
                    return {.terminal = stored_from_terminal(
                                proto::M3Operation::MsgDelete,
                                not_authed_terminal(account_, core::AuthState::Ready)),
                            .mutation_state = audit_state(outcome.mutation_state),
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
        "msg edit", {Tier::Write,
                     [&coordinator](const proto::Request& request, RequestSession& session) {
                         coordinator.edit_message(request, session);
                     },
                     false, proto::M3Operation::MsgEdit});
    dispatcher.register_command(
        "msg delete", {Tier::Destructive,
                       [&coordinator](const proto::Request& request, RequestSession& session) {
                           coordinator.delete_messages(request, session);
                       },
                       false, proto::M3Operation::MsgDelete});
    dispatcher.register_command(
        "msg react", {Tier::Write,
                      [&coordinator](const proto::Request& request, RequestSession& session) {
                          coordinator.react_to_message(request, session);
                      },
                      false, proto::M3Operation::MsgReact});
    dispatcher.register_command(
        "msg pin", {Tier::Write,
                    [&coordinator](const proto::Request& request, RequestSession& session) {
                        coordinator.pin_message(request, session, true);
                    },
                    false, proto::M3Operation::MsgPin});
    dispatcher.register_command(
        "msg unpin", {Tier::Write,
                      [&coordinator](const proto::Request& request, RequestSession& session) {
                          coordinator.pin_message(request, session, false);
                      },
                      false, proto::M3Operation::MsgUnpin});
    dispatcher.register_command(
        "chat mark-read", {Tier::Write,
                           [&coordinator](const proto::Request& request, RequestSession& session) {
                               coordinator.mark_chat_read(request, session);
                           },
                           false, proto::M3Operation::ChatMarkRead});
    dispatcher.register_command(
        "chat mute", {Tier::Write,
                      [&coordinator](const proto::Request& request, RequestSession& session) {
                          coordinator.mute_chat(request, session, true);
                      },
                      false, proto::M3Operation::ChatMute});
    dispatcher.register_command(
        "chat unmute", {Tier::Write,
                        [&coordinator](const proto::Request& request, RequestSession& session) {
                            coordinator.mute_chat(request, session, false);
                        },
                        false, proto::M3Operation::ChatUnmute});
    dispatcher.register_command(
        "chat pin", {Tier::Write,
                     [&coordinator](const proto::Request& request, RequestSession& session) {
                         coordinator.pin_chat(request, session, true);
                     },
                     false, proto::M3Operation::ChatPin});
    dispatcher.register_command(
        "chat unpin", {Tier::Write,
                       [&coordinator](const proto::Request& request, RequestSession& session) {
                           coordinator.pin_chat(request, session, false);
                       },
                       false, proto::M3Operation::ChatUnpin});
    dispatcher.register_command(
        "chat archive", {Tier::Write,
                         [&coordinator](const proto::Request& request, RequestSession& session) {
                             coordinator.archive_chat(request, session, true);
                         },
                         false, proto::M3Operation::ChatArchive});
    dispatcher.register_command(
        "chat unarchive", {Tier::Write,
                           [&coordinator](const proto::Request& request, RequestSession& session) {
                               coordinator.archive_chat(request, session, false);
                           },
                           false, proto::M3Operation::ChatUnarchive});
    dispatcher.register_command(
        "chat join", {Tier::Write,
                      [&coordinator](const proto::Request& request, RequestSession& session) {
                          coordinator.join_chat(request, session);
                      },
                      false, proto::M3Operation::ChatJoin});
    dispatcher.register_command(
        "chat leave", {Tier::Destructive,
                       [&coordinator](const proto::Request& request, RequestSession& session) {
                           coordinator.leave_chat(request, session);
                       },
                       false, proto::M3Operation::ChatLeave});
}

} // namespace tgcli::daemon
