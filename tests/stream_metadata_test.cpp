#include "daemon/stream_metadata.hpp"
#include "schema_matcher.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <nlohmann/json.hpp>

namespace {

using namespace tgcli::daemon;

StreamEntity user(std::int64_t id, std::vector<std::string> usernames = {}, bool is_bot = false) {
    return {.kind = StreamEntityKind::User,
            .id = id,
            .usernames = std::move(usernames),
            .is_bot = is_bot,
            .is_channel = false};
}

StreamEntity supergroup(std::int64_t id, bool is_channel, std::vector<std::string> usernames = {}) {
    return {.kind = StreamEntityKind::Supergroup,
            .id = id,
            .usernames = std::move(usernames),
            .is_bot = false,
            .is_channel = is_channel};
}

StreamChat chat(std::int64_t id, std::int64_t related_id, std::string title = "Project",
                StreamChatKind kind = StreamChatKind::Private) {
    return {.id = id,
            .title = std::move(title),
            .kind = kind,
            .related_id = related_id,
            .chat_lists = {},
            .is_marked_unread = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt};
}

MessageSummary message(std::int64_t chat_id = -2000) {
    return {.id = 123,
            .chat_id = chat_id,
            .date = "2026-08-05T10:00:00Z",
            .sender = {.kind = MessageSenderKind::User, .id = 42},
            .is_outgoing = false,
            .topic = std::nullopt,
            .type = MessageContentKind::Text,
            .text = "later"};
}

void complete_empty(StreamGenerationState& state, std::uint64_t generation,
                    std::uint64_t barrier = 1) {
    REQUIRE(state.reset(generation));
    const auto result = state.complete_bootstrap(generation, barrier, {});
    REQUIRE(result.status == StreamMetadataStatus::Accepted);
    REQUIRE(result.items.empty());
    REQUIRE(state.ready_for_admission());
}

std::vector<nlohmann::json> checked_items(const StreamMetadataResult& result) {
    REQUIRE(result.status == StreamMetadataStatus::Accepted);
    std::vector<nlohmann::json> items;
    items.reserve(result.items.size());
    for (const auto& line : result.items) {
        REQUIRE_FALSE(line.empty());
        REQUIRE(line.back() == '\n');
        auto item = nlohmann::json::parse(line.begin(), line.end() - 1);
        CHECK_THAT(item, tgcli::test::matches_json_schema("listen.item.schema.json"));
        items.push_back(std::move(item));
    }
    return items;
}

const StreamMetadataCapacityFailure& checked_capacity(const StreamMetadataResult& result,
                                                      StreamMetadataResource resource,
                                                      StreamMetadataPhase phase,
                                                      std::string_view operation = "listen") {
    REQUIRE(result.status == StreamMetadataStatus::Failed);
    REQUIRE(result.items.empty());
    REQUIRE(result.capacity);
    CHECK(result.capacity->resource == resource);
    CHECK(result.capacity->phase == phase);
    const auto details = stream_metadata_capacity_details(*result.capacity, operation);
    REQUIRE(details);
    const auto envelope = nlohmann::json{{"error",
                                          {{"code", "STREAM_CAPACITY"},
                                           {"message", "stream service capacity is unavailable"},
                                           {"details", *details}}}};
    CHECK_THAT(envelope, tgcli::test::matches_json_schema("stream.error.schema.json"));
    return *result.capacity;
}

std::vector<StreamMetadataDelta> chats(std::size_t count) {
    std::vector<StreamMetadataDelta> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto ordinal = static_cast<std::int64_t>(index + 1);
        result.emplace_back(StreamNewChatDelta{chat(-ordinal, ordinal, "")});
    }
    return result;
}

std::vector<StreamMetadataDelta> entities(std::size_t count) {
    std::vector<StreamMetadataDelta> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.emplace_back(StreamEntityDelta{user(static_cast<std::int64_t>(index + 1))});
    }
    return result;
}

