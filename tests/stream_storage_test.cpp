#include "daemon/stream_model.hpp"
#include "daemon/stream_storage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using namespace tgcli;

// Fixed test sinks mirror the callback's checked bounded indexing without throwing accessors.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)

class FixedSink final : public daemon::StreamReceiveSink {
  public:
    void on_item(const daemon::StreamItemView& item,
                 const daemon::StreamMetadataView& metadata) noexcept override {
        static_cast<void>(metadata);
        if (count_ == lines_.size() || item.size() > lines_.front().size()) {
            overflow_ = true;
            return;
        }
        auto& destination = lines_[count_];
        std::size_t copied = 0;
        for (const auto bytes : item.spans()) {
            if (!bytes.empty()) {
                std::memcpy(destination.data() + copied, bytes.data(), bytes.size());
            }
            copied += bytes.size();
        }
        sizes_[count_] = copied;
        ++count_;
    }

    [[nodiscard]] std::string_view line(std::size_t index) const {
        return {lines_.at(index).data(), sizes_.at(index)};
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }

    [[nodiscard]] bool overflowed() const noexcept {
        return overflow_;
    }

  private:
    std::array<std::array<char, daemon::kStreamMetadataItemBytes>, 16> lines_{};
    std::array<std::size_t, 16> sizes_{};
    std::size_t count_ = 0;
    bool overflow_ = false;
};

class LatestSink final : public daemon::StreamReceiveSink {
  public:
    void on_item(const daemon::StreamItemView& item,
                 const daemon::StreamMetadataView& metadata) noexcept override {
        static_cast<void>(metadata);
        const auto spans = item.spans();
        first_span_size_ = spans.at(0).size();
        second_span_size_ = spans.at(1).size();
        borrowed_ = spans.at(0).data();
        borrowed_size_ = spans.at(0).size();
        std::size_t copied = 0;
        for (const auto bytes : spans) {
            if (!bytes.empty()) {
                std::memcpy(line_.data() + copied, bytes.data(), bytes.size());
                copied += bytes.size();
            }
        }
        size_ = copied;
        ++count_;
    }

    [[nodiscard]] std::string_view line() const noexcept {
        return {line_.data(), size_};
    }
    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }
    [[nodiscard]] bool used_two_spans() const noexcept {
        return first_span_size_ != 0 && second_span_size_ != 0;
    }
    [[nodiscard]] bool borrowed_item_is_poisoned() const noexcept {
        if (borrowed_ == nullptr || borrowed_size_ == 0) {
            return false;
        }
        return std::all_of(borrowed_, borrowed_ + borrowed_size_,
                           [](char byte) { return static_cast<unsigned char>(byte) == 0xA5U; });
    }

  private:
    std::array<char, daemon::kStreamMetadataItemBytes> line_{};
    const char* borrowed_ = nullptr;
    std::size_t borrowed_size_ = 0;
    std::size_t size_ = 0;
    std::size_t count_ = 0;
    std::size_t first_span_size_ = 0;
    std::size_t second_span_size_ = 0;
};

template <typename Value> core::TdValue stamped(Value value, std::uint64_t sequence) {
    auto result = core::TdValue::from(std::move(value));
    result.set_receive_event_metadata(sequence, core::TdEventClock::time_point{});
    return result;
}

core::TdUserSummary user(std::int64_t id = 42) {
    return {.id = id,
            .first_name = "Ada",
            .last_name = "Lovelace",
            .usernames = {"ada"},
            .phone_number = {},
            .is_bot = false,
            .is_premium = false,
            .presence = core::TdUserPresence::Online};
}

core::TdChat chat(std::int64_t id = -1001, std::int64_t related_id = 42,
                  core::TdChatKind kind = core::TdChatKind::Private) {
    return {.id = id,
            .title = "Project",
            .kind = kind,
            .related_id = related_id,
            .tdlib_type_id = 0,
            .positions = {},
            .chat_lists = {{.kind = core::TdChatListKind::Main, .folder_id = 0}},
            .is_marked_unread = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt,
            .notification_settings = std::nullopt};
}

core::TdMessageSummary message(std::int64_t chat_id = -1001) {
    return {.id = 123,
            .chat_id = chat_id,
            .date = 1'785'924'000,
            .sender = {.kind = core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 0},
            .is_outgoing = false,
            .topic = core::TdTopic{.kind = core::TdTopicKind::Forum, .id = 7, .tdlib_type_id = 0},
            .content_kind = core::TdMessageContentKind::Text,
            .text = "experiment result"};
}

daemon::MessageContentKind oracle_content_kind(core::TdMessageContentKind kind) {
    switch (kind) {
    case core::TdMessageContentKind::Text:
        return daemon::MessageContentKind::Text;
    case core::TdMessageContentKind::Photo:
        return daemon::MessageContentKind::Photo;
    case core::TdMessageContentKind::Video:
        return daemon::MessageContentKind::Video;
    case core::TdMessageContentKind::Document:
        return daemon::MessageContentKind::Document;
    case core::TdMessageContentKind::Voice:
        return daemon::MessageContentKind::Voice;
    case core::TdMessageContentKind::Other:
        return daemon::MessageContentKind::Other;
    }
    return daemon::MessageContentKind::Other;
}

void check_oracle(const LatestSink& sink, const daemon::StreamEvent& event) {
    const auto expected = daemon::stream_event_line(event);
    REQUIRE(expected);
    REQUIRE_FALSE(sink.line().empty());
    CHECK(sink.line().back() == '\n');
    CHECK(nlohmann::json::parse(sink.line()) == nlohmann::json::parse(*expected));
}

void bootstrap(daemon::FixedStreamNormalizer& normalizer) {
    REQUIRE(normalizer.begin(7, 9));
    core::TdCurrentState state;
    state.updates.push_back(core::TdValue::from(core::TdUpdateUser{.user = user()}));
    state.updates.push_back(core::TdValue::from(core::TdUpdateNewChat{.chat = chat()}));
    auto response = stamped(std::move(state), 10);
    normalizer.on_current_state(7, 9, response);
    REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
}

void bootstrap_empty(daemon::FixedStreamNormalizer& normalizer, std::uint64_t barrier = 1) {
    REQUIRE(normalizer.begin(1, 1));
    auto response = stamped(core::TdCurrentState{}, barrier);
    normalizer.on_current_state(1, 1, response);
    REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
}

struct BlockingStatusProbe {
    daemon::detail::StreamStatusPublishPoint target =
        daemon::detail::StreamStatusPublishPoint::FailurePayload;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    static void notify(void* context, daemon::detail::StreamStatusPublishPoint point) noexcept {
        auto& probe = *static_cast<BlockingStatusProbe*>(context);
        if (point != probe.target) {
            return;
        }
        probe.entered.store(true, std::memory_order_release);
        while (!probe.release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

void wait_entered(const BlockingStatusProbe& probe) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!probe.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(probe.entered.load(std::memory_order_acquire));
}

} // namespace

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

static_assert(!std::is_constructible_v<daemon::StreamMetadataView, const void*>);
static_assert(!std::is_copy_constructible_v<daemon::StreamMetadataView>);
static_assert(!std::is_move_constructible_v<daemon::StreamMetadataView>);
static_assert(!std::is_copy_constructible_v<daemon::StreamMetadataCursor>);
static_assert(!std::is_move_constructible_v<daemon::StreamMetadataCursor>);
static_assert(!std::is_copy_constructible_v<daemon::StreamItemView>);
static_assert(!std::is_move_constructible_v<daemon::StreamItemView>);

TEST_CASE("fixed stream writer escapes JSON and renders UTC without locale",
          "[stream][storage][writer]") {
    std::array<char, 20> timestamp{};
    REQUIRE(daemon::stream_timestamp_utc(1, timestamp));
    CHECK(std::string_view(timestamp.data(), timestamp.size()) == "1970-01-01T00:00:01Z");
    REQUIRE(daemon::stream_timestamp_utc(2'000'000'000, timestamp));
    CHECK(std::string_view(timestamp.data(), timestamp.size()) == "2033-05-18T03:33:20Z");
    CHECK_FALSE(daemon::stream_timestamp_utc(0, timestamp));

    std::array<char, 32> escaped{};
    const auto exact = daemon::stream_json_escape("a\n\"\\\x01\xF0\x9F\xA7\xAA", escaped);
    REQUIRE(exact.valid);
    CHECK(exact.required_bytes == 17);
    CHECK(std::string_view(escaped.data(), exact.written_bytes) ==
          "a\\n\\\"\\\\\\u0001\xF0\x9F\xA7\xAA");

    std::array<char, 3> short_buffer{};
    const auto overflow = daemon::stream_json_escape("\n\n", short_buffer);
    CHECK(overflow.valid);
    CHECK(overflow.required_bytes == 4);
    CHECK(overflow.written_bytes == short_buffer.size());
}

TEST_CASE("fixed stream normalizer bootstraps and emits compact borrowed lines",
          "[stream][storage][normalize]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);

    auto update = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "New title"}, 11);
    normalizer.on_update(7, 9, update);

    REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
    REQUIRE(sink.count() == 1);
    CHECK_FALSE(sink.overflowed());
    CHECK(sink.line(0) == "{\"event\":\"chat_change\",\"change\":\"title\",\"chat_id\":-1001,"
                          "\"title\":\"New title\"}\n");
}

TEST_CASE("fixed stream borrowed items are split and poisoned after the sink returns",
          "[stream][storage][borrowed]") {
    LatestSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);
    auto update = stamped(
        core::TdUpdateChatTitle{.chat_id = -1001, .title = "two-span \xF0\x9F\xA7\xAA"}, 11);
    normalizer.on_update(7, 9, update);
    REQUIRE(sink.count() == 1);
    CHECK(sink.used_two_spans());
    CHECK(sink.borrowed_item_is_poisoned());
}

