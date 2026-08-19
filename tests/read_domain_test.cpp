#include "daemon/read_domain.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using tgcli::core::TdMessageContentKind;
using tgcli::core::TdMessageSenderKind;

tgcli::core::TdMessageSummary message(std::int64_t id, std::int32_t date,
                                      std::int64_t chat_id = -1001,
                                      std::optional<tgcli::core::TdTopic> topic = std::nullopt) {
    return {.id = id,
            .chat_id = chat_id,
            .date = date,
            .sender = {.kind = TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
            .is_outgoing = false,
            .topic = topic,
            .content_kind = TdMessageContentKind::Text,
            .text = "message"};
}

} // namespace

TEST_CASE("read timestamps implement the closed grammar and mathematical rounding",
          "[read][domain][timestamp]") {
    using tgcli::daemon::parse_read_timestamp;
    using tgcli::daemon::ReadTimestampBound;
    const auto epoch = std::chrono::system_clock::time_point{};

    CHECK(parse_read_timestamp("1970-01-01", ReadTimestampBound::Since, epoch) == 0);
    CHECK(parse_read_timestamp("1970-01-01", ReadTimestampBound::Until, epoch) == 86399);
    CHECK(parse_read_timestamp("1969-12-31T23:59:59.1Z", ReadTimestampBound::Since, epoch) == 0);
    CHECK(parse_read_timestamp("1969-12-31T23:59:59.1Z", ReadTimestampBound::Until, epoch) == -1);
    CHECK(parse_read_timestamp("1970-01-01T01:00:00+01:00", ReadTimestampBound::Since, epoch) == 0);
    CHECK(parse_read_timestamp("1970-01-01T00:00:00-00:00", ReadTimestampBound::Until, epoch) == 0);
    CHECK(parse_read_timestamp("1901-12-13T20:45:52Z", ReadTimestampBound::Since, epoch) ==
          std::numeric_limits<std::int32_t>::min());
    CHECK(parse_read_timestamp("2038-01-19T03:14:07Z", ReadTimestampBound::Until, epoch) ==
          std::numeric_limits<std::int32_t>::max());

    const auto fractional_now = std::chrono::system_clock::time_point{1000s + 500ms};
    CHECK(parse_read_timestamp("1m", ReadTimestampBound::Since, fractional_now) == 941);
    CHECK(parse_read_timestamp("1m", ReadTimestampBound::Until, fractional_now) == 940);
    const auto exact_now = std::chrono::system_clock::time_point{1000s};
    CHECK(parse_read_timestamp("1m", ReadTimestampBound::Since, exact_now) == 940);
    CHECK(parse_read_timestamp("1m", ReadTimestampBound::Until, exact_now) == 940);

    const std::string long_fraction = "1970-01-01T00:00:00." + std::string(4096, '0') + "1Z";
    CHECK(parse_read_timestamp(long_fraction, ReadTimestampBound::Since, epoch) == 1);
    CHECK(parse_read_timestamp(long_fraction, ReadTimestampBound::Until, epoch) == 0);

    for (const auto* invalid :
         {"19700101", "1970-01-01 00:00:00Z", "1970-01-01T00:00:00", "1970-01-01T00:00:00z",
          "1970-01-01T00:00:00+0000", "1970-01-01T00:00:60Z", "1970-01-01T24:00:00Z",
          "1970-01-01T00:00:00.Z", "1970-02-29", "0000-01-01", "01m", "+1m", "1M",
          "2038-01-19T03:14:08Z", "1901-12-13T20:45:51Z"}) {
        CHECK_FALSE(parse_read_timestamp(invalid, ReadTimestampBound::Since, epoch));
        CHECK_FALSE(parse_read_timestamp(invalid, ReadTimestampBound::Until, epoch));
    }
    CHECK(parse_read_timestamp("2000-02-29", ReadTimestampBound::Since, epoch));
}

TEST_CASE("read topic parser accepts only canonical ASCII positive decimals",
          "[read][domain][topic]") {
    using tgcli::daemon::parse_read_topic;
    using tgcli::daemon::TopicKind;
    CHECK(parse_read_topic("7") == tgcli::daemon::TopicRef{TopicKind::Forum, 7});
    CHECK(parse_read_topic("forum:2147483647") ==
          tgcli::daemon::TopicRef{TopicKind::Forum, 2147483647});
    CHECK(parse_read_topic("thread:9007199254740991") ==
          tgcli::daemon::TopicRef{TopicKind::Thread, 9007199254740991LL});
    CHECK(parse_read_topic("direct:8") == tgcli::daemon::TopicRef{TopicKind::Direct, 8});
    CHECK(parse_read_topic("saved:9") == tgcli::daemon::TopicRef{TopicKind::Saved, 9});
    for (const auto* invalid :
         {"", "+1", "-1", "0", "01", "forum:0", "forum:01", "forum:2147483648",
          "thread:9007199254740992", "thread:+1", "thread:١", " thread:1", "thread:1 "}) {
        CHECK_FALSE(parse_read_topic(invalid));
    }
}

