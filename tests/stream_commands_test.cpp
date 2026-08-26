#include "common/exit_codes.hpp"
#include "daemon/stream_commands.hpp"

#include <limits>

#include <catch2/catch_test_macros.hpp>

using nlohmann::json;
using namespace tgcli::daemon;

TEST_CASE("stream command arguments have closed defaults and types", "[stream][commands]") {
    const auto listen =
        parse_listen_arguments({{"chats", json::array()}, {"types", nullptr}, {"count", nullptr}});
    REQUIRE(std::holds_alternative<ListenArguments>(listen));
    const auto& defaults = std::get<ListenArguments>(listen);
    CHECK(defaults.chat_selectors.empty());
    CHECK(defaults.type_mask == all_stream_event_mask());
    CHECK_FALSE(defaults.count.has_value());

    const auto typed =
        parse_listen_arguments({{"chats", json::array({"@one", "-1002"})},
                                {"types", json::array({"message", "delete", "chat"})},
                                {"count", 17}});
    REQUIRE(std::holds_alternative<ListenArguments>(typed));
    CHECK(std::get<ListenArguments>(typed).type_mask ==
          (stream_event_mask(StreamEventClass::Message) |
           stream_event_mask(StreamEventClass::Delete) |
           stream_event_mask(StreamEventClass::Chat)));
    CHECK(std::get<ListenArguments>(typed).count == 17);
}

TEST_CASE("stream command arguments reject malformed closed forms", "[stream][commands]") {
    const auto duplicate = parse_listen_arguments({{"chats", json::array()},
                                                   {"types", json::array({"message", "message"})},
                                                   {"count", nullptr}});
    REQUIRE(std::holds_alternative<StreamArgumentError>(duplicate));
    CHECK(std::get<StreamArgumentError>(duplicate).argument == "--types");

    const auto wait = parse_wait_for_arguments(
        {{"chat", nullptr}, {"from", nullptr}, {"regex", nullptr}, {"after", 1}});
    REQUIRE(std::holds_alternative<StreamArgumentError>(wait));
    CHECK(std::get<StreamArgumentError>(wait).argument == "--after");
    CHECK(std::get<StreamArgumentError>(wait).reason == "missing_argument");
}

TEST_CASE("stream timeout accepts only the finite canonical interval", "[stream][commands]") {
    CHECK_FALSE(validate_stream_timeout(std::nullopt));
    CHECK_FALSE(validate_stream_timeout(0.001));
    CHECK_FALSE(validate_stream_timeout(31'536'000.0));
    for (const double timeout :
         {0.0, 0.0009, 31'536'000.001, std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()}) {
        const auto failure = validate_stream_timeout(timeout);
        REQUIRE(failure);
        CHECK(failure->argument == "--timeout");
        CHECK(failure->reason == "invalid_argument");
    }
}

TEST_CASE("stream regex uses exact UTF8 partial matching policy", "[stream][commands]") {
    auto compiled = compile_stream_regex("target-[0-9]+$");
    REQUIRE(std::holds_alternative<StreamRegex>(compiled));
    CHECK(std::get<StreamRegex>(compiled).matches("prefix target-42"));
    CHECK_FALSE(std::get<StreamRegex>(compiled).matches("prefix TARGET-42"));

    CHECK(std::holds_alternative<StreamArgumentError>(compile_stream_regex("(")));
}

TEST_CASE("stream terminal mapper preserves listen silence and wait timeout",
          "[stream][commands][terminal]") {
    const auto listen = stream_terminal_frame({.cause = StreamTerminalCause::Deadline,
                                               .operation = StreamOperation::Listen,
                                               .metadata_failure = {}},
                                              4, "main");
    REQUIRE(std::holds_alternative<StreamTerminalResultFrame>(listen));
    CHECK(std::get<StreamTerminalResultFrame>(listen).data.empty());

    const auto wait = stream_terminal_frame({.cause = StreamTerminalCause::Deadline,
                                             .operation = StreamOperation::WaitFor,
                                             .metadata_failure = {}},
                                            0, "main");
    REQUIRE(std::holds_alternative<StreamTerminalErrorFrame>(wait));
    const auto& error = std::get<StreamTerminalErrorFrame>(wait);
    CHECK(error.code == "TIMEOUT");
    CHECK(error.details == json{{"operation", "wait_for"}, {"state", nullptr}});
    CHECK(error.exit_code == tgcli::kTimeout);
}

TEST_CASE("stream terminal mapper emits exact overlap and rate payloads",
          "[stream][commands][terminal]") {
    const auto overflow = stream_terminal_frame({.cause = StreamTerminalCause::HistoryOverlap,
                                                 .operation = StreamOperation::WaitFor,
                                                 .limit_items = kStreamQueueItems,
                                                 .limit_bytes = kStreamQueueBytes,
                                                 .queued_items = 8,
                                                 .queued_bytes = 800,
                                                 .incoming_bytes = 90,
                                                 .metadata_failure = {}},
                                                0, "main");
    REQUIRE(std::holds_alternative<StreamTerminalErrorFrame>(overflow));
    CHECK(std::get<StreamTerminalErrorFrame>(overflow).details == json{{"operation", "wait_for"},
                                                                       {"cause", "history_overlap"},
                                                                       {"limit_items", 1024},
                                                                       {"limit_bytes", 8'388'608},
                                                                       {"queued_items", 8},
                                                                       {"queued_bytes", 800},
                                                                       {"incoming_bytes", 90}});

    const auto rate = stream_terminal_frame({.cause = StreamTerminalCause::RateLimited,
                                             .operation = StreamOperation::WaitFor,
                                             .tdlib_code = 429,
                                             .retry_after = 17,
                                             .metadata_failure = {}},
                                            0, "main");
    REQUIRE(std::holds_alternative<StreamTerminalErrorFrame>(rate));
    CHECK(std::get<StreamTerminalErrorFrame>(rate).details ==
          json{{"operation", "wait_for"}, {"tdlib_code", 429}, {"retry_after", 17}});
}

TEST_CASE("stream message matcher uses user sender then decoded text",
          "[stream][commands][filter]") {
    auto compiled = compile_stream_regex("^caption Ω$");
    REQUIRE(std::holds_alternative<StreamRegex>(compiled));
    const StreamMessageMatcher matcher{
        .sender_user_id = 42,
        .regex = std::make_shared<StreamRegex>(std::move(std::get<StreamRegex>(compiled)))};
    MessageSummary message{.id = 9,
                           .chat_id = -1001,
                           .date = "2026-01-01T00:00:00Z",
                           .sender = {.kind = MessageSenderKind::User, .id = 42},
                           .is_outgoing = false,
                           .topic = std::nullopt,
                           .type = MessageContentKind::Photo,
                           .text = "caption Ω"};
    CHECK(matcher.matches(message));
    message.sender = {.kind = MessageSenderKind::Chat, .id = 42};
    CHECK_FALSE(matcher.matches(message));
    message.sender = {.kind = MessageSenderKind::User, .id = 42};
    message.text = "caption \\u03a9";
    CHECK_FALSE(matcher.matches(message));
}