TEST_CASE("fixed writer matches the heap oracle for content topic sender and scalar edges",
          "[stream][storage][writer][differential]") {
    LatestSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);
    constexpr std::array content_kinds{
        core::TdMessageContentKind::Text,  core::TdMessageContentKind::Photo,
        core::TdMessageContentKind::Video, core::TdMessageContentKind::Document,
        core::TdMessageContentKind::Voice, core::TdMessageContentKind::Other};
    constexpr std::array topic_kinds{core::TdTopicKind::Forum, core::TdTopicKind::Thread,
                                     core::TdTopicKind::Direct, core::TdTopicKind::Saved};
    std::uint64_t sequence = 11;
    for (std::size_t index = 0; index < content_kinds.size(); ++index) {
        auto value = message();
        value.id = 9'007'199'254'740'991LL - static_cast<std::int64_t>(index);
        value.date = 1'785'924'000;
        if (index == 0) {
            value.date = 1;
        } else if (index + 1 == content_kinds.size()) {
            value.date = std::numeric_limits<std::int32_t>::max();
        }
        value.content_kind = content_kinds.at(index);
        value.text = index == 0 ? "control:\n\t\x01 UTF-8:\xF0\x9F\xA7\xAA"
                                : "variant-" + std::to_string(index);
        if (index == 0) {
            value.topic = std::nullopt;
        } else {
            const auto topic_kind = topic_kinds.at((index - 1) % topic_kinds.size());
            value.topic = core::TdTopic{.kind = topic_kind,
                                        .id = topic_kind == core::TdTopicKind::Forum
                                                  ? 2'147'483'647
                                                  : 9'007'199'254'740'991LL,
                                        .tdlib_type_id = 0};
        }
        if (index == 1) {
            value.sender = {.kind = core::TdMessageSenderKind::Chat,
                            .id = -9'007'199'254'740'991LL,
                            .tdlib_type_id = 0};
        }
        const auto oracle = daemon::materialize_message_summary(value);
        REQUIRE(oracle);
        auto update = stamped(core::TdUpdateNewMessage{.message = std::move(value)}, sequence++);
        normalizer.on_update(7, 9, update);
        check_oracle(sink, daemon::MessageEvent{.message = *oracle});
    }

    for (const auto kind : content_kinds) {
        const std::string text = "edit:\n\x01\xF0\x9F\xA7\xAA";
        auto update = stamped(
            core::TdUpdateMessageContent{
                .chat_id = -1001,
                .message_id = 9'007'199'254'740'991LL,
                .content = {.kind = kind, .text = text, .tdlib_type_id = 0}},
            sequence++);
        normalizer.on_update(7, 9, update);
        check_oracle(sink, daemon::EditContentEvent{
                               .chat_id = -1001,
                               .message_id = 9'007'199'254'740'991LL,
                               .content = {.type = oracle_content_kind(kind), .text = text}});
    }

    for (const auto date : {std::int32_t{0}, std::numeric_limits<std::int32_t>::max()}) {
        auto update = stamped(core::TdUpdateMessageEdited{.chat_id = -1001,
                                                          .message_id = 123,
                                                          .edit_date = date,
                                                          .has_reply_markup = date != 0},
                              sequence++);
        normalizer.on_update(7, 9, update);
        auto source = message();
        source.date = date;
        const auto rendered = daemon::materialize_message_summary(source);
        REQUIRE(rendered);
        check_oracle(sink, daemon::EditMetadataEvent{.chat_id = -1001,
                                                     .message_id = 123,
                                                     .edit_date = rendered->date,
                                                     .has_reply_markup = date != 0});
    }
}

TEST_CASE("fixed stream normalizer rejects forged chat-list overflow",
          "[stream][storage][malformed]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    REQUIRE(normalizer.begin(1, 1));
    auto oversized = chat();
    oversized.chat_lists.assign(daemon::kStreamRawChatLists + 1,
                                {.kind = core::TdChatListKind::Main, .folder_id = 0});
    core::TdCurrentState state;
    state.updates.push_back(core::TdValue::from(core::TdUpdateNewChat{.chat = oversized}));
    auto response = stamped(std::move(state), 1);
    normalizer.on_current_state(1, 1, response);
    const auto status = normalizer.status();
    CHECK(status.phase == daemon::StreamNormalizationPhase::Failed);
    CHECK(status.failure.kind == daemon::StreamFailureKind::MalformedSupported);
}