StreamEvent title_event_with_line_bytes(std::size_t target) {
    const StreamEvent empty = ChatChangeEvent{TitleChatChange{.chat_id = -1001, .title = ""}};
    const auto empty_line = stream_event_line(empty);
    REQUIRE(empty_line);
    REQUIRE(empty_line->size() <= target);
    StreamEvent event = ChatChangeEvent{
        TitleChatChange{.chat_id = -1001, .title = std::string(target - empty_line->size(), 'x')}};
    const auto line = stream_event_line(event);
    REQUIRE(line);
    REQUIRE(line->size() == target);
    return event;
}

} // namespace

TEST_CASE("metadata bootstrap applies a receive barrier and publishes one snapshot",
          "[stream][metadata][bootstrap]") {
    StreamGenerationState state;
    CHECK_FALSE(state.reset(0));
    REQUIRE(state.reset(7));
    CHECK(state.bootstrapping());
    CHECK_FALSE(state.stream_ready());
    CHECK_FALSE(state.snapshot());

    CHECK(state
              .buffer_bootstrap_delta(
                  6, 1, StreamTitleDelta{.chat_id = -1001, .title = "stale generation"})
              .status == StreamMetadataStatus::StaleGeneration);
    REQUIRE(state
                .buffer_bootstrap_delta(
                    7, 1, StreamTitleDelta{.chat_id = -1001, .title = "ignored by base"})
                .status == StreamMetadataStatus::Accepted);
    REQUIRE(state.buffer_bootstrap_delta(7, 3, StreamEntityDelta{user(42, {"later"})}).status ==
            StreamMetadataStatus::Accepted);

    std::vector<StreamMetadataDelta> current_state;
    current_state.emplace_back(StreamEntityDelta{user(42, {"base"})});
    current_state.emplace_back(StreamNewChatDelta{chat(-1001, 42, "Base")});
    const auto completed = state.complete_bootstrap(7, 2, std::move(current_state));
    REQUIRE(completed.status == StreamMetadataStatus::Accepted);
    CHECK(completed.items.empty());
    CHECK(state.stream_ready());
    CHECK(state.ready_for_admission());

    const auto snapshot = state.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->generation == 7);
    REQUIRE(snapshot->chats.size() == 1);
    CHECK(snapshot->chats.front().identity.title == "Base");
    CHECK(snapshot->chats.front().identity.usernames == std::vector<std::string>{"later"});
    CHECK(state
              .buffer_bootstrap_delta(7, 4,
                                      StreamTitleDelta{.chat_id = -1001, .title = "wrong phase"})
              .status == StreamMetadataStatus::WrongPhase);
}

TEST_CASE("metadata bootstrap accepts either entity/chat order and rejects mismatched halves",
          "[stream][metadata][bootstrap]") {
    SECTION("entity before chat") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        std::vector<StreamMetadataDelta> base;
        base.emplace_back(StreamEntityDelta{supergroup(55, true, {"project"})});
        base.emplace_back(StreamNewChatDelta{chat(-1001, 55, "Project", StreamChatKind::Channel)});
        REQUIRE(state.complete_bootstrap(1, 1, base).status == StreamMetadataStatus::Accepted);
        REQUIRE(state.snapshot());
        REQUIRE(state.snapshot()->chats.size() == 1);
        CHECK(state.snapshot()->chats.front().identity.type == "channel");
    }
    SECTION("chat before entity") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        std::vector<StreamMetadataDelta> base;
        base.emplace_back(
            StreamNewChatDelta{chat(-1001, 55, "Project", StreamChatKind::Supergroup)});
        base.emplace_back(StreamEntityDelta{supergroup(55, false, {"project"})});
        REQUIRE(state.complete_bootstrap(1, 1, base).status == StreamMetadataStatus::Accepted);
        REQUIRE(state.snapshot());
        REQUIRE(state.snapshot()->chats.size() == 1);
        CHECK(state.snapshot()->chats.front().identity.type == "supergroup");
    }
    SECTION("entity before mismatched chat") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        std::vector<StreamMetadataDelta> base;
        base.emplace_back(StreamEntityDelta{supergroup(55, false)});
        base.emplace_back(StreamNewChatDelta{chat(-1001, 55, "Project", StreamChatKind::Channel)});
        CHECK(state.complete_bootstrap(1, 1, base).status == StreamMetadataStatus::Malformed);
        CHECK(state.failed());
    }
    SECTION("chat before mismatched entity") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        std::vector<StreamMetadataDelta> base;
        base.emplace_back(StreamNewChatDelta{chat(-1001, 55, "Project", StreamChatKind::Channel)});
        base.emplace_back(StreamEntityDelta{supergroup(55, false)});
        CHECK(state.complete_bootstrap(1, 1, base).status == StreamMetadataStatus::Malformed);
        CHECK(state.failed());
    }
}

