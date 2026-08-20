#include "daemon/fetch_domain.hpp"
#include "schema_matcher.hpp"

#include <cstdint>
#include <limits>
#include <optional>

#include <catch2/catch_test_macros.hpp>

using nlohmann::json;

namespace {

tgcli::core::TdMessageSummary message(std::int64_t chat_id, std::int64_t id,
                                      std::int32_t date = 20) {
    return {
        .id = id,
        .chat_id = chat_id,
        .date = date,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 1},
        .is_outgoing = false,
        .topic = std::nullopt,
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "message",
    };
}

} // namespace

TEST_CASE("fetch targets and signed-int32 timestamps retain their closed representation",
          "[fetch][domain][target]") {
    using namespace tgcli::daemon;

    CHECK(valid_fetch_target({.limit = 100, .all = false, .since = std::nullopt}));
    CHECK(valid_fetch_target({.limit = 7, .all = false, .since = 0}));
    CHECK(valid_fetch_target({.limit = std::nullopt, .all = false, .since = 0}));
    CHECK(valid_fetch_target({.limit = std::nullopt, .all = true, .since = std::nullopt}));
    CHECK(valid_fetch_target({.limit = std::nullopt, .all = true, .since = 0}));
    CHECK_FALSE(valid_fetch_target({.limit = std::nullopt, .all = false, .since = std::nullopt}));
    CHECK_FALSE(valid_fetch_target({.limit = 0, .all = false, .since = std::nullopt}));
    CHECK_FALSE(
        valid_fetch_target({.limit = kMaximumFetchLimit + 1, .all = false, .since = std::nullopt}));
    CHECK_FALSE(valid_fetch_target({.limit = 1, .all = true, .since = std::nullopt}));
    CHECK(finite_fetch_target({.limit = 1, .all = false, .since = std::nullopt}));
    CHECK(finite_fetch_target({.limit = std::nullopt, .all = false, .since = 0}));
    CHECK_FALSE(finite_fetch_target({.limit = std::nullopt, .all = true, .since = std::nullopt}));

    CHECK(format_fetch_timestamp(std::numeric_limits<std::int32_t>::min()) ==
          "1901-12-13T20:45:52Z");
    CHECK(format_fetch_timestamp(0) == "1970-01-01T00:00:00Z");
    CHECK(format_fetch_timestamp(std::numeric_limits<std::int32_t>::max()) ==
          "2038-01-19T03:14:07Z");
    CHECK(fetch_target_json({.limit = 7, .all = false, .since = 0}) ==
          json{{"limit", 7}, {"all", false}, {"since", "1970-01-01T00:00:00Z"}});
}

TEST_CASE("fetch page scan incorporates complete pages and accepts missing inclusive anchors",
          "[fetch][domain][scan]") {
    using namespace tgcli::daemon;

    auto scanned = scan_fetch_page({.total_count = 4,
                                    .messages = {message(-1001, 100), message(-1001, 99),
                                                 message(-1001, 98), message(-1001, 97)}},
                                   {.chat_id = -1001,
                                    .exclusive_anchor = std::nullopt,
                                    .since_cutoff_message_id = 98,
                                    .cached_count = 9});
    CHECK(scanned.error == FetchScanError::None);
    CHECK(scanned.added_count == 4);
    CHECK(scanned.oldest_message_id == 97);
    CHECK(scanned.since_anchor_observed);

    scanned =
        scan_fetch_page({.total_count = 3,
                         .messages = {message(-1001, 97), message(-1001, 96), message(-1001, 95)}},
                        {.chat_id = -1001,
                         .exclusive_anchor = 97,
                         .since_cutoff_message_id = std::nullopt,
                         .cached_count = 13});
    CHECK(scanned.error == FetchScanError::None);
    CHECK(scanned.added_count == 2);
    CHECK(scanned.oldest_message_id == 95);

    scanned =
        scan_fetch_page({.total_count = 2, .messages = {message(-1001, 94), message(-1001, 93)}},
                        {.chat_id = -1001,
                         .exclusive_anchor = 95,
                         .since_cutoff_message_id = std::nullopt,
                         .cached_count = 15});
    CHECK(scanned.error == FetchScanError::None);
    CHECK(scanned.added_count == 2);
    CHECK(scanned.oldest_message_id == 93);
}