TEST_CASE("fixed stream normalizer covers the closed message and reaction families",
          "[stream][storage][normalize]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);
    std::uint64_t sequence = 11;

    auto new_message = stamped(core::TdUpdateNewMessage{.message = message()}, sequence++);
    normalizer.on_update(7, 9, new_message);
    auto item = nlohmann::json::parse(sink.line(0));
    CHECK(item.at("event") == "message");
    CHECK(item.at("message").at("text") == "experiment result");

    auto content =
        stamped(core::TdUpdateMessageContent{.chat_id = -1001,
                                             .message_id = 123,
                                             .content = {.kind = core::TdMessageContentKind::Photo,
                                                         .text = "replacement",
                                                         .tdlib_type_id = 0}},
                sequence++);
    normalizer.on_update(7, 9, content);
    item = nlohmann::json::parse(sink.line(1));
    CHECK(item.at("event") == "edit_content");
    CHECK(item.at("content").at("type") == "photo");

    auto edited = stamped(core::TdUpdateMessageEdited{.chat_id = -1001,
                                                      .message_id = 123,
                                                      .edit_date = 1'785'924'000,
                                                      .has_reply_markup = true},
                          sequence++);
    normalizer.on_update(7, 9, edited);
    item = nlohmann::json::parse(sink.line(2));
    CHECK(item.at("event") == "edit_metadata");
    CHECK(item.at("edit_date") == "2026-08-05T10:00:00Z");

    core::TdReactionSnapshot snapshot{
        .items = {{.reaction = {.kind = core::TdReactionKind::Emoji,
                                .emoji = "🧪",
                                .custom_emoji_id = 0,
                                .tdlib_type_id = 0},
                   .total_count = 3,
                   .is_chosen = true,
                   .used_sender = core::TdMessageSender{.kind = core::TdMessageSenderKind::User,
                                                        .id = 42,
                                                        .tdlib_type_id = 0},
                   .recent_senders = {{.kind = core::TdMessageSenderKind::Chat,
                                       .id = -1001,
                                       .tdlib_type_id = 0}}}},
        .are_tags = false,
        .can_get_added_reactions = true};
    auto interaction = stamped(
        core::TdUpdateMessageInteractionInfo{
            .chat_id = -1001, .message_id = 123, .reactions = std::move(snapshot)},
        sequence++);
    normalizer.on_update(7, 9, interaction);
    item = nlohmann::json::parse(sink.line(3));
    CHECK(item.at("event") == "reaction_snapshot");
    CHECK(item.at("reactions").at("items").at(0).at("total_count") == 3);

    auto bot_delta = stamped(
        core::TdUpdateMessageReaction{
            .chat_id = -1001,
            .message_id = 123,
            .actor = {.kind = core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 0},
            .date = 1'785'924'000,
            .old_reactions = {{.kind = core::TdReactionKind::Paid,
                               .emoji = {},
                               .custom_emoji_id = 0,
                               .tdlib_type_id = 0}},
            .new_reactions = {{.kind = core::TdReactionKind::CustomEmoji,
                               .emoji = {},
                               .custom_emoji_id = 123456789,
                               .tdlib_type_id = 0}}},
        sequence++);
    normalizer.on_update(7, 9, bot_delta);
    item = nlohmann::json::parse(sink.line(4));
    CHECK(item.at("event") == "bot_reaction_change");
    CHECK(item.at("new_reactions").at(0).at("custom_emoji_id") == "123456789");

    auto bot_snapshot = stamped(
        core::TdUpdateMessageReactions{
            .chat_id = -1001,
            .message_id = 123,
            .date = 1'785'924'000,
            .reactions = {{.reaction = {.kind = core::TdReactionKind::Emoji,
                                        .emoji = "🧪",
                                        .custom_emoji_id = 0,
                                        .tdlib_type_id = 0},
                           .total_count = 2}}},
        sequence++);
    normalizer.on_update(7, 9, bot_snapshot);
    item = nlohmann::json::parse(sink.line(5));
    CHECK(item.at("event") == "bot_reaction_snapshot");
    CHECK(item.at("reactions").at(0).at("total_count") == 2);

    auto deletion = stamped(core::TdUpdateDeleteMessages{.client_generation = 9,
                                                         .chat_id = -1001,
                                                         .message_ids = {123, 124},
                                                         .is_permanent = true,
                                                         .from_cache = false},
                            sequence++);
    normalizer.on_update(7, 9, deletion);
    item = nlohmann::json::parse(sink.line(6));
    CHECK(item.at("event") == "delete_batch");
    CHECK(item.at("message_ids").size() == 2);
    CHECK(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
}

TEST_CASE("fixed writer matches the heap oracle for reaction and deletion variants",
          "[stream][storage][writer][differential]") {
    LatestSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);
    std::uint64_t sequence = 11;

    auto null_snapshot = stamped(core::TdUpdateMessageInteractionInfo{.chat_id = -1001,
                                                                      .message_id = 123,
                                                                      .reactions = std::nullopt},
                                 sequence++);
    normalizer.on_update(7, 9, null_snapshot);
    check_oracle(sink, daemon::ReactionSnapshotEvent{
                           .chat_id = -1001, .message_id = 123, .reactions = std::nullopt});

    core::TdReactionSnapshot snapshot{
        .items = {{.reaction = {.kind = core::TdReactionKind::Emoji,
                                .emoji = "\xF0\x9F\xA7\xAA",
                                .custom_emoji_id = 0,
                                .tdlib_type_id = 0},
                   .total_count = 0,
                   .is_chosen = true,
                   .used_sender = core::TdMessageSender{.kind = core::TdMessageSenderKind::User,
                                                        .id = 9'007'199'254'740'991LL,
                                                        .tdlib_type_id = 0},
                   .recent_senders = {{.kind = core::TdMessageSenderKind::Chat,
                                       .id = -9'007'199'254'740'991LL,
                                       .tdlib_type_id = 0}}},
                  {.reaction = {.kind = core::TdReactionKind::CustomEmoji,
                                .emoji = {},
                                .custom_emoji_id = 9'007'199'254'740'991LL,
                                .tdlib_type_id = 0},
                   .total_count = std::numeric_limits<std::int32_t>::max(),
                   .is_chosen = false,
                   .used_sender = std::nullopt,
                   .recent_senders = {}},
                  {.reaction = {.kind = core::TdReactionKind::Paid,
                                .emoji = {},
                                .custom_emoji_id = 0,
                                .tdlib_type_id = 0},
                   .total_count = 1,
                   .is_chosen = false,
                   .used_sender = std::nullopt,
                   .recent_senders = {}}},
        .are_tags = true,
        .can_get_added_reactions = false};
    auto full_snapshot = stamped(
        core::TdUpdateMessageInteractionInfo{
            .chat_id = -1001, .message_id = 123, .reactions = std::move(snapshot)},
        sequence++);
    normalizer.on_update(7, 9, full_snapshot);
    daemon::ReactionSnapshot oracle_snapshot{
        .items =
            {{.reaction = {.kind = daemon::ReactionKind::Emoji,
                           .emoji = "\xF0\x9F\xA7\xAA",
                           .custom_emoji_id = 0},
              .total_count = 0,
              .is_chosen = true,
              .used_sender = daemon::MessageSenderRef{.kind = daemon::MessageSenderKind::User,
                                                      .id = 9'007'199'254'740'991LL},
              .recent_senders = {{.kind = daemon::MessageSenderKind::Chat,
                                  .id = -9'007'199'254'740'991LL}}},
             {.reaction = {.kind = daemon::ReactionKind::CustomEmoji,
                           .emoji = {},
                           .custom_emoji_id = 9'007'199'254'740'991LL},
              .total_count = std::numeric_limits<std::int32_t>::max(),
              .is_chosen = false,
              .used_sender = std::nullopt,
              .recent_senders = {}},
             {.reaction = {.kind = daemon::ReactionKind::Paid, .emoji = {}, .custom_emoji_id = 0},
              .total_count = 1,
              .is_chosen = false,
              .used_sender = std::nullopt,
              .recent_senders = {}}},
        .are_tags = true,
        .can_get_added_reactions = false};
    check_oracle(sink, daemon::ReactionSnapshotEvent{
                           .chat_id = -1001, .message_id = 123, .reactions = oracle_snapshot});

    auto bot_delta = stamped(
        core::TdUpdateMessageReaction{.chat_id = -1001,
                                      .message_id = 123,
                                      .actor = {.kind = core::TdMessageSenderKind::Chat,
                                                .id = -9'007'199'254'740'991LL,
                                                .tdlib_type_id = 0},
                                      .date = std::numeric_limits<std::int32_t>::max(),
                                      .old_reactions = {{.kind = core::TdReactionKind::Emoji,
                                                         .emoji = "\xF0\x9F\x91\x8D",
                                                         .custom_emoji_id = 0,
                                                         .tdlib_type_id = 0},
                                                        {.kind = core::TdReactionKind::Paid,
                                                         .emoji = {},
                                                         .custom_emoji_id = 0,
                                                         .tdlib_type_id = 0}},
                                      .new_reactions = {{.kind = core::TdReactionKind::CustomEmoji,
                                                         .emoji = {},
                                                         .custom_emoji_id = 9'007'199'254'740'991LL,
                                                         .tdlib_type_id = 0}}},
        sequence++);
    normalizer.on_update(7, 9, bot_delta);
    auto date_source = message();
    date_source.date = std::numeric_limits<std::int32_t>::max();
    const auto date_oracle = daemon::materialize_message_summary(date_source);
    REQUIRE(date_oracle);
    REQUIRE(date_oracle->date);
    check_oracle(sink, daemon::BotReactionChangeEvent{
                           .chat_id = -1001,
                           .message_id = 123,
                           .actor = {.kind = daemon::MessageSenderKind::Chat,
                                     .id = -9'007'199'254'740'991LL},
                           .date = *date_oracle->date,
                           .old_reactions = {{.kind = daemon::ReactionKind::Emoji,
                                              .emoji = "\xF0\x9F\x91\x8D",
                                              .custom_emoji_id = 0},
                                             {.kind = daemon::ReactionKind::Paid,
                                              .emoji = {},
                                              .custom_emoji_id = 0}},
                           .new_reactions = {{.kind = daemon::ReactionKind::CustomEmoji,
                                              .emoji = {},
                                              .custom_emoji_id = 9'007'199'254'740'991LL}}});

    auto anonymous = stamped(
        core::TdUpdateMessageReactions{
            .chat_id = -1001,
            .message_id = 123,
            .date = 1,
            .reactions = {{.reaction = {.kind = core::TdReactionKind::Emoji,
                                        .emoji = "\xF0\x9F\xA7\xAA",
                                        .custom_emoji_id = 0,
                                        .tdlib_type_id = 0},
                           .total_count = 0},
                          {.reaction = {.kind = core::TdReactionKind::CustomEmoji,
                                        .emoji = {},
                                        .custom_emoji_id = 42,
                                        .tdlib_type_id = 0},
                           .total_count = 2},
                          {.reaction = {.kind = core::TdReactionKind::Paid,
                                        .emoji = {},
                                        .custom_emoji_id = 0,
                                        .tdlib_type_id = 0},
                           .total_count = std::numeric_limits<std::int32_t>::max()}}},
        sequence++);
    normalizer.on_update(7, 9, anonymous);
    date_source.date = 1;
    const auto first_date = daemon::materialize_message_summary(date_source);
    REQUIRE(first_date);
    REQUIRE(first_date->date);
    check_oracle(
        sink,
        daemon::BotReactionSnapshotEvent{
            .chat_id = -1001,
            .message_id = 123,
            .date = *first_date->date,
            .reactions = {
                {{.kind = daemon::ReactionKind::Emoji,
                  .emoji = "\xF0\x9F\xA7\xAA",
                  .custom_emoji_id = 0},
                 0},
                {{.kind = daemon::ReactionKind::CustomEmoji, .emoji = {}, .custom_emoji_id = 42},
                 2},
                {{.kind = daemon::ReactionKind::Paid, .emoji = {}, .custom_emoji_id = 0},
                 std::numeric_limits<std::int32_t>::max()}}});

    auto deletion = stamped(core::TdUpdateDeleteMessages{.client_generation = 9,
                                                         .chat_id = -1001,
                                                         .message_ids = {-9'007'199'254'740'991LL,
                                                                         9'007'199'254'740'991LL},
                                                         .is_permanent = true,
                                                         .from_cache = true},
                            sequence++);
    normalizer.on_update(7, 9, deletion);
    check_oracle(sink, daemon::DeleteBatchEvent{
                           .chat_id = -1001,
                           .message_ids = {-9'007'199'254'740'991LL, 9'007'199'254'740'991LL},
                           .is_permanent = true,
                           .from_cache = true});
}

TEST_CASE("fixed stream normalizer covers every chat delta source and same values",
          "[stream][storage][normalize]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);
    std::uint64_t sequence = 11;

    auto last = stamped(core::TdUpdateChatLastMessage{.chat_id = -1001, .last_message = message()},
                        sequence++);
    normalizer.on_update(7, 9, last);
    auto item = nlohmann::json::parse(sink.line(0));
    CHECK(item.at("change") == "last_message");

    auto added = stamped(
        core::TdUpdateChatAddedToList{
            .chat_id = -1001, .list = {.kind = core::TdChatListKind::Folder, .folder_id = 2}},
        sequence++);
    normalizer.on_update(7, 9, added);
    CHECK(nlohmann::json::parse(sink.line(1)).at("change") == "list_added");
    auto removed = stamped(
        core::TdUpdateChatRemovedFromList{
            .chat_id = -1001, .list = {.kind = core::TdChatListKind::Folder, .folder_id = 2}},
        sequence++);
    normalizer.on_update(7, 9, removed);
    CHECK(nlohmann::json::parse(sink.line(2)).at("change") == "list_removed");

    auto inbox = stamped(core::TdUpdateChatReadInbox{.chat_id = -1001,
                                                     .last_read_inbox_message_id = 123,
                                                     .unread_count = 2},
                         sequence++);
    normalizer.on_update(7, 9, inbox);
    CHECK(nlohmann::json::parse(sink.line(3)).at("change") == "read_inbox");

    auto mention_message = stamped(core::TdUpdateMessageMentionRead{.chat_id = -1001,
                                                                    .message_id = 123,
                                                                    .unread_mention_count = 1},
                                   sequence++);
    normalizer.on_update(7, 9, mention_message);
    auto mention_chat =
        stamped(core::TdUpdateChatUnreadMentionCount{.chat_id = -1001, .unread_mention_count = 1},
                sequence++);
    normalizer.on_update(7, 9, mention_chat);
    CHECK(nlohmann::json::parse(sink.line(4)).at("unread_mention_count") == 1);
    CHECK(nlohmann::json::parse(sink.line(5)).at("unread_mention_count") == 1);

    auto reaction_message = stamped(
        core::TdUpdateMessageUnreadReactions{
            .chat_id = -1001, .message_id = 123, .unread_reaction_count = 2},
        sequence++);
    normalizer.on_update(7, 9, reaction_message);
    auto reaction_chat =
        stamped(core::TdUpdateChatUnreadReactionCount{.chat_id = -1001, .unread_reaction_count = 2},
                sequence++);
    normalizer.on_update(7, 9, reaction_chat);
    CHECK(nlohmann::json::parse(sink.line(6)).at("unread_reaction_count") == 2);
    CHECK(nlohmann::json::parse(sink.line(7)).at("unread_reaction_count") == 2);

    auto poll_message =
        stamped(core::TdUpdateMessageContainsUnreadPollVotes{.chat_id = -1001,
                                                             .message_id = 123,
                                                             .contains_unread_poll_votes = true,
                                                             .unread_poll_vote_count = 3},
                sequence++);
    normalizer.on_update(7, 9, poll_message);
    auto poll_chat = stamped(
        core::TdUpdateChatUnreadPollVoteCount{.chat_id = -1001, .unread_poll_vote_count = 3},
        sequence++);
    normalizer.on_update(7, 9, poll_chat);
    CHECK(nlohmann::json::parse(sink.line(8)).at("unread_poll_vote_count") == 3);
    CHECK(nlohmann::json::parse(sink.line(9)).at("unread_poll_vote_count") == 3);

    auto marked = stamped(
        core::TdUpdateChatIsMarkedAsUnread{.chat_id = -1001, .is_marked_unread = true}, sequence++);
    normalizer.on_update(7, 9, marked);
    CHECK(nlohmann::json::parse(sink.line(10)).at("change") == "marked_unread");
    CHECK(sink.count() == 11);
}

TEST_CASE("fixed writer matches the heap oracle for every chat change and list variant",
          "[stream][storage][writer][differential]") {
    LatestSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);
    std::uint64_t sequence = 11;

    auto newcomer = user(43);
    newcomer.usernames = {"new", "second"};
    auto entity = stamped(core::TdUpdateUser{.user = newcomer}, sequence++);
    normalizer.on_update(7, 9, entity);
    auto new_chat = chat(-2002, 43, core::TdChatKind::Private);
    new_chat.title = "New\n\x01\xF0\x9F\xA7\xAA";
    new_chat.chat_lists = {{.kind = core::TdChatListKind::Folder, .folder_id = 9},
                           {.kind = core::TdChatListKind::Archive, .folder_id = 0},
                           {.kind = core::TdChatListKind::Main, .folder_id = 0},
                           {.kind = core::TdChatListKind::Folder, .folder_id = 2}};
    new_chat.is_marked_unread = true;
    new_chat.unread_count = 1;
    new_chat.unread_mention_count = 2;
    new_chat.unread_reaction_count = 3;
    new_chat.unread_poll_vote_count = 4;
    auto last = message(-2002);
    last.topic = std::nullopt;
    new_chat.last_message = last;
    auto update_new = stamped(core::TdUpdateNewChat{.chat = std::move(new_chat)}, sequence++);
    normalizer.on_update(7, 9, update_new);
    const auto last_oracle = daemon::materialize_message_summary(last);
    REQUIRE(last_oracle);
    check_oracle(sink, daemon::ChatChangeEvent{daemon::NewChatChange{
                           .chat = {.identity = {.id = -2002,
                                                 .title = "New\n\x01\xF0\x9F\xA7\xAA",
                                                 .type = "private",
                                                 .is_bot = false,
                                                 .usernames = {"new", "second"}},
                                    .is_archived = true,
                                    .folder_ids = {2, 9},
                                    .is_marked_unread = true,
                                    .unread_count = 1,
                                    .unread_mention_count = 2,
                                    .unread_reaction_count = 3,
                                    .unread_poll_vote_count = 4,
                                    .last_message = last_oracle}}});

    newcomer.usernames = {"changed"};
    auto identity = stamped(core::TdUpdateUser{.user = std::move(newcomer)}, sequence++);
    normalizer.on_update(7, 9, identity);
    check_oracle(sink, daemon::ChatChangeEvent{
                           daemon::IdentityChatChange{.chat = {.id = -2002,
                                                               .title = "New\n\x01\xF0\x9F\xA7\xAA",
                                                               .type = "private",
                                                               .is_bot = false,
                                                               .usernames = {"changed"}}}});

    const std::string title = "Title\n\t\x01\xF0\x9F\xA7\xAA";
    auto title_update =
        stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = title}, sequence++);
    normalizer.on_update(7, 9, title_update);
    check_oracle(
        sink, daemon::ChatChangeEvent{daemon::TitleChatChange{.chat_id = -1001, .title = title}});

    for (const bool present : {false, true}) {
        auto last_message = present ? std::optional(message()) : std::nullopt;
        const auto expected = last_message ? daemon::materialize_message_summary(*last_message)
                                           : std::optional<daemon::MessageSummary>{};
        auto update =
            stamped(core::TdUpdateChatLastMessage{.chat_id = -1001, .last_message = last_message},
                    sequence++);
        normalizer.on_update(7, 9, update);
        check_oracle(sink, daemon::ChatChangeEvent{daemon::LastMessageChatChange{
                               .chat_id = -1001, .last_message = expected}});
    }

    struct ListCase {
        core::TdChatList source;
        daemon::ChatListRef oracle;
    };
    const std::array lists{
        ListCase{{.kind = core::TdChatListKind::Main, .folder_id = 0},
                 {.kind = daemon::ChatListKind::Main, .folder_id = 0}},
        ListCase{{.kind = core::TdChatListKind::Archive, .folder_id = 0},
                 {.kind = daemon::ChatListKind::Archive, .folder_id = 0}},
        ListCase{{.kind = core::TdChatListKind::Folder, .folder_id = 2'147'483'647},
                 {.kind = daemon::ChatListKind::Folder, .folder_id = 2'147'483'647}},
    };
    for (const auto& list : lists) {
        auto added = stamped(core::TdUpdateChatAddedToList{.chat_id = -1001, .list = list.source},
                             sequence++);
        normalizer.on_update(7, 9, added);
        check_oracle(sink, daemon::ChatChangeEvent{
                               daemon::ListAddedChatChange{.chat_id = -1001, .list = list.oracle}});
        auto removed = stamped(
            core::TdUpdateChatRemovedFromList{.chat_id = -1001, .list = list.source}, sequence++);
        normalizer.on_update(7, 9, removed);
        check_oracle(sink, daemon::ChatChangeEvent{daemon::ListRemovedChatChange{
                               .chat_id = -1001, .list = list.oracle}});
    }

    auto inbox = stamped(
        core::TdUpdateChatReadInbox{.chat_id = -1001,
                                    .last_read_inbox_message_id = 9'007'199'254'740'991LL,
                                    .unread_count = std::numeric_limits<std::int32_t>::max()},
        sequence++);
    normalizer.on_update(7, 9, inbox);
    check_oracle(sink, daemon::ChatChangeEvent{daemon::ReadInboxChatChange{
                           .chat_id = -1001,
                           .last_read_inbox_message_id = 9'007'199'254'740'991LL,
                           .unread_count = std::numeric_limits<std::int32_t>::max()}});

    auto mention = stamped(
        core::TdUpdateChatUnreadMentionCount{
            .chat_id = -1001, .unread_mention_count = std::numeric_limits<std::int32_t>::max()},
        sequence++);
    normalizer.on_update(7, 9, mention);
    check_oracle(sink, daemon::ChatChangeEvent{daemon::UnreadMentionChatChange{
                           .chat_id = -1001,
                           .unread_mention_count = std::numeric_limits<std::int32_t>::max()}});
    auto reaction = stamped(
        core::TdUpdateChatUnreadReactionCount{
            .chat_id = -1001, .unread_reaction_count = std::numeric_limits<std::int32_t>::max()},
        sequence++);
    normalizer.on_update(7, 9, reaction);
    check_oracle(sink, daemon::ChatChangeEvent{daemon::UnreadReactionChatChange{
                           .chat_id = -1001,
                           .unread_reaction_count = std::numeric_limits<std::int32_t>::max()}});
    auto poll = stamped(
        core::TdUpdateChatUnreadPollVoteCount{
            .chat_id = -1001, .unread_poll_vote_count = std::numeric_limits<std::int32_t>::max()},
        sequence++);
    normalizer.on_update(7, 9, poll);
    check_oracle(sink, daemon::ChatChangeEvent{daemon::UnreadPollVoteChatChange{
                           .chat_id = -1001,
                           .unread_poll_vote_count = std::numeric_limits<std::int32_t>::max()}});
    auto marked = stamped(
        core::TdUpdateChatIsMarkedAsUnread{.chat_id = -1001, .is_marked_unread = true}, sequence++);
    normalizer.on_update(7, 9, marked);
    check_oracle(sink, daemon::ChatChangeEvent{daemon::MarkedUnreadChatChange{
                           .chat_id = -1001, .is_marked_unread = true}});
}

TEST_CASE("incomplete new chat freezes bytes and preserves global FIFO",
          "[stream][storage][ordering]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap(normalizer);

    auto missing = chat(-2000, 55, core::TdChatKind::Supergroup);
    missing.title = "Frozen";
    auto new_chat = stamped(core::TdUpdateNewChat{.chat = std::move(missing)}, 11);
    normalizer.on_update(7, 9, new_chat);
    auto later = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "Later"}, 12);
    normalizer.on_update(7, 9, later);
    CHECK(sink.count() == 0);

    core::TdSupergroup group{
        .id = 55, .usernames = {"frozen"}, .is_channel = false, .is_forum = false};
    auto entity = stamped(core::TdUpdateSupergroup{.supergroup = std::move(group)}, 13);
    normalizer.on_update(7, 9, entity);
    REQUIRE(sink.count() == 3);
    CHECK(nlohmann::json::parse(sink.line(0)).at("change") == "new");
    CHECK(nlohmann::json::parse(sink.line(0)).at("chat").at("title") == "Frozen");
    CHECK(nlohmann::json::parse(sink.line(1)).at("change") == "title");
    CHECK(nlohmann::json::parse(sink.line(2)).at("change") == "identity");
    CHECK(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
}

TEST_CASE("secret and unsupported updates are suppressed while malformed is retained",
          "[stream][storage][malformed]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    REQUIRE(normalizer.begin(1, 1));
    core::TdCurrentState state;
    state.updates.push_back(core::TdValue::from(
        core::TdUpdateNewChat{.chat = chat(-2000, 42, core::TdChatKind::Secret)}));
    auto response = stamped(std::move(state), 1);
    normalizer.on_current_state(1, 1, response);
    REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);

    auto secret_message = stamped(core::TdUpdateNewMessage{.message = message(-2000)}, 2);
    normalizer.on_update(1, 1, secret_message);
    auto unsupported = stamped(core::TdOk{}, 3);
    normalizer.on_update(1, 1, unsupported);
    CHECK(sink.count() == 0);

    auto malformed = stamped(
        core::TdMalformedSupportedUpdate{.kind = core::TdSupportedUpdateKind::MessageContent,
                                         .reason = core::TdMalformedUpdateReason::InvalidContent,
                                         .tdlib_type_id = 77},
        4);
    normalizer.on_update(1, 1, malformed);
    const auto failure = normalizer.status();
    CHECK(failure.phase == daemon::StreamNormalizationPhase::Failed);
    CHECK(failure.failure.kind == daemon::StreamFailureKind::MalformedSupported);
    CHECK(failure.failure.update_kind == core::TdSupportedUpdateKind::MessageContent);
    CHECK(failure.failure.tdlib_type_id == 77);
}

TEST_CASE("fixed timestamp arithmetic agrees with the existing DTO oracle",
          "[stream][storage][writer]") {
    constexpr std::array<std::int32_t, 12> values{1,
                                                  59,
                                                  60,
                                                  86'399,
                                                  86'400,
                                                  951'782'400,
                                                  1'000'000'000,
                                                  1'585'440'000,
                                                  1'704'067'200,
                                                  1'785'924'000,
                                                  2'000'000'000,
                                                  std::numeric_limits<std::int32_t>::max()};
    for (const auto seconds : values) {
        auto source = message();
        source.date = seconds;
        const auto oracle = daemon::materialize_message_summary(source);
        REQUIRE(oracle);
        REQUIRE(oracle->date);
        std::array<char, 20> rendered{};
        REQUIRE(daemon::stream_timestamp_utc(seconds, rendered));
        CHECK(std::string_view(rendered.data(), rendered.size()) == *oracle->date);
    }
}

TEST_CASE("bootstrap item and byte charges fail at the exact plus one",
          "[stream][storage][capacity]") {
    SECTION("items") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        for (std::size_t index = 0; index < daemon::kStreamMetadataBootstrapItems; ++index) {
            auto update =
                stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = {}}, index + 1);
            normalizer.on_update(1, 1, update);
            REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Bootstrap);
        }
        auto overflow = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = {}},
                                daemon::kStreamMetadataBootstrapItems + 1);
        normalizer.on_update(1, 1, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.kind == daemon::StreamFailureKind::Capacity);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::BootstrapItems);
        CHECK(status.failure.capacity.used == daemon::kStreamMetadataBootstrapItems);
    }
    SECTION("bytes") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        std::string exact(daemon::kStreamMetadataBootstrapBytes - 64, 'x');
        auto update =
            stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = std::move(exact)}, 1);
        normalizer.on_update(1, 1, update);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Bootstrap);
        auto overflow = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = {}}, 2);
        normalizer.on_update(1, 1, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::BootstrapBytes);
        CHECK(status.failure.capacity.would_use == daemon::kStreamMetadataBootstrapBytes + 64);
    }
}