TEST_CASE("chat-before-entity freezes new and retains every later candidate",
          "[stream][metadata][ordering]") {
    StreamGenerationState state;
    complete_empty(state, 1);

    auto result = state.ingest(1, 2, StreamNewChatDelta{chat(-1001, 42, "Original")});
    CHECK(result.items.empty());
    CHECK(state.ordering_barrier_open());
    CHECK_FALSE(state.ready_for_admission());
    CHECK_FALSE(state.snapshot());

    result = state.ingest(1, 3, StreamTitleDelta{.chat_id = -1001, .title = "Current title"});
    CHECK(result.items.empty());
    result = state.ingest(1, 4, StreamEvent{MessageEvent{message()}});
    CHECK(result.items.empty());

    result = state.ingest(1, 5, StreamEntityDelta{user(42, {"project"})});
    const auto items = checked_items(result);
    REQUIRE(items.size() == 4);
    CHECK(items[0].at("change") == "new");
    CHECK(items[0].at("chat").at("title") == "Original");
    CHECK(items[1].at("change") == "title");
    CHECK(items[1].at("title") == "Current title");
    CHECK(items[2].at("event") == "message");
    CHECK(items[3].at("change") == "identity");
    CHECK(items[3].at("chat").at("title") == "Current title");
    CHECK_FALSE(state.ordering_barrier_open());
    CHECK(state.ready_for_admission());
    REQUIRE(state.snapshot());
    REQUIRE(state.snapshot()->chats.size() == 1);
    CHECK(state.snapshot()->chats.front().identity.title == "Current title");
}

TEST_CASE("ordered metadata FIFO drains complete candidates only from its head",
          "[stream][metadata][ordering]") {
    StreamGenerationState state;
    complete_empty(state, 1);
    REQUIRE(state.ingest(1, 2, StreamNewChatDelta{chat(-1001, 41, "First")}).items.empty());
    REQUIRE(state.ingest(1, 3, StreamNewChatDelta{chat(-1002, 42, "Second")}).items.empty());
    REQUIRE(state.ingest(1, 4, StreamEntityDelta{user(42, {"second"})}).items.empty());

    const auto items = checked_items(state.ingest(1, 5, StreamEntityDelta{user(41, {"first"})}));
    REQUIRE(items.size() == 4);
    CHECK(items[0].at("chat").at("id") == -1001);
    CHECK(items[1].at("chat").at("id") == -1002);
    CHECK(items[2].at("change") == "identity");
    CHECK(items[2].at("chat").at("id") == -1002);
    CHECK(items[3].at("change") == "identity");
    CHECK(items[3].at("chat").at("id") == -1001);
}

