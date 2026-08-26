#pragma once

#include "daemon/message_summary.hpp"
#include "daemon/stream_storage.hpp"
#include "daemon/stream_subscription.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace re2 {
class RE2;
}

namespace tgcli::daemon {

inline constexpr std::uint64_t kMaximumStreamCount = 1'000'000;
inline constexpr std::int64_t kMaximumStreamMessageId = 9'007'199'254'740'991LL;
inline constexpr std::size_t kMaximumStreamRegexBytes = 4'096;
inline constexpr std::int64_t kStreamRegexMaxMemory = 1'048'576;
inline constexpr double kMinimumStreamTimeoutSeconds = 0.001;
inline constexpr double kMaximumStreamTimeoutSeconds = 31'536'000.0;

constexpr std::uint8_t all_stream_event_mask() noexcept {
    return stream_event_mask(StreamEventClass::Message) |
           stream_event_mask(StreamEventClass::Edit) | stream_event_mask(StreamEventClass::Delete) |
           stream_event_mask(StreamEventClass::Reaction) |
           stream_event_mask(StreamEventClass::Chat);
}

struct StreamArgumentError {
    std::string message;
    std::string argument;
    std::string reason = "invalid_argument";

    bool operator==(const StreamArgumentError&) const = default;
};

struct ListenArguments {
    std::vector<std::string> chat_selectors;
    std::uint8_t type_mask = all_stream_event_mask();
    std::optional<std::uint64_t> count;
};

struct WaitForArguments {
    std::optional<std::string> chat_selector;
    std::optional<std::string> from_selector;
    std::optional<std::string> regex_pattern;
    std::optional<std::int64_t> after;
};

using ListenArgumentsResult = std::variant<ListenArguments, StreamArgumentError>;
using WaitForArgumentsResult = std::variant<WaitForArguments, StreamArgumentError>;

[[nodiscard]] ListenArgumentsResult parse_listen_arguments(const nlohmann::json& args);
[[nodiscard]] WaitForArgumentsResult parse_wait_for_arguments(const nlohmann::json& args);
[[nodiscard]] std::optional<StreamArgumentError>
validate_stream_timeout(std::optional<double> timeout_seconds);

class StreamRegex {
  public:
    ~StreamRegex();
    StreamRegex(const StreamRegex&) = delete;
    StreamRegex& operator=(const StreamRegex&) = delete;
    StreamRegex(StreamRegex&&) noexcept;
    StreamRegex& operator=(StreamRegex&&) noexcept;

    [[nodiscard]] bool matches(std::string_view text) const noexcept;

  private:
    explicit StreamRegex(std::unique_ptr<re2::RE2> expression) noexcept;

    std::unique_ptr<re2::RE2> expression_;

    friend std::variant<StreamRegex, StreamArgumentError>
    compile_stream_regex(std::string_view pattern);
};

using StreamRegexResult = std::variant<StreamRegex, StreamArgumentError>;

[[nodiscard]] StreamRegexResult compile_stream_regex(std::string_view pattern);

struct StreamMessageMatcher {
    std::optional<std::int64_t> sender_user_id;
    std::shared_ptr<const StreamRegex> regex;

    [[nodiscard]] bool matches(const MessageSummary& message) const noexcept;
};

[[nodiscard]] std::optional<MessageSummary>
parse_stream_message_summary(const nlohmann::json& value);
[[nodiscard]] std::optional<MessageSummary> parse_stream_message_item(const StreamCopiedItem& item);

[[nodiscard]] StreamTerminalFrame stream_terminal_frame(const StreamTerminalPayload& terminal,
                                                        std::uint64_t delivered_count,
                                                        std::string_view account);
[[nodiscard]] StreamTerminalErrorFrame
stream_admission_error(const StreamIngressAdmissionFailure& failure, StreamOperation operation);

} // namespace tgcli::daemon