TEST_CASE("read cursor is canonical unsigned state with closed cross-field rules",
          "[read][domain][cursor]") {
    tgcli::daemon::ReadCursor cursor{
        .version = 1,
        .operation = "read",
        .account = "main",
        .user_id = 42,
        .limit = 20,
        .chat_id = -1001,
        .history_chat_id = -1002,
        .topic = tgcli::daemon::TopicRef{tgcli::daemon::TopicKind::Thread, 123},
        .local = false,
        .since = -10,
        .until = 20,
        .since_cutoff_message_id = 90,
        .from_message_id = 100};
    const auto token = tgcli::daemon::encode_read_cursor(cursor);
    CHECK(tgcli::daemon::decode_read_cursor(token) == cursor);
    CHECK_FALSE(tgcli::daemon::decode_read_cursor(token + "="));

    auto caller_modified = cursor;
    caller_modified.chat_id = -2001;
    caller_modified.history_chat_id = -2002;
    const auto modified_token = tgcli::daemon::encode_read_cursor(caller_modified);
    CHECK(tgcli::daemon::decode_read_cursor(modified_token) == caller_modified);

    auto invalid = cursor;
    invalid.local = true;
    CHECK_FALSE(tgcli::daemon::decode_read_cursor(tgcli::daemon::encode_read_cursor(invalid)));
    invalid = cursor;
    invalid.topic = tgcli::daemon::TopicRef{tgcli::daemon::TopicKind::Forum, 7};
    CHECK_FALSE(tgcli::daemon::decode_read_cursor(tgcli::daemon::encode_read_cursor(invalid)));
    invalid = cursor;
    invalid.since.reset();
    CHECK_FALSE(tgcli::daemon::decode_read_cursor(tgcli::daemon::encode_read_cursor(invalid)));
}

TEST_CASE("read scanner advances raw anchors independently of filters and stops on exact cutoff",
          "[read][domain][scanner]") {
    const tgcli::core::TdTopic forum{
        .kind = tgcli::core::TdTopicKind::Forum, .id = 7, .tdlib_type_id = 1};
    const tgcli::core::TdMessages page{
        .total_count = 4,
        .messages = {message(100, 1000), message(99, 990, -1001, forum),
                     message(98, 980, -1001, forum), message(97, 970, -1001, forum)}};
    const auto scanned = tgcli::daemon::scan_read_page(
        page, {.history_chat_id = -1001,
               .topic = tgcli::daemon::TopicRef{tgcli::daemon::TopicKind::Forum, 7},
               .since = 970,
               .until = 995,
               .since_cutoff_message_id = 97,
               .exclusive_anchor = 100,
               .remaining = 20});
    CHECK(scanned.error == tgcli::daemon::ReadScanError::None);
    REQUIRE(scanned.items.size() == 2);
    CHECK(scanned.items[0].id == 99);
    CHECK(scanned.items[1].id == 98);
    CHECK(scanned.last_consumed_message_id == 97);
    CHECK(scanned.reached_time_anchor);

    const auto filtered = tgcli::daemon::scan_read_page(
        {.total_count = 2, .messages = {message(50, 0), message(49, 1)}},
        {.history_chat_id = -1001,
         .topic = std::nullopt,
         .since = 10,
         .until = std::nullopt,
         .since_cutoff_message_id = std::nullopt,
         .exclusive_anchor = std::nullopt,
         .remaining = 20});
    CHECK(filtered.items.empty());
    CHECK(filtered.last_consumed_message_id == 49);
    CHECK_FALSE(filtered.reached_time_anchor);
}

TEST_CASE("read scanner distinguishes structural failures from non-advancing pagination",
          "[read][domain][scanner][error]") {
    const tgcli::daemon::ReadScanInput input{.history_chat_id = -1001,
                                             .topic = std::nullopt,
                                             .since = std::nullopt,
                                             .until = std::nullopt,
                                             .since_cutoff_message_id = std::nullopt,
                                             .exclusive_anchor = 100,
                                             .remaining = 20};
    CHECK(tgcli::daemon::scan_read_page({.total_count = 0, .messages = {message(99, 1)}}, input)
              .error == tgcli::daemon::ReadScanError::Internal);
    CHECK(tgcli::daemon::scan_read_page(
              {.total_count = 2, .messages = {message(99, 1), std::nullopt}}, input)
              .error == tgcli::daemon::ReadScanError::Internal);
    CHECK(tgcli::daemon::scan_read_page({.total_count = 1, .messages = {message(99, 1, -1002)}},
                                        input)
              .error == tgcli::daemon::ReadScanError::Internal);
    CHECK(tgcli::daemon::scan_read_page({.total_count = 1, .messages = {message(100, 1)}}, input)
              .error == tgcli::daemon::ReadScanError::None);
    CHECK_FALSE(
        tgcli::daemon::scan_read_page({.total_count = 1, .messages = {message(100, 1)}}, input)
            .last_consumed_message_id);
    CHECK(tgcli::daemon::scan_read_page({.total_count = 1, .messages = {message(101, 1)}}, input)
              .error == tgcli::daemon::ReadScanError::NonAdvancing);
    CHECK(tgcli::daemon::scan_read_page(
              {.total_count = 2, .messages = {message(99, 1), message(99, 1)}}, input)
              .error == tgcli::daemon::ReadScanError::NonAdvancing);
    CHECK(tgcli::daemon::scan_read_page(
              {.total_count = 8, .messages = {std::nullopt, std::nullopt}}, input)
              .error == tgcli::daemon::ReadScanError::None);
}