TEST_CASE("persistent maps and arena enforce exact logical limits", "[stream][storage][capacity]") {
    SECTION("chats") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        core::TdCurrentState state;
        state.updates.reserve(daemon::kStreamMetadataChats);
        for (std::size_t index = 0; index < daemon::kStreamMetadataChats; ++index) {
            const auto ordinal = static_cast<std::int64_t>(index + 1);
            state.updates.push_back(core::TdValue::from(
                core::TdUpdateNewChat{.chat = chat(-ordinal, ordinal, core::TdChatKind::Secret)}));
        }
        auto response = stamped(std::move(state), 1);
        normalizer.on_current_state(1, 1, response);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        auto overflow = stamped(
            core::TdUpdateNewChat{.chat = chat(-70'000, 70'000, core::TdChatKind::Secret)}, 2);
        normalizer.on_update(1, 1, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::Chats);
        CHECK(status.failure.capacity.used == daemon::kStreamMetadataChats);
    }
    SECTION("entities") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        core::TdCurrentState state;
        state.updates.reserve(daemon::kStreamMetadataEntities);
        for (std::size_t index = 0; index < daemon::kStreamMetadataEntities; ++index) {
            const auto ordinal = static_cast<std::int64_t>(index + 1);
            state.updates.push_back(core::TdValue::from(
                core::TdUpdateBasicGroup{.basic_group = {.id = ordinal,
                                                         .member_count = 0,
                                                         .is_active = true,
                                                         .upgraded_to_supergroup_id = 0}}));
        }
        auto response = stamped(std::move(state), 1);
        normalizer.on_current_state(1, 1, response);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        auto overflow =
            stamped(core::TdUpdateBasicGroup{.basic_group = {.id = 200'000,
                                                             .member_count = 0,
                                                             .is_active = true,
                                                             .upgraded_to_supergroup_id = 0}},
                    2);
        normalizer.on_update(1, 1, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::Entities);
        CHECK(status.failure.capacity.used == daemon::kStreamMetadataEntities);
    }
    SECTION("string bytes") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        auto full = user(1);
        full.usernames = {std::string(daemon::kStreamMetadataBytes - 1, 'u')};
        core::TdCurrentState state;
        state.updates.push_back(core::TdValue::from(core::TdUpdateUser{.user = std::move(full)}));
        auto response = stamped(std::move(state), 1);
        normalizer.on_current_state(1, 1, response);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        auto extra = user(2);
        extra.usernames = {"x"};
        auto overflow = stamped(core::TdUpdateUser{.user = std::move(extra)}, 2);
        normalizer.on_update(1, 1, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::Bytes);
        CHECK(status.failure.capacity.would_use == daemon::kStreamMetadataBytes + 2);
    }
}

