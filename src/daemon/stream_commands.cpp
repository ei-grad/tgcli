#include "daemon/stream_commands.hpp"

#include "common/exit_codes.hpp"
#include "common/utf8.hpp"
#include "daemon/resolver.hpp"
#include "daemon/stream_metadata.hpp"
#include "daemon/stream_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <re2/re2.h>
#include <set>
#include <string_view>
#include <utility>

namespace tgcli::daemon {

namespace {

using nlohmann::json;

bool exact_fields(const json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_object() || value.size() != names.size()) {
        return false;
    }
    return std::ranges::all_of(names, [&](std::string_view name) { return value.contains(name); });
}

StreamArgumentError malformed(std::string message, std::string argument = {}) {
    return {.message = std::move(message),
            .argument = std::move(argument),
            .reason = "invalid_argument"};
}

std::optional<std::uint64_t> unsigned_integer(const json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    const auto parsed = value.get<std::int64_t>();
    return parsed < 0 ? std::nullopt : std::optional<std::uint64_t>{parsed};
}

std::optional<std::int64_t> signed_integer(const json& value) {
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

std::optional<StreamEventClass> stream_event_class(std::string_view name) {
    static constexpr std::array names{std::pair{"message", StreamEventClass::Message},
                                      std::pair{"edit", StreamEventClass::Edit},
                                      std::pair{"delete", StreamEventClass::Delete},
                                      std::pair{"reaction", StreamEventClass::Reaction},
                                      std::pair{"chat", StreamEventClass::Chat}};
    const auto* const found = std::ranges::find(names, name, &decltype(names)::value_type::first);
    return found == names.end() ? std::nullopt : std::optional{found->second};
}

std::optional<MessageSenderRef> parse_message_sender(const json& value) {
    if (!exact_fields(value, {"type", "id"}) || !value["type"].is_string() ||
        !value["id"].is_number_integer()) {
        return std::nullopt;
    }
    const auto id = signed_integer(value["id"]);
    if (!id) {
        return std::nullopt;
    }
    const auto& type = value["type"].get_ref<const std::string&>();
    if (type == "user") {
        return MessageSenderRef{MessageSenderKind::User, *id};
    }
    if (type == "chat") {
        return MessageSenderRef{MessageSenderKind::Chat, *id};
    }
    return std::nullopt;
}

bool parse_message_topic(const json& value, std::optional<TopicRef>& output) {
    if (value.is_null()) {
        output = std::nullopt;
        return true;
    }
    if (!exact_fields(value, {"kind", "id"}) || !value["kind"].is_string() ||
        !value["id"].is_number_integer()) {
        return false;
    }
    const auto id = signed_integer(value["id"]);
    if (!id) {
        return false;
    }
    const auto& kind = value["kind"].get_ref<const std::string&>();
    if (kind == "forum") {
        output = TopicRef{TopicKind::Forum, *id};
    } else if (kind == "thread") {
        output = TopicRef{TopicKind::Thread, *id};
    } else if (kind == "direct") {
        output = TopicRef{TopicKind::Direct, *id};
    } else if (kind == "saved") {
        output = TopicRef{TopicKind::Saved, *id};
    } else {
        return false;
    }
    return true;
}

std::optional<MessageContentKind> parse_message_content(std::string_view type) {
    static constexpr std::array names{std::pair{"text", MessageContentKind::Text},
                                      std::pair{"photo", MessageContentKind::Photo},
                                      std::pair{"video", MessageContentKind::Video},
                                      std::pair{"doc", MessageContentKind::Document},
                                      std::pair{"voice", MessageContentKind::Voice},
                                      std::pair{"other", MessageContentKind::Other}};
    const auto* const found = std::ranges::find(names, type, &decltype(names)::value_type::first);
    return found == names.end() ? std::nullopt : std::optional{found->second};
}

std::string_view stream_operation_name(StreamOperation operation) {
    return operation == StreamOperation::Listen ? "listen" : "wait_for";
}

StreamTerminalErrorFrame terminal_error(std::string code, std::string message,
                                        nlohmann::json details, int exit_code) {
    return {.code = std::move(code),
            .message = std::move(message),
            .details = std::move(details),
            .exit_code = exit_code};
}

StreamTerminalFrame internal_terminal(StreamOperation operation) {
    return terminal_error(
        "INTERNAL", "stream processing failed",
        {{"operation", stream_operation_name(operation)}, {"reason", "internal_error"}}, kGeneric);
}

StreamTerminalFrame overflow_terminal(const StreamTerminalPayload& terminal) {
    const auto operation = stream_operation_name(terminal.operation);
    nlohmann::json details{{"operation", operation}};
    switch (terminal.cause) {
    case StreamTerminalCause::CounterExhausted:
        details["cause"] = "counter_exhausted";
        break;
    case StreamTerminalCause::ItemBytes:
        details["cause"] = "item_bytes";
        details["limit_bytes"] = kStreamQueueItemBytes;
        details["incoming_bytes"] = terminal.incoming_bytes;
        break;
    case StreamTerminalCause::QueueItems:
        details["cause"] = "queue_items";
        break;
    case StreamTerminalCause::QueueBytes:
        details["cause"] = "queue_bytes";
        break;
    case StreamTerminalCause::HistoryOverlap:
        details["cause"] = "history_overlap";
        break;
    default:
        return internal_terminal(terminal.operation);
    }
    if (terminal.cause == StreamTerminalCause::QueueItems ||
        terminal.cause == StreamTerminalCause::QueueBytes ||
        terminal.cause == StreamTerminalCause::HistoryOverlap) {
        details["limit_items"] = terminal.limit_items;
        details["limit_bytes"] = terminal.limit_bytes;
        details["queued_items"] = terminal.queued_items;
        details["queued_bytes"] = terminal.queued_bytes;
        details["incoming_bytes"] = terminal.incoming_bytes;
    }
    return terminal_error("STREAM_OVERFLOW", "stream buffer capacity was exceeded",
                          std::move(details), kGeneric);
}

StreamTerminalFrame metadata_terminal(const StreamTerminalPayload& terminal) {
    const auto operation = stream_operation_name(terminal.operation);
    const auto& failure = terminal.metadata_failure;
    if (failure.kind == StreamFailureKind::Capacity) {
        const auto details = stream_metadata_capacity_details(failure.capacity, operation);
        return details
                   ? StreamTerminalFrame{terminal_error("STREAM_CAPACITY",
                                                        "stream service capacity is unavailable",
                                                        *details, kGeneric)}
                   : internal_terminal(terminal.operation);
    }
    if (failure.kind == StreamFailureKind::RateLimited) {
        return terminal_error(
            "RATE_LIMITED", "Telegram rate limit",
            {{"operation", operation}, {"tdlib_code", 429}, {"retry_after", failure.retry_after}},
            kRateLimited);
    }
    if (failure.kind == StreamFailureKind::TdlibError) {
        return terminal_error("TDLIB_ERROR", "stream TDLib request failed",
                              {{"operation", operation}, {"tdlib_code", failure.tdlib_error_code}},
                              kGeneric);
    }
    return internal_terminal(terminal.operation);
}

std::optional<StreamArgumentError> parse_listen_types(const json& values, std::uint8_t& type_mask) {
    if (values.is_null()) {
        type_mask = all_stream_event_mask();
        return std::nullopt;
    }
    if (values.empty()) {
        return malformed("listen types must be a nonempty comma-list", "--types");
    }
    type_mask = 0;
    for (const auto& value : values) {
        if (!value.is_string()) {
            return malformed("listen type is invalid", "--types");
        }
        const auto event = stream_event_class(value.get_ref<const std::string&>());
        if (!event) {
            return malformed("listen type is invalid", "--types");
        }
        const auto mask = stream_event_mask(*event);
        if ((type_mask & mask) != 0) {
            return malformed("listen types must not contain duplicates", "--types");
        }
        type_mask |= mask;
    }
    return std::nullopt;
}

} // namespace

ListenArgumentsResult parse_listen_arguments(const nlohmann::json& args) {
    if (!exact_fields(args, {"chats", "types", "count"}) || !args["chats"].is_array() ||
        !(args["types"].is_null() || args["types"].is_array()) ||
        !(args["count"].is_null() || args["count"].is_number_integer())) {
        return malformed("listen received malformed arguments");
    }

    ListenArguments result;
    if (args["chats"].size() > kStreamChatFilters) {
        return malformed("listen accepts at most 64 chat selectors", "--chat");
    }
    for (const auto& value : args["chats"]) {
        if (!value.is_string()) {
            return malformed("listen chat selector is invalid", "--chat");
        }
        auto selector = value.get<std::string>();
        if (!valid_resolve_selector(selector)) {
            return malformed("listen chat selector is invalid", "--chat");
        }
        result.chat_selectors.push_back(std::move(selector));
    }

    if (auto error = parse_listen_types(args["types"], result.type_mask)) {
        return std::move(*error);
    }

    if (!args["count"].is_null()) {
        const auto count = unsigned_integer(args["count"]);
        if (!count || *count == 0 || *count > kMaximumStreamCount) {
            return malformed("listen count must be between 1 and 1000000", "--count");
        }
        result.count = count;
    }
    return result;
}

WaitForArgumentsResult parse_wait_for_arguments(const nlohmann::json& args) {
    if (!exact_fields(args, {"chat", "from", "regex", "after"}) ||
        !(args["chat"].is_null() || args["chat"].is_string()) ||
        !(args["from"].is_null() || args["from"].is_string()) ||
        !(args["regex"].is_null() || args["regex"].is_string()) ||
        !(args["after"].is_null() || args["after"].is_number_integer())) {
        return malformed("wait-for received malformed arguments");
    }

    WaitForArguments result;
    const auto parse_selector =
        [](const json& value, std::string_view argument,
           std::optional<std::string>& output) -> std::optional<StreamArgumentError> {
        if (value.is_null()) {
            return std::nullopt;
        }
        auto selector = value.get<std::string>();
        if (!valid_resolve_selector(selector)) {
            return malformed("wait-for selector is invalid", std::string(argument));
        }
        output = std::move(selector);
        return std::nullopt;
    };
    if (auto error = parse_selector(args["chat"], "--chat", result.chat_selector)) {
        return std::move(*error);
    }
    if (auto error = parse_selector(args["from"], "--from", result.from_selector)) {
        return std::move(*error);
    }
    if (!args["regex"].is_null()) {
        const auto& pattern = args["regex"].get_ref<const std::string&>();
        if (pattern.empty() || pattern.size() > kMaximumStreamRegexBytes ||
            !common::valid_utf8(pattern)) {
            return malformed("wait-for regex must be valid UTF-8 between 1 and 4096 bytes",
                             "--regex");
        }
        result.regex_pattern = pattern;
    }
    if (!args["after"].is_null()) {
        const auto after = signed_integer(args["after"]);
        if (!after || *after <= 0 || *after > kMaximumStreamMessageId) {
            return malformed("wait-for after must be a positive int53 message id", "--after");
        }
        if (!result.chat_selector) {
            return StreamArgumentError{.message = "wait-for after requires a chat selector",
                                       .argument = "--after",
                                       .reason = "missing_argument"};
        }
        result.after = after;
    }
    return result;
}

std::optional<StreamArgumentError> validate_stream_timeout(std::optional<double> timeout_seconds) {
    if (!timeout_seconds) {
        return std::nullopt;
    }
    if (!std::isfinite(*timeout_seconds) || *timeout_seconds < kMinimumStreamTimeoutSeconds ||
        *timeout_seconds > kMaximumStreamTimeoutSeconds) {
        return malformed("stream timeout must be between 0.001 and 31536000 seconds", "--timeout");
    }
    return std::nullopt;
}

StreamRegex::StreamRegex(std::unique_ptr<re2::RE2> expression) noexcept
    : expression_(std::move(expression)) {}

StreamRegex::~StreamRegex() = default;
StreamRegex::StreamRegex(StreamRegex&&) noexcept = default;
StreamRegex& StreamRegex::operator=(StreamRegex&&) noexcept = default;

bool StreamRegex::matches(std::string_view text) const noexcept {
    return expression_ != nullptr && re2::RE2::PartialMatch(text, *expression_);
}

StreamRegexResult compile_stream_regex(std::string_view pattern) {
    if (pattern.empty() || pattern.size() > kMaximumStreamRegexBytes ||
        !common::valid_utf8(pattern)) {
        return malformed("wait-for regex must be valid UTF-8 between 1 and 4096 bytes", "--regex");
    }
    re2::RE2::Options options;
    options.set_encoding(re2::RE2::Options::EncodingUTF8);
    options.set_case_sensitive(true);
    options.set_log_errors(false);
    options.set_max_mem(kStreamRegexMaxMemory);
    auto expression = std::make_unique<re2::RE2>(pattern, options);
    if (!expression->ok()) {
        return malformed("wait-for regex is invalid", "--regex");
    }
    return StreamRegex(std::move(expression));
}

bool StreamMessageMatcher::matches(const MessageSummary& message) const noexcept {
    if (sender_user_id &&
        (message.sender.kind != MessageSenderKind::User || message.sender.id != *sender_user_id)) {
        return false;
    }
    return regex == nullptr || regex->matches(message.text);
}

std::optional<MessageSummary> parse_stream_message_summary(const nlohmann::json& value) {
    if (!exact_fields(
            value, {"id", "chat_id", "date", "sender", "is_outgoing", "topic", "type", "text"}) ||
        !value["id"].is_number_integer() || !value["chat_id"].is_number_integer() ||
        !(value["date"].is_null() || value["date"].is_string()) || !value["sender"].is_object() ||
        !value["is_outgoing"].is_boolean() ||
        !(value["topic"].is_null() || value["topic"].is_object()) || !value["type"].is_string() ||
        !value["text"].is_string()) {
        return std::nullopt;
    }
    const auto id = signed_integer(value["id"]);
    const auto chat_id = signed_integer(value["chat_id"]);
    const auto sender = parse_message_sender(value["sender"]);
    if (!id || !chat_id || !sender) {
        return std::nullopt;
    }
    std::optional<TopicRef> topic;
    if (!parse_message_topic(value["topic"], topic)) {
        return std::nullopt;
    }
    const auto content = parse_message_content(value["type"].get_ref<const std::string&>());
    if (!content) {
        return std::nullopt;
    }
    MessageSummary result{.id = *id,
                          .chat_id = *chat_id,
                          .date = value["date"].is_string()
                                      ? std::optional<std::string>{value["date"].get<std::string>()}
                                      : std::nullopt,
                          .sender = *sender,
                          .is_outgoing = value["is_outgoing"].get<bool>(),
                          .topic = topic,
                          .type = *content,
                          .text = value["text"].get<std::string>()};
    return valid_stream_message(result) ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<MessageSummary> parse_stream_message_item(const StreamCopiedItem& item) {
    if (item.descriptor.event_class != StreamEventClass::Message || !item.data.is_object() ||
        item.data.size() != 2 || item.data.value("event", "") != "message" ||
        !item.data.contains("message")) {
        return std::nullopt;
    }
    auto message = parse_stream_message_summary(item.data["message"]);
    if (!message || message->chat_id != item.descriptor.chat_id ||
        (message->sender.kind == MessageSenderKind::User
             ? item.descriptor.sender_kind != StreamSenderKind::User
             : item.descriptor.sender_kind != StreamSenderKind::Chat) ||
        message->sender.id != item.descriptor.sender_id) {
        return std::nullopt;
    }
    return message;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed terminal cause union.
StreamTerminalFrame stream_terminal_frame(const StreamTerminalPayload& terminal,
                                          std::uint64_t delivered_count, std::string_view account) {
    static_cast<void>(delivered_count);
    const auto operation = stream_operation_name(terminal.operation);
    switch (terminal.cause) {
    case StreamTerminalCause::PlannedSuccess:
        return StreamTerminalResultFrame{};
    case StreamTerminalCause::Deadline:
        if (terminal.operation == StreamOperation::Listen) {
            return StreamTerminalResultFrame{};
        }
        return terminal_error("TIMEOUT", "wait-for request timed out",
                              {{"operation", operation}, {"state", nullptr}}, kTimeout);
    case StreamTerminalCause::AuthorizationLost:
        return terminal_error(
            "NOT_AUTHED", "stream requires an authenticated account",
            {{"account", account},
             {"state", core::auth_state_name(static_cast<core::AuthState>(terminal.auth_state))},
             {"reason", "authorization_lost"}},
            kNotAuthed);
    case StreamTerminalCause::GenerationReplaced:
        return terminal_error(
            "NOT_AUTHED", "stream authorization generation was replaced",
            {{"account", account}, {"state", "closed"}, {"reason", "authorization_lost"}},
            kNotAuthed);
    case StreamTerminalCause::Shutdown:
        return terminal_error("DAEMON_SHUTDOWN", "daemon is shutting down",
                              {{"reason", "daemon_shutdown"}}, kGeneric);
    case StreamTerminalCause::MetadataFailure:
        return metadata_terminal(terminal);
    case StreamTerminalCause::TdlibError:
        return terminal_error("TDLIB_ERROR", "stream TDLib request failed",
                              {{"operation", operation}, {"tdlib_code", terminal.tdlib_code}},
                              kGeneric);
    case StreamTerminalCause::RateLimited:
        return terminal_error(
            "RATE_LIMITED", "Telegram rate limit",
            {{"operation", operation}, {"tdlib_code", 429}, {"retry_after", terminal.retry_after}},
            kRateLimited);
    case StreamTerminalCause::PaginationInvalid:
        return terminal_error("PAGINATION_INVALID", "stream pagination did not advance",
                              {{"operation", operation}, {"reason", "non_advancing_upstream"}},
                              kGeneric);
    case StreamTerminalCause::CounterExhausted:
    case StreamTerminalCause::ItemBytes:
    case StreamTerminalCause::QueueItems:
    case StreamTerminalCause::QueueBytes:
    case StreamTerminalCause::HistoryOverlap:
        return overflow_terminal(terminal);
    case StreamTerminalCause::Internal:
    case StreamTerminalCause::Disconnected:
    case StreamTerminalCause::ProtocolAnswerInvalid:
    case StreamTerminalCause::Open:
    case StreamTerminalCause::ClaimingCounterExhausted:
    case StreamTerminalCause::ClaimingItemBytes:
    case StreamTerminalCause::ClaimingQueueItems:
    case StreamTerminalCause::ClaimingQueueBytes:
    case StreamTerminalCause::ClaimingHistoryOverlap:
    case StreamTerminalCause::ClaimingTdlibError:
    case StreamTerminalCause::ClaimingRateLimited:
    case StreamTerminalCause::ClaimingPaginationInvalid:
    case StreamTerminalCause::ClaimingInternal:
    case StreamTerminalCause::ClaimingAuthorizationLost:
    case StreamTerminalCause::ClaimingGenerationReplaced:
    case StreamTerminalCause::ClaimingShutdown:
    case StreamTerminalCause::ClaimingMetadataFailure:
    case StreamTerminalCause::ClaimingPlannedSuccess:
    case StreamTerminalCause::ClaimingDeadline:
    case StreamTerminalCause::ClaimingDisconnected:
    case StreamTerminalCause::ClaimingProtocolAnswerInvalid:
        return internal_terminal(terminal.operation);
    }
    return internal_terminal(terminal.operation);
}

StreamTerminalErrorFrame stream_admission_error(const StreamIngressAdmissionFailure& failure,
                                                StreamOperation operation) {
    const auto operation_name = stream_operation_name(operation);
    if (failure.resource == StreamIngressAdmissionResource::SubscriberSlots) {
        return terminal_error("STREAM_CAPACITY", "stream service capacity is unavailable",
                              {{"operation", operation_name},
                               {"phase", "admission"},
                               {"resource", "subscriber_slots"},
                               {"limit", kStreamSubscriberSlots}},
                              kGeneric);
    }
    std::string_view atomic = "slot_pointer";
    switch (failure.atomic) {
    case StreamIngressAtomic::SlotPointer:
        break;
    case StreamIngressAtomic::PublisherCount:
        atomic = "publisher_count";
        break;
    case StreamIngressAtomic::DescriptorIndex:
        atomic = "descriptor_index";
        break;
    case StreamIngressAtomic::ByteIndex:
        atomic = "byte_index";
        break;
    case StreamIngressAtomic::TerminalCause:
        atomic = "terminal_cause";
        break;
    }
    return terminal_error("STREAM_CAPACITY", "stream service capacity is unavailable",
                          {{"operation", operation_name},
                           {"phase", "admission"},
                           {"resource", "lock_free_ingress"},
                           {"atomic", atomic}},
                          kGeneric);
}

} // namespace tgcli::daemon