TEST_CASE("entity-before-chat and derived identity changes stay closed",
          "[stream][metadata][identity]") {
    StreamGenerationState state;
    complete_empty(state, 1);
    CHECK(state.ingest(1, 2, StreamEntityDelta{user(42, {"project"})}).items.empty());

    auto items = checked_items(state.ingest(1, 3, StreamNewChatDelta{chat(-1001, 42, "Project")}));
    REQUIRE(items.size() == 1);
    CHECK(items.front().at("change") == "new");

    CHECK(state.ingest(1, 4, StreamEntityDelta{user(42, {"project"})}).items.empty());
    items = checked_items(state.ingest(1, 5, StreamEntityDelta{user(42, {"renamed"})}));
    REQUIRE(items.size() == 1);
    CHECK(items.front().at("change") == "identity");
    CHECK(items.front().at("chat").at("usernames") == nlohmann::json::array({"renamed"}));

    items = checked_items(state.ingest(1, 6, StreamTitleDelta{.chat_id = -1001, .title = "Title"}));
    REQUIRE(items.size() == 1);
    CHECK(items.front().at("change") == "title");
}

TEST_CASE("metadata failure and generation reset are fail closed", "[stream][metadata]") {
    StreamGenerationState state;
    complete_empty(state, 1);
    CHECK(state.ingest(1, 2, StreamTitleDelta{.chat_id = -999, .title = "unknown"}).status ==
          StreamMetadataStatus::Malformed);
    CHECK(state.failed());
    CHECK(state.ingest(1, 3, StreamEvent{MessageEvent{message()}}).status ==
          StreamMetadataStatus::Malformed);

    REQUIRE(state.reset(2));
    CHECK(state.bootstrapping());
    CHECK_FALSE(state.capacity_failure());
    CHECK(state.ingest(1, 4, StreamEvent{MessageEvent{message()}}).status ==
          StreamMetadataStatus::StaleGeneration);
    REQUIRE(state.complete_bootstrap(2, 1, {}).status == StreamMetadataStatus::Accepted);
    REQUIRE(state.snapshot());
    CHECK(state.snapshot()->generation == 2);
    CHECK(state.snapshot()->chats.empty());
}

TEST_CASE("bootstrap delta bounds report exact checked measurements",
          "[stream][metadata][capacity]") {
    SECTION("item count") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        for (std::size_t index = 0; index < kStreamMetadataBootstrapItems; ++index) {
            REQUIRE(state
                        .buffer_bootstrap_delta(
                            1, index + 1,
                            StreamMarkedUnreadDelta{.chat_id = -1, .is_marked_unread = false})
                        .status == StreamMetadataStatus::Accepted);
        }
        const auto result = state.buffer_bootstrap_delta(
            1, kStreamMetadataBootstrapItems + 1,
            StreamMarkedUnreadDelta{.chat_id = -1, .is_marked_unread = false});
        const auto& failure = checked_capacity(result, StreamMetadataResource::BootstrapItems,
                                               StreamMetadataPhase::Bootstrap, "wait_for");
        CHECK(failure.limit == kStreamMetadataBootstrapItems);
        CHECK(failure.used == kStreamMetadataBootstrapItems);
        CHECK(failure.incoming == 1);
    }
    SECTION("logical bytes") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        const std::size_t copied_bytes = kStreamMetadataBootstrapBytes - 64 + 1;
        const auto result = state.buffer_bootstrap_delta(
            1, 1, StreamTitleDelta{.chat_id = -1, .title = std::string(copied_bytes, 'x')});
        const auto& failure = checked_capacity(result, StreamMetadataResource::BootstrapBytes,
                                               StreamMetadataPhase::Bootstrap);
        CHECK(failure.limit == kStreamMetadataBootstrapBytes);
        CHECK(failure.would_use == kStreamMetadataBootstrapBytes + 1);
    }
}