TEST_CASE("ordered storage enforces item, byte and descriptor limits",
          "[stream][storage][capacity]") {
    SECTION("candidate bytes") {
        daemon::FixedStreamNormalizer normalizer;
        bootstrap_empty(normalizer);
        for (std::size_t index = 0;
             index < daemon::kStreamMetadataOrderBytes / daemon::kStreamMetadataItemBytes;
             ++index) {
            const auto ordinal = static_cast<std::int64_t>(index + 1);
            auto update = stamped(core::TdUpdateNewChat{.chat = chat(-ordinal, ordinal,
                                                                     core::TdChatKind::Supergroup)},
                                  index + 2);
            normalizer.on_update(1, 1, update);
            REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        }
        auto overflow = stamped(
            core::TdUpdateNewChat{.chat = chat(-1000, 1000, core::TdChatKind::Supergroup)}, 100);
        normalizer.on_update(1, 1, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::OrderBytes);
        CHECK(status.failure.capacity.would_use ==
              daemon::kStreamMetadataOrderBytes + daemon::kStreamMetadataItemBytes);
    }
    SECTION("candidate descriptors") {
        daemon::FixedStreamNormalizer normalizer;
        bootstrap_empty(normalizer);
        auto blocker = stamped(
            core::TdUpdateNewChat{.chat = chat(-2000, 55, core::TdChatKind::Supergroup)}, 2);
        normalizer.on_update(1, 1, blocker);
        auto base_entity = user();
        auto base_chat = chat();
        auto entity = stamped(core::TdUpdateUser{.user = base_entity}, 3);
        normalizer.on_update(1, 1, entity);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        auto known = stamped(core::TdUpdateNewChat{.chat = base_chat}, 4);
        normalizer.on_update(1, 1, known);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        for (std::size_t index = 0; index < daemon::kStreamMetadataOrderItems - 2; ++index) {
            auto title =
                stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "x"}, index + 5);
            normalizer.on_update(1, 1, title);
            REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        }
        auto overflow = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "x"},
                                daemon::kStreamMetadataOrderItems + 3);
        normalizer.on_update(1, 1, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::OrderItems);
    }
    SECTION("single item bytes") {
        daemon::FixedStreamNormalizer normalizer;
        bootstrap(normalizer);
        const daemon::StreamEvent empty =
            daemon::ChatChangeEvent{daemon::TitleChatChange{.chat_id = -1001, .title = {}}};
        const auto empty_line = daemon::stream_event_line(empty);
        REQUIRE(empty_line);
        std::string title(daemon::kStreamMetadataItemBytes + 1 - empty_line->size(), 'x');
        auto overflow =
            stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = std::move(title)}, 11);
        normalizer.on_update(7, 9, overflow);
        const auto status = normalizer.status();
        REQUIRE(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.capacity.resource == daemon::StreamMetadataResource::ItemBytes);
        CHECK(status.failure.capacity.incoming == daemon::kStreamMetadataItemBytes + 1);
    }
}