TEST_CASE("fetch page scan distinguishes local zero progress from malformed and non-advancing data",
          "[fetch][domain][scan][error]") {
    using namespace tgcli::daemon;

    for (const auto& page :
         {tgcli::core::TdMessages{.total_count = 0, .messages = {}},
          tgcli::core::TdMessages{.total_count = 2, .messages = {std::nullopt, std::nullopt}},
          tgcli::core::TdMessages{.total_count = 1, .messages = {message(-1001, 100)}}}) {
        const auto scanned = scan_fetch_page(page, {.chat_id = -1001,
                                                    .exclusive_anchor = 100,
                                                    .since_cutoff_message_id = std::nullopt,
                                                    .cached_count = 1});
        CHECK(scanned.error == FetchScanError::None);
        CHECK(scanned.added_count == 0);
        CHECK_FALSE(scanned.oldest_message_id);
    }

    auto invalid =
        scan_fetch_page({.total_count = 2, .messages = {message(-1001, 99), std::nullopt}},
                        {.chat_id = -1001,
                         .exclusive_anchor = 100,
                         .since_cutoff_message_id = std::nullopt,
                         .cached_count = 0});
    CHECK(invalid.error == FetchScanError::Internal);

    invalid = scan_fetch_page({.total_count = 0, .messages = {message(-1001, 99)}},
                              {.chat_id = -1001,
                               .exclusive_anchor = 100,
                               .since_cutoff_message_id = std::nullopt,
                               .cached_count = 0});
    CHECK(invalid.error == FetchScanError::Internal);

    invalid = scan_fetch_page({.total_count = 1, .messages = {message(-2001, 99)}},
                              {.chat_id = -1001,
                               .exclusive_anchor = 100,
                               .since_cutoff_message_id = std::nullopt,
                               .cached_count = 0});
    CHECK(invalid.error == FetchScanError::Internal);

    invalid =
        scan_fetch_page({.total_count = 2, .messages = {message(-1001, 99), message(-1001, 99)}},
                        {.chat_id = -1001,
                         .exclusive_anchor = 100,
                         .since_cutoff_message_id = std::nullopt,
                         .cached_count = 0});
    CHECK(invalid.error == FetchScanError::NonAdvancing);

    invalid = scan_fetch_page({.total_count = 1, .messages = {message(-1001, 101)}},
                              {.chat_id = -1001,
                               .exclusive_anchor = 100,
                               .since_cutoff_message_id = std::nullopt,
                               .cached_count = 0});
    CHECK(invalid.error == FetchScanError::NonAdvancing);

    invalid = scan_fetch_page({.total_count = 1, .messages = {message(-1001, 99)}},
                              {.chat_id = -1001,
                               .exclusive_anchor = 100,
                               .since_cutoff_message_id = std::nullopt,
                               .cached_count = std::numeric_limits<std::uint64_t>::max()});
    CHECK(invalid.error == FetchScanError::Overflow);
    CHECK(invalid.added_count == 0);
}