TEST_CASE("persistent metadata map bounds fail in bootstrap and active phases",
          "[stream][metadata][capacity]") {
    SECTION("bootstrap chats") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        const auto result = state.complete_bootstrap(1, 1, chats(kStreamMetadataChats + 1));
        const auto& failure =
            checked_capacity(result, StreamMetadataResource::Chats, StreamMetadataPhase::Bootstrap);
        CHECK(failure.used == kStreamMetadataChats);
        CHECK(failure.incoming == 1);
    }
    SECTION("active chats") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        REQUIRE(state.complete_bootstrap(1, 1, chats(kStreamMetadataChats)).status ==
                StreamMetadataStatus::Accepted);
        const auto ordinal = static_cast<std::int64_t>(kStreamMetadataChats + 1);
        const auto result = state.ingest(1, 2, StreamNewChatDelta{chat(-ordinal, ordinal, "")});
        checked_capacity(result, StreamMetadataResource::Chats, StreamMetadataPhase::Active);
    }
    SECTION("bootstrap entities") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        const auto result = state.complete_bootstrap(1, 1, entities(kStreamMetadataEntities + 1));
        const auto& failure = checked_capacity(result, StreamMetadataResource::Entities,
                                               StreamMetadataPhase::Bootstrap);
        CHECK(failure.used == kStreamMetadataEntities);
        CHECK(failure.incoming == 1);
    }
    SECTION("active entities") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        REQUIRE(state.complete_bootstrap(1, 1, entities(kStreamMetadataEntities)).status ==
                StreamMetadataStatus::Accepted);
        const auto result = state.ingest(
            1, 2, StreamEntityDelta{user(static_cast<std::int64_t>(kStreamMetadataEntities + 1))});
        checked_capacity(result, StreamMetadataResource::Entities, StreamMetadataPhase::Active);
    }
}

TEST_CASE("persistent string arena reports exact bootstrap and active sums",
          "[stream][metadata][capacity]") {
    SECTION("bootstrap") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        StreamEntity oversized = user(1);
        oversized.usernames.emplace_back(kStreamMetadataBytes, 'x');
        std::vector<StreamMetadataDelta> base;
        base.emplace_back(StreamEntityDelta{std::move(oversized)});
        const auto result = state.complete_bootstrap(1, 1, std::move(base));
        const auto& failure =
            checked_capacity(result, StreamMetadataResource::Bytes, StreamMetadataPhase::Bootstrap);
        CHECK(failure.would_use == kStreamMetadataBytes + 1);
    }
    SECTION("active replacement") {
        StreamGenerationState state;
        REQUIRE(state.reset(1));
        StreamEntity base_entity = user(1);
        base_entity.usernames.emplace_back(kStreamMetadataBytes - 1, 'x');
        std::vector<StreamMetadataDelta> base;
        base.emplace_back(StreamEntityDelta{std::move(base_entity)});
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): Catch2 expansion.
        REQUIRE(state.complete_bootstrap(1, 1, std::move(base)).status ==
                StreamMetadataStatus::Accepted);

        StreamEntity replacement = user(1);
        replacement.usernames.emplace_back(kStreamMetadataBytes, 'x');
        const auto result = state.ingest(1, 2, StreamEntityDelta{std::move(replacement)});
        const auto& failure =
            checked_capacity(result, StreamMetadataResource::Bytes, StreamMetadataPhase::Active);
        CHECK(failure.would_use == kStreamMetadataBytes + 1);
    }
}