TEST_CASE("raw chat-list bound accepts Main Archive and one hundred folders",
          "[stream][storage][capacity]") {
    daemon::FixedStreamNormalizer normalizer;
    REQUIRE(normalizer.begin(1, 1));
    auto value = chat();
    value.chat_lists.clear();
    value.chat_lists.push_back({.kind = core::TdChatListKind::Main, .folder_id = 0});
    value.chat_lists.push_back({.kind = core::TdChatListKind::Archive, .folder_id = 0});
    for (std::int32_t folder = 1; folder <= 100; ++folder) {
        value.chat_lists.push_back({.kind = core::TdChatListKind::Folder, .folder_id = folder});
    }
    core::TdCurrentState state;
    state.updates.push_back(core::TdValue::from(core::TdUpdateUser{.user = user()}));
    state.updates.push_back(core::TdValue::from(core::TdUpdateNewChat{.chat = std::move(value)}));
    auto response = stamped(std::move(state), 1);
    normalizer.on_current_state(1, 1, response);
    CHECK(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
}

TEST_CASE("bootstrap folds the current-state base and only deltas after its barrier",
          "[stream][storage][bootstrap]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    REQUIRE(normalizer.begin(1, 1));
    auto discarded = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "discarded"}, 2);
    normalizer.on_update(1, 1, discarded);
    auto retained = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "later"}, 4);
    normalizer.on_update(1, 1, retained);

    core::TdCurrentState state;
    state.updates.push_back(core::TdValue::from(core::TdUpdateUser{.user = user()}));
    state.updates.push_back(core::TdValue::from(core::TdUpdateNewChat{.chat = chat()}));
    auto response = stamped(std::move(state), 3);
    normalizer.on_current_state(1, 1, response);
    REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
    CHECK(normalizer.status().receive_sequence == 4);
    CHECK(sink.count() == 0);

    auto changed = user();
    changed.usernames = {"changed"};
    auto identity = stamped(core::TdUpdateUser{.user = std::move(changed)}, 5);
    normalizer.on_update(1, 1, identity);
    REQUIRE(sink.count() == 1);
    const auto item = nlohmann::json::parse(sink.line(0));
    CHECK(item.at("change") == "identity");
    CHECK(item.at("chat").at("title") == "later");
}