TEST_CASE("fetch result construction enforces runtime-only latch, count, and boundary invariants",
          "[fetch][domain][runtime-only]") {
    using namespace tgcli::daemon;

    const FetchTarget limit{.limit = 2, .all = false, .since = std::nullopt};
    const FetchCompletion limit_completion{.stop_reason = FetchStopReason::TargetReached,
                                           .numeric_latched = true,
                                           .since_latched = false,
                                           .local_boundary_sealed = true,
                                           .network_fill_started = false,
                                           .terminal_page_advanced = false};
    const auto reached = make_fetch_result(-1001, 2, 99, limit, limit_completion);
    REQUIRE(reached);
    CHECK((*reached)["oldest_message_id"] == (*reached)["resume_from_message_id"]);
    CHECK((*reached)["target_reached"] == true);
    CHECK_FALSE(make_fetch_result(-1001, 0, std::nullopt, limit, limit_completion));
    CHECK_FALSE(make_fetch_result(-1001, 1, 99, limit, limit_completion));
    auto insufficient_count = *reached;
    insufficient_count["cached_count"] = 1;
    CHECK_FALSE(
        fetch_result_matches_runtime(insufficient_count, -1001, 1, 99, limit, limit_completion));
    CHECK_THAT(insufficient_count, tgcli::test::matches_json_schema("fetch.result.schema.json"));
    auto wrong_completion = limit_completion;
    wrong_completion.numeric_latched = false;
    CHECK_FALSE(make_fetch_result(-1001, 2, 99, limit, wrong_completion));
    wrong_completion = limit_completion;
    wrong_completion.since_latched = true;
    CHECK_FALSE(make_fetch_result(-1001, 2, 98, limit, wrong_completion));

    const FetchTarget since{.limit = std::nullopt, .all = false, .since = 20};
    const FetchCompletion since_completion{.stop_reason = FetchStopReason::SinceAnchorReached,
                                           .numeric_latched = false,
                                           .since_latched = true,
                                           .local_boundary_sealed = true,
                                           .network_fill_started = true,
                                           .terminal_page_advanced = true};
    const auto since_reached = make_fetch_result(-1001, 1, 99, since, since_completion);
    REQUIRE(since_reached);
    CHECK_FALSE(make_fetch_result(-1001, 0, std::nullopt, since, since_completion));
    CHECK(fetch_result_matches_runtime(*since_reached, -1001, 1, 99, since, since_completion));
    wrong_completion = since_completion;
    wrong_completion.since_latched = false;
    CHECK_FALSE(make_fetch_result(-1001, 1, 99, since, wrong_completion));
    CHECK_FALSE(
        fetch_result_matches_runtime(*since_reached, -1001, 1, 99, since, wrong_completion));
    CHECK_THAT(*since_reached, tgcli::test::matches_json_schema("fetch.result.schema.json"));

    const FetchTarget all{.limit = std::nullopt, .all = true, .since = std::nullopt};
    const FetchCompletion idle_completion{.stop_reason = FetchStopReason::TdlibIdle,
                                          .numeric_latched = false,
                                          .since_latched = false,
                                          .local_boundary_sealed = true,
                                          .network_fill_started = true,
                                          .terminal_page_advanced = false};
    const auto idle = make_fetch_result(-1001, 0, std::nullopt, all, idle_completion);
    REQUIRE(idle);
    CHECK((*idle)["target_reached"] == nullptr);
    CHECK_FALSE(make_fetch_result(-1001, 0, 99, all, idle_completion));
    CHECK_FALSE(make_fetch_result(-1001, 1, std::nullopt, all, idle_completion));

    auto mismatched = *reached;
    mismatched["resume_from_message_id"] = 98;
    CHECK_FALSE(fetch_result_matches_runtime(mismatched, -1001, 2, 99, limit, limit_completion));
    CHECK(fetch_result_matches_runtime(*reached, -1001, 2, 99, limit, limit_completion));
    wrong_completion = limit_completion;
    wrong_completion.local_boundary_sealed = false;
    CHECK_FALSE(fetch_result_matches_runtime(*reached, -1001, 2, 99, limit, wrong_completion));
    CHECK_THAT(mismatched, tgcli::test::matches_json_schema("fetch.result.schema.json"));
    CHECK_THAT(*reached, tgcli::test::matches_json_schema("fetch.result.schema.json"));
}