TEST_CASE("ordered FIFO item and byte limits retain exact active failures",
          "[stream][metadata][capacity][ordering]") {
    SECTION("candidate count") {
        StreamGenerationState state;
        complete_empty(state, 1);
        REQUIRE(state.ingest(1, 2, StreamNewChatDelta{chat(-1001, 42)}).items.empty());
        for (std::size_t index = 0; index < kStreamMetadataOrderItems - 1; ++index) {
            REQUIRE(state
                        .ingest(1, index + 3,
                                StreamEvent{ChatChangeEvent{MarkedUnreadChatChange{
                                    .chat_id = -2000, .is_marked_unread = false}}})
                        .items.empty());
        }
        const auto result = state.ingest(1, kStreamMetadataOrderItems + 2,
                                         StreamEvent{ChatChangeEvent{MarkedUnreadChatChange{
                                             .chat_id = -2000, .is_marked_unread = false}}});
        const auto& failure = checked_capacity(result, StreamMetadataResource::OrderItems,
                                               StreamMetadataPhase::Active);
        CHECK(failure.used == kStreamMetadataOrderItems);
        CHECK(failure.incoming == 1);
    }
    SECTION("candidate bytes") {
        StreamGenerationState state;
        complete_empty(state, 1);
        REQUIRE(state.ingest(1, 2, StreamNewChatDelta{chat(-1001, 42)}).items.empty());
        const auto maximal = title_event_with_line_bytes(kStreamMetadataItemBytes);
        constexpr std::size_t kCompleteCandidates =
            kStreamMetadataOrderBytes / kStreamMetadataItemBytes - 1;
        for (std::size_t index = 0; index < kCompleteCandidates; ++index) {
            REQUIRE(state.ingest(1, index + 3, maximal).items.empty());
        }
        const StreamEvent incoming =
            ChatChangeEvent{MarkedUnreadChatChange{.chat_id = -2000, .is_marked_unread = false}};
        const auto incoming_line = stream_event_line(incoming);
        REQUIRE(incoming_line);
        const auto result = state.ingest(1, kCompleteCandidates + 3, incoming);
        const auto& failure = checked_capacity(result, StreamMetadataResource::OrderBytes,
                                               StreamMetadataPhase::Active);
        CHECK(failure.would_use == kStreamMetadataOrderBytes + incoming_line->size());
    }
    SECTION("single complete item") {
        StreamGenerationState state;
        complete_empty(state, 1);
        const auto oversized = title_event_with_line_bytes(kStreamMetadataItemBytes + 1);
        const auto result = state.ingest(1, 2, oversized);
        const auto& failure = checked_capacity(result, StreamMetadataResource::ItemBytes,
                                               StreamMetadataPhase::Active);
        CHECK(failure.incoming == kStreamMetadataItemBytes + 1);
    }
    SECTION("completed frozen item") {
        StreamGenerationState state;
        complete_empty(state, 1);
        const ChatSummary empty_summary{.identity = {.id = -1001,
                                                     .title = "",
                                                     .type = "private",
                                                     .is_bot = false,
                                                     .usernames = {}},
                                        .is_archived = false,
                                        .folder_ids = {},
                                        .is_marked_unread = false,
                                        .unread_count = 0,
                                        .unread_mention_count = 0,
                                        .unread_reaction_count = 0,
                                        .unread_poll_vote_count = 0,
                                        .last_message = std::nullopt};
        const auto empty_line =
            stream_event_line(StreamEvent{ChatChangeEvent{NewChatChange{empty_summary}}});
        REQUIRE(empty_line);
        const auto title_bytes = kStreamMetadataItemBytes + 1 - empty_line->size();
        REQUIRE(
            state.ingest(1, 2, StreamNewChatDelta{chat(-1001, 42, std::string(title_bytes, 'x'))})
                .items.empty());
        const auto result = state.ingest(1, 3, StreamEntityDelta{user(42)});
        const auto& failure = checked_capacity(result, StreamMetadataResource::ItemBytes,
                                               StreamMetadataPhase::Active);
        CHECK(failure.incoming == kStreamMetadataItemBytes + 1);
    }
}

TEST_CASE("capacity details reject impossible public combinations",
          "[stream][metadata][capacity]") {
    const StreamMetadataCapacityFailure valid{.resource = StreamMetadataResource::BootstrapItems,
                                              .phase = StreamMetadataPhase::Bootstrap,
                                              .limit = kStreamMetadataBootstrapItems,
                                              .used = kStreamMetadataBootstrapItems,
                                              .incoming = 1,
                                              .would_use = 0};
    CHECK(stream_metadata_capacity_details(valid, "listen"));
    CHECK_FALSE(stream_metadata_capacity_details(valid, "resolve"));

    auto impossible = valid;
    impossible.used -= 1;
    CHECK_FALSE(stream_metadata_capacity_details(impossible, "listen"));
    impossible = valid;
    impossible.phase = StreamMetadataPhase::Active;
    CHECK_FALSE(stream_metadata_capacity_details(impossible, "wait_for"));
}