TEST_CASE("bootstrap retains exact first failures and rejects incomplete state",
          "[stream][storage][bootstrap]") {
    SECTION("missing entity half") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        core::TdCurrentState state;
        state.updates.push_back(core::TdValue::from(core::TdUpdateNewChat{.chat = chat()}));
        auto response = stamped(std::move(state), 1);
        normalizer.on_current_state(1, 1, response);
        const auto status = normalizer.status();
        CHECK(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.kind == daemon::StreamFailureKind::MalformedSupported);
    }
    SECTION("wrong entry position") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        core::TdCurrentState state;
        state.updates.push_back(core::TdValue::from(core::TdUpdateUser{.user = user()}));
        state.updates.push_back(core::TdValue::from(core::TdOk{}));
        auto response = stamped(std::move(state), 1);
        normalizer.on_current_state(1, 1, response);
        const auto status = normalizer.status();
        CHECK(status.failure.kind == daemon::StreamFailureKind::WrongCurrentState);
        CHECK(status.failure.current_state_index == 1);
    }
    SECTION("rate limit remains distinct") {
        struct RateLimitCase {
            std::string_view message;
            std::int32_t retry_after;
        };
        static constexpr std::array cases{
            RateLimitCase{"FLOOD_WAIT_17", 17},
            RateLimitCase{"retry", 0},
            RateLimitCase{"FLOOD_WAIT_0", 0},
            RateLimitCase{"retry after 2147483647 seconds",
                          std::numeric_limits<std::int32_t>::max()},
            RateLimitCase{"retry after 21474836499999999999 seconds",
                          std::numeric_limits<std::int32_t>::max()},
        };
        for (const auto& entry : cases) {
            CAPTURE(entry.message);
            daemon::FixedStreamNormalizer normalizer;
            REQUIRE(normalizer.begin(1, 1));
            auto response =
                stamped(core::TdError{.code = 429, .message = std::string(entry.message)}, 1);
            normalizer.on_current_state(1, 1, response);
            const auto status = normalizer.status();
            CHECK(status.failure.kind == daemon::StreamFailureKind::RateLimited);
            CHECK(status.failure.tdlib_error_code == 429);
            CHECK(status.failure.retry_after == entry.retry_after);
        }
    }
    SECTION("direct conversion remains distinct") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        auto response = stamped(core::TdDirectConversionError{.tdlib_type_id = 99}, 1);
        normalizer.on_current_state(1, 1, response);
        const auto status = normalizer.status();
        CHECK(status.failure.kind == daemon::StreamFailureKind::DirectConversion);
        CHECK(status.failure.tdlib_type_id == 99);
    }
    SECTION("dispatch failure is one shot") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        normalizer.on_current_state_failure(1, 1);
        CHECK(normalizer.status().failure.kind == daemon::StreamFailureKind::DispatchFailure);
        normalizer.on_current_state_failure(1, 1);
        CHECK(normalizer.status().failure.kind == daemon::StreamFailureKind::DispatchFailure);
    }
}

TEST_CASE("persistent and ordered arenas compact before admitting reusable capacity",
          "[stream][storage][compaction]") {
    SECTION("persistent replacement") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        auto first = user(1);
        first.usernames = {std::string(std::size_t{4} * 1024 * 1024, 'a')};
        auto second = user(2);
        second.usernames = {std::string(std::size_t{4} * 1024 * 1024, 'b')};
        core::TdCurrentState state;
        state.updates.push_back(core::TdValue::from(core::TdUpdateUser{.user = std::move(first)}));
        state.updates.push_back(core::TdValue::from(core::TdUpdateUser{.user = std::move(second)}));
        auto response = stamped(std::move(state), 1);
        normalizer.on_current_state(1, 1, response);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);

        auto grown = user(1);
        grown.usernames = {std::string(std::size_t{12} * 1024 * 1024, 'c')};
        auto replace_first = stamped(core::TdUpdateUser{.user = std::move(grown)}, 2);
        normalizer.on_update(1, 1, replace_first);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
        auto shrunk = user(2);
        shrunk.usernames = {"b"};
        auto replace_second = stamped(core::TdUpdateUser{.user = std::move(shrunk)}, 3);
        normalizer.on_update(1, 1, replace_second);
        CHECK(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
    }
    SECTION("order completion and drain") {
        FixedSink sink;
        daemon::FixedStreamNormalizer normalizer(&sink);
        bootstrap(normalizer);
        auto missing = stamped(
            core::TdUpdateNewChat{.chat = chat(-2000, 55, core::TdChatKind::Supergroup)}, 11);
        normalizer.on_update(7, 9, missing);
        auto queued = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "queued"}, 12);
        normalizer.on_update(7, 9, queued);
        core::TdSupergroup group{
            .id = 55, .usernames = {"group"}, .is_channel = false, .is_forum = false};
        auto completion = stamped(core::TdUpdateSupergroup{.supergroup = std::move(group)}, 13);
        normalizer.on_update(7, 9, completion);
        REQUIRE(sink.count() == 3);
        auto after = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "after"}, 14);
        normalizer.on_update(7, 9, after);
        REQUIRE(sink.count() == 4);
        CHECK(nlohmann::json::parse(sink.line(3)).at("title") == "after");
    }
}

TEST_CASE("candidate descriptor ring wraps and reuses a nonzero head",
          "[stream][storage][ordering][compaction]") {
    daemon::FixedStreamNormalizer normalizer;
    bootstrap(normalizer);
    std::uint64_t sequence = 11;
    auto first = stamped(
        core::TdUpdateNewChat{.chat = chat(-2000, 55, core::TdChatKind::Supergroup)}, sequence++);
    normalizer.on_update(7, 9, first);
    auto before_second =
        stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "before"}, sequence++);
    normalizer.on_update(7, 9, before_second);
    auto second = stamped(
        core::TdUpdateNewChat{.chat = chat(-3000, 88, core::TdChatKind::Supergroup)}, sequence++);
    normalizer.on_update(7, 9, second);
    auto after_second =
        stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "after"}, sequence++);
    normalizer.on_update(7, 9, after_second);
    auto first_entity = stamped(core::TdUpdateSupergroup{.supergroup = {.id = 55,
                                                                        .usernames = {"first"},
                                                                        .is_channel = false,
                                                                        .is_forum = false}},
                                sequence++);
    normalizer.on_update(7, 9, first_entity);
    REQUIRE(normalizer.status().ordering_barrier_open);

    for (std::size_t index = 0; index < daemon::kStreamMetadataOrderItems - 4; ++index) {
        auto title =
            stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "wrapped"}, sequence++);
        normalizer.on_update(7, 9, title);
        REQUIRE(normalizer.status().phase == daemon::StreamNormalizationPhase::Ready);
    }
    auto second_entity = stamped(core::TdUpdateSupergroup{.supergroup = {.id = 88,
                                                                         .usernames = {"second"},
                                                                         .is_channel = false,
                                                                         .is_forum = false}},
                                 sequence++);
    normalizer.on_update(7, 9, second_entity);
    const auto status = normalizer.status();
    CHECK(status.phase == daemon::StreamNormalizationPhase::Ready);
    CHECK_FALSE(status.ordering_barrier_open);
    CHECK(status.ready_for_admission());
    CHECK(status.receive_sequence == sequence - 1);
}