TEST_CASE("fetch schema asserts every structural target count and stop branch",
          "[fetch][schema][contract]") {
    const json limit_idle{{"chat_id", -1001},
                          {"cached_count", 1},
                          {"oldest_message_id", 99},
                          {"target", {{"limit", 2}, {"all", false}, {"since", nullptr}}},
                          {"target_reached", false},
                          {"stop_reason", "tdlib_idle"},
                          {"resume_from_message_id", 99}};
    CHECK_THAT(limit_idle, tgcli::test::matches_json_schema("fetch.result.schema.json"));

    const json all_idle{{"chat_id", -1001},
                        {"cached_count", 0},
                        {"oldest_message_id", nullptr},
                        {"target", {{"limit", nullptr}, {"all", true}, {"since", nullptr}}},
                        {"target_reached", nullptr},
                        {"stop_reason", "tdlib_idle"},
                        {"resume_from_message_id", nullptr}};
    CHECK_THAT(all_idle, tgcli::test::matches_json_schema("fetch.result.schema.json"));

    const json since_reached{
        {"chat_id", -1001},
        {"cached_count", 3},
        {"oldest_message_id", 97},
        {"target", {{"limit", nullptr}, {"all", false}, {"since", "1901-12-13T20:45:52Z"}}},
        {"target_reached", true},
        {"stop_reason", "since_anchor_reached"},
        {"resume_from_message_id", 97}};
    CHECK_THAT(since_reached, tgcli::test::matches_json_schema("fetch.result.schema.json"));

    auto zero_target_reached = since_reached;
    zero_target_reached["cached_count"] = 0;
    zero_target_reached["oldest_message_id"] = nullptr;
    zero_target_reached["resume_from_message_id"] = nullptr;
    zero_target_reached["target"] = {{"limit", 1}, {"all", false}, {"since", nullptr}};
    zero_target_reached["stop_reason"] = "target_reached";
    CHECK_THAT(zero_target_reached, !tgcli::test::matches_json_schema("fetch.result.schema.json"));

    auto zero_since_reached = since_reached;
    zero_since_reached["cached_count"] = 0;
    zero_since_reached["oldest_message_id"] = nullptr;
    zero_since_reached["resume_from_message_id"] = nullptr;
    CHECK_THAT(zero_since_reached, !tgcli::test::matches_json_schema("fetch.result.schema.json"));
    auto upper = since_reached;
    upper["target"]["since"] = "2038-01-19T03:14:07Z";
    CHECK_THAT(upper, tgcli::test::matches_json_schema("fetch.result.schema.json"));

    const auto rejects = [](json invalid) {
        CHECK_THAT(invalid, !tgcli::test::matches_json_schema("fetch.result.schema.json"));
    };

    auto invalid = limit_idle;
    invalid["extra"] = true;
    rejects(invalid);
    invalid = limit_idle;
    invalid["chat_id"] = 0;
    rejects(invalid);
    invalid = limit_idle;
    invalid["cached_count"] = -1;
    rejects(invalid);
    invalid = limit_idle;
    invalid["cached_count"] = 0;
    rejects(invalid);
    invalid = all_idle;
    invalid["cached_count"] = 1;
    rejects(invalid);
    invalid = limit_idle;
    invalid["oldest_message_id"] = nullptr;
    rejects(invalid);
    invalid = limit_idle;
    invalid["resume_from_message_id"] = nullptr;
    rejects(invalid);

    invalid = limit_idle;
    invalid["target"] = {{"limit", nullptr}, {"all", false}, {"since", nullptr}};
    rejects(invalid);
    invalid = limit_idle;
    invalid["target"] = {{"limit", 2}, {"all", true}, {"since", nullptr}};
    rejects(invalid);
    invalid = limit_idle;
    invalid["target"]["extra"] = true;
    rejects(invalid);
    invalid = limit_idle;
    invalid["target"]["limit"] = 1'000'001;
    rejects(invalid);

    invalid = limit_idle;
    invalid["target_reached"] = nullptr;
    rejects(invalid);
    invalid = all_idle;
    invalid["target_reached"] = false;
    rejects(invalid);
    invalid = limit_idle;
    invalid["target_reached"] = true;
    rejects(invalid);
    invalid = limit_idle;
    invalid["stop_reason"] = "target_reached";
    invalid["target_reached"] = false;
    rejects(invalid);
    invalid = since_reached;
    invalid["target"]["since"] = nullptr;
    rejects(invalid);

    for (const auto* timestamp :
         {"1901-12-13T20:45:51Z", "1902-02-29T00:00:00Z", "2000-02-30T00:00:00Z",
          "2038-01-19T03:14:08Z", "2038-01-19T03:14:07+00:00"}) {
        invalid = since_reached;
        invalid["target"]["since"] = timestamp;
        rejects(invalid);
    }
}