TEST_CASE("stream rate-limit details survive every normalization phase",
          "[stream][storage][rate-limit]") {
    SECTION("buffered bootstrap update") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        auto error = stamped(core::TdError{.code = 429, .message = "FLOOD_WAIT_17"}, 1);
        normalizer.on_update(1, 1, error);
        const auto status = normalizer.status();
        CHECK(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.kind == daemon::StreamFailureKind::RateLimited);
        CHECK(status.failure.tdlib_error_code == 429);
        CHECK(status.failure.retry_after == 17);
    }
    SECTION("current-state entry") {
        daemon::FixedStreamNormalizer normalizer;
        REQUIRE(normalizer.begin(1, 1));
        core::TdCurrentState state;
        state.updates.push_back(
            core::TdValue::from(core::TdError{.code = 429, .message = "retry after 23 seconds"}));
        auto response = stamped(std::move(state), 1);
        normalizer.on_current_state(1, 1, response);
        const auto status = normalizer.status();
        CHECK(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.kind == daemon::StreamFailureKind::RateLimited);
        CHECK(status.failure.tdlib_error_code == 429);
        CHECK(status.failure.retry_after == 23);
        CHECK(status.failure.current_state_index == 0);
    }
    SECTION("active update") {
        daemon::FixedStreamNormalizer normalizer;
        bootstrap_empty(normalizer);
        auto error = stamped(core::TdError{.code = 429, .message = "FLOOD_WAIT_29"}, 2);
        normalizer.on_update(1, 1, error);
        const auto status = normalizer.status();
        CHECK(status.phase == daemon::StreamNormalizationPhase::Failed);
        CHECK(status.failure.kind == daemon::StreamFailureKind::RateLimited);
        CHECK(status.failure.tdlib_error_code == 429);
        CHECK(status.failure.retry_after == 29);
    }
}

TEST_CASE("reverse entity completion retains FIFO and admission readiness",
          "[stream][storage][ordering]") {
    FixedSink sink;
    daemon::FixedStreamNormalizer normalizer(&sink);
    bootstrap_empty(normalizer);
    auto first =
        stamped(core::TdUpdateNewChat{.chat = chat(-2000, 55, core::TdChatKind::Supergroup)}, 2);
    normalizer.on_update(1, 1, first);
    auto second =
        stamped(core::TdUpdateNewChat{.chat = chat(-3000, 88, core::TdChatKind::Supergroup)}, 3);
    normalizer.on_update(1, 1, second);
    CHECK(normalizer.status().ordering_barrier_open);
    CHECK_FALSE(normalizer.status().ready_for_admission());

    auto second_entity = stamped(core::TdUpdateSupergroup{.supergroup = {.id = 88,
                                                                         .usernames = {"second"},
                                                                         .is_channel = false,
                                                                         .is_forum = false}},
                                 4);
    normalizer.on_update(1, 1, second_entity);
    CHECK(sink.count() == 0);
    CHECK_FALSE(normalizer.status().ready_for_admission());

    auto first_entity = stamped(core::TdUpdateSupergroup{.supergroup = {.id = 55,
                                                                        .usernames = {"first"},
                                                                        .is_channel = false,
                                                                        .is_forum = false}},
                                5);
    normalizer.on_update(1, 1, first_entity);
    REQUIRE(sink.count() == 4);
    CHECK(nlohmann::json::parse(sink.line(0)).at("chat").at("id") == -2000);
    CHECK(nlohmann::json::parse(sink.line(1)).at("chat").at("id") == -3000);
    CHECK(nlohmann::json::parse(sink.line(2)).at("change") == "identity");
    CHECK(nlohmann::json::parse(sink.line(3)).at("change") == "identity");
    CHECK(normalizer.status().ready_for_admission());
}

TEST_CASE("stream status publication never exposes mixed snapshots",
          "[stream][storage][status][concurrency]") {
    BlockingStatusProbe probe;
    daemon::FixedStreamNormalizer normalizer(
        nullptr, {.context = &probe, .hook = &BlockingStatusProbe::notify});
    bootstrap_empty(normalizer);

    probe.target = daemon::detail::StreamStatusPublishPoint::FailurePayload;
    auto malformed = stamped(
        core::TdMalformedSupportedUpdate{.kind = core::TdSupportedUpdateKind::MessageContent,
                                         .reason = core::TdMalformedUpdateReason::InvalidContent,
                                         .tdlib_type_id = 91},
        2);
    std::thread writer([&] { normalizer.on_update(1, 1, malformed); });
    wait_entered(probe);
    std::atomic<bool> reader_started{false};
    auto reader = std::async(std::launch::async, [&] {
        reader_started.store(true, std::memory_order_release);
        return normalizer.status();
    });
    while (!reader_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK(reader.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    probe.release.store(true, std::memory_order_release);
    writer.join();
    const auto failure = reader.get();
    CHECK(failure.phase == daemon::StreamNormalizationPhase::Failed);
    CHECK(failure.failure.kind == daemon::StreamFailureKind::MalformedSupported);
    CHECK(failure.failure.update_kind == core::TdSupportedUpdateKind::MessageContent);
    CHECK(failure.failure.malformed_reason == core::TdMalformedUpdateReason::InvalidContent);
    CHECK(failure.failure.tdlib_type_id == 91);

    BlockingStatusProbe reset_probe;
    reset_probe.target = daemon::detail::StreamStatusPublishPoint::Reset;
    daemon::FixedStreamNormalizer replacement(
        nullptr, {.context = &reset_probe, .hook = &BlockingStatusProbe::notify});
    bool replacement_began = false;
    std::thread replacer([&] { replacement_began = replacement.begin(17, 22); });
    wait_entered(reset_probe);
    std::atomic<bool> reset_reader_started{false};
    auto reset_reader = std::async(std::launch::async, [&] {
        reset_reader_started.store(true, std::memory_order_release);
        return replacement.status();
    });
    while (!reset_reader_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK(reset_reader.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    reset_probe.release.store(true, std::memory_order_release);
    replacer.join();
    REQUIRE(replacement_began);
    const auto reset = reset_reader.get();
    CHECK(reset.client_id == 17);
    CHECK(reset.generation == 22);
    CHECK(reset.phase == daemon::StreamNormalizationPhase::Bootstrap);
    CHECK(reset.receive_sequence == 0);
    CHECK_FALSE(reset.ordering_barrier_open);
    CHECK(reset.failure.kind == daemon::StreamFailureKind::None);
}

TEST_CASE("stream status publication covers sequence barrier and revision rollover",
          "[stream][storage][status]") {
    BlockingStatusProbe sequence_probe;
    daemon::FixedStreamNormalizer sequenced(
        nullptr, {.context = &sequence_probe, .hook = &BlockingStatusProbe::notify});
    bootstrap_empty(sequenced);
    sequence_probe.target = daemon::detail::StreamStatusPublishPoint::Sequence;
    auto title = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "x"}, 2);
    std::thread update([&] { sequenced.on_update(1, 1, title); });
    wait_entered(sequence_probe);
    std::atomic<bool> reader_started{false};
    auto reader = std::async(std::launch::async, [&] {
        reader_started.store(true, std::memory_order_release);
        return sequenced.status();
    });
    while (!reader_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK(reader.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    sequence_probe.release.store(true, std::memory_order_release);
    update.join();
    const auto advanced = reader.get();
    CHECK(advanced.client_id == 1);
    CHECK(advanced.generation == 1);
    CHECK(advanced.receive_sequence == 2);
    CHECK(advanced.phase == daemon::StreamNormalizationPhase::Failed);
    CHECK(advanced.failure.kind == daemon::StreamFailureKind::MalformedSupported);

    BlockingStatusProbe barrier_probe;
    daemon::FixedStreamNormalizer barrier(
        nullptr, {.context = &barrier_probe, .hook = &BlockingStatusProbe::notify});
    bootstrap(barrier);
    barrier_probe.target = daemon::detail::StreamStatusPublishPoint::Barrier;
    auto barrier_title = stamped(core::TdUpdateChatTitle{.chat_id = -1001, .title = "barrier"}, 11);
    std::thread barrier_writer([&] { barrier.on_update(7, 9, barrier_title); });
    wait_entered(barrier_probe);
    std::atomic<bool> barrier_reader_started{false};
    auto barrier_reader = std::async(std::launch::async, [&] {
        barrier_reader_started.store(true, std::memory_order_release);
        return barrier.status();
    });
    while (!barrier_reader_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK(barrier_reader.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    barrier_probe.release.store(true, std::memory_order_release);
    barrier_writer.join();
    const auto drained = barrier_reader.get();
    CHECK(drained.phase == daemon::StreamNormalizationPhase::Ready);
    CHECK(drained.receive_sequence == 11);
    CHECK_FALSE(drained.ordering_barrier_open);

    daemon::FixedStreamNormalizer rollover(
        nullptr,
        {.initial_revision = std::numeric_limits<std::uint64_t>::max() - std::uint64_t{1}});
    REQUIRE(rollover.begin(9, 11));
    const auto wrapped = rollover.status();
    CHECK(wrapped.client_id == 9);
    CHECK(wrapped.generation == 11);
    CHECK(wrapped.phase == daemon::StreamNormalizationPhase::Bootstrap);
}
