#include "daemon/stream_storage.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace tgcli::daemon {

// Fixed callback storage uses validated dynamic indices without throwing accessors; the closed
// JSON/update unions intentionally keep their exact literals and branch structure together.
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-constant-array-index,modernize-raw-string-literal,readability-function-cognitive-complexity,bugprone-unchecked-optional-access,modernize-use-nodiscard,readability-make-member-function-const,readability-convert-member-functions-to-static,readability-simplify-boolean-expr)

namespace {

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;
constexpr std::uint32_t kNoHandle = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kEntityHandleBit = 0x80000000U;
constexpr std::size_t kBootstrapPhysicalBytes = kStreamMetadataBootstrapBytes * 2;
constexpr unsigned char kBorrowPoison = 0xA5;

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<StreamNormalizationPhase>::is_always_lock_free);
static_assert(std::atomic<StreamFailureKind>::is_always_lock_free);
static_assert(std::atomic<core::TdSupportedUpdateKind>::is_always_lock_free);
static_assert(std::atomic<core::TdMalformedUpdateReason>::is_always_lock_free);
static_assert(std::atomic<StreamMetadataResource>::is_always_lock_free);
static_assert(std::atomic<StreamMetadataPhase>::is_always_lock_free);

bool valid_int53(std::int64_t value) noexcept {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_positive_int53(std::int64_t value) noexcept {
    return value > 0 && value <= kMaximumInt53;
}

bool valid_nonnegative_int53(std::int64_t value) noexcept {
    return value >= 0 && value <= kMaximumInt53;
}

bool ascii_equal_ignore_case(char left, char right) noexcept {
    const auto lower = [](char value) noexcept {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
    };
    return lower(left) == lower(right);
}

bool ascii_word(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
}

bool consume_ignore_case(std::string_view value, std::size_t& position,
                         std::string_view expected) noexcept {
    if (expected.size() > value.size() - position) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (!ascii_equal_ignore_case(value[position + index], expected[index])) {
            return false;
        }
    }
    position += expected.size();
    return true;
}

std::int32_t stream_retry_after(std::string_view message) noexcept {
    for (std::size_t start = 0; start < message.size(); ++start) {
        if (start != 0 && ascii_word(message[start - 1])) {
            continue;
        }
        auto position = start;
        if (!consume_ignore_case(message, position, "FLOOD_WAIT_")) {
            position = start;
            if (!consume_ignore_case(message, position, "retry")) {
                continue;
            }
            const auto space_start = position;
            while (position < message.size() && ascii_space(message[position])) {
                ++position;
            }
            if (position == space_start || !consume_ignore_case(message, position, "after")) {
                continue;
            }
            while (position < message.size() && ascii_space(message[position])) {
                ++position;
            }
        }
        if (position == message.size() || message[position] < '0' || message[position] > '9') {
            continue;
        }
        std::int32_t result = 0;
        constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
        while (position < message.size() && message[position] >= '0' && message[position] <= '9') {
            const auto digit = static_cast<std::int32_t>(message[position] - '0');
            result = result > (maximum - digit) / 10 ? maximum : result * 10 + digit;
            ++position;
        }
        return result;
    }
    return 0;
}

std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

bool valid_chat_list(const core::TdChatList& list) noexcept {
    switch (list.kind) {
    case core::TdChatListKind::Main:
    case core::TdChatListKind::Archive:
        return list.folder_id == 0;
    case core::TdChatListKind::Folder:
        return list.folder_id > 0;
    case core::TdChatListKind::Unknown:
        return false;
    }
    return false;
}

bool same_chat_list(const core::TdChatList& left, const core::TdChatList& right) noexcept {
    return left.kind == right.kind && left.folder_id == right.folder_id;
}

bool valid_sender(const core::TdMessageSender& sender) noexcept {
    switch (sender.kind) {
    case core::TdMessageSenderKind::User:
        return valid_positive_int53(sender.id);
    case core::TdMessageSenderKind::Chat:
        return valid_int53(sender.id);
    case core::TdMessageSenderKind::Unknown:
        return false;
    }
    return false;
}

bool valid_topic(const core::TdTopic& topic) noexcept {
    switch (topic.kind) {
    case core::TdTopicKind::Forum:
        return topic.id > 0 && topic.id <= std::numeric_limits<std::int32_t>::max();
    case core::TdTopicKind::Thread:
    case core::TdTopicKind::Direct:
    case core::TdTopicKind::Saved:
        return valid_positive_int53(topic.id);
    case core::TdTopicKind::Unknown:
        return false;
    }
    return false;
}

bool valid_message(const core::TdMessageSummary& message) noexcept {
    return valid_int53(message.id) && valid_int53(message.chat_id) && message.date >= 0 &&
           valid_sender(message.sender) && (!message.topic || valid_topic(*message.topic)) &&
           common::valid_utf8(message.text);
}

bool valid_reaction(const core::TdReactionType& reaction) noexcept {
    switch (reaction.kind) {
    case core::TdReactionKind::Emoji:
        return !reaction.emoji.empty() && common::valid_utf8(reaction.emoji) &&
               reaction.custom_emoji_id == 0;
    case core::TdReactionKind::CustomEmoji:
        return reaction.emoji.empty() && reaction.custom_emoji_id > 0;
    case core::TdReactionKind::Paid:
        return reaction.emoji.empty() && reaction.custom_emoji_id == 0;
    case core::TdReactionKind::Unknown:
        return false;
    }
    return false;
}

bool valid_username(std::string_view value) noexcept {
    return !value.empty() && value.find('\0') == std::string_view::npos &&
           common::valid_utf8(value);
}

std::uint64_t message_string_charge(const core::TdMessageSummary& message,
                                    bool persistent) noexcept {
    std::uint64_t result = message.text.size() + (persistent ? 1U : 0U);
    if (message.date != 0) {
        result = saturated_add(result, 20U + (persistent ? 1U : 0U));
    }
    return result;
}

class JsonWriter {
  public:
    explicit JsonWriter(std::span<char> output) noexcept : output_(output) {}

    void raw(std::string_view value) noexcept {
        add_required(value.size());
        for (const char character : value) {
            put(character);
        }
    }

    void character(char value) noexcept {
        add_required(1);
        put(value);
    }

    void boolean(bool value) noexcept {
        raw(value ? "true" : "false");
    }

    template <typename Integer> void integer(Integer value) noexcept {
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (converted.ec != std::errc{}) {
            valid_ = false;
            return;
        }
        raw({buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())});
    }

    void escaped(std::string_view value) noexcept {
        if (!common::valid_utf8(value)) {
            valid_ = false;
        }
        for (const unsigned char byte : value) {
            switch (byte) {
            case '"':
                raw("\\\"");
                break;
            case '\\':
                raw("\\\\");
                break;
            case '\b':
                raw("\\b");
                break;
            case '\f':
                raw("\\f");
                break;
            case '\n':
                raw("\\n");
                break;
            case '\r':
                raw("\\r");
                break;
            case '\t':
                raw("\\t");
                break;
            default:
                if (byte < 0x20U) {
                    constexpr std::string_view hex = "0123456789abcdef";
                    raw("\\u00");
                    character(hex[byte >> 4U]);
                    character(hex[byte & 0x0FU]);
                } else {
                    character(static_cast<char>(byte));
                }
            }
        }
    }

    void quoted(std::string_view value) noexcept {
        character('"');
        escaped(value);
        character('"');
    }

    void timestamp(std::int32_t seconds, bool nullable) noexcept {
        if (seconds == 0 && nullable) {
            raw("null");
            return;
        }
        std::array<char, 20> value{};
        if (!stream_timestamp_utc(seconds, value)) {
            valid_ = false;
            return;
        }
        character('"');
        raw(std::string_view(value.data(), value.size()));
        character('"');
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_ && !required_overflow_;
    }

    [[nodiscard]] std::size_t required() const noexcept {
        return required_;
    }

    [[nodiscard]] std::size_t written() const noexcept {
        return std::min(required_, output_.size());
    }

  private:
    void add_required(std::size_t amount) noexcept {
        if (amount > std::numeric_limits<std::size_t>::max() - required_) {
            required_ = std::numeric_limits<std::size_t>::max();
            required_overflow_ = true;
            return;
        }
        required_ += amount;
    }

    void put(char value) noexcept {
        if (position_ < output_.size()) {
            output_[position_] = value;
        }
        if (position_ != std::numeric_limits<std::size_t>::max()) {
            ++position_;
        }
    }

    std::span<char> output_;
    std::size_t position_ = 0;
    std::size_t required_ = 0;
    bool valid_ = true;
    bool required_overflow_ = false;
};

std::string_view sender_kind(core::TdMessageSenderKind kind) noexcept {
    return kind == core::TdMessageSenderKind::User ? "user" : "chat";
}

std::string_view topic_kind(core::TdTopicKind kind) noexcept {
    switch (kind) {
    case core::TdTopicKind::Forum:
        return "forum";
    case core::TdTopicKind::Thread:
        return "thread";
    case core::TdTopicKind::Direct:
        return "direct";
    case core::TdTopicKind::Saved:
        return "saved";
    case core::TdTopicKind::Unknown:
        return {};
    }
    return {};
}

std::string_view content_kind(core::TdMessageContentKind kind) noexcept {
    switch (kind) {
    case core::TdMessageContentKind::Text:
        return "text";
    case core::TdMessageContentKind::Photo:
        return "photo";
    case core::TdMessageContentKind::Video:
        return "video";
    case core::TdMessageContentKind::Document:
        return "doc";
    case core::TdMessageContentKind::Voice:
        return "voice";
    case core::TdMessageContentKind::Other:
        return "other";
    }
    return {};
}

void write_sender(JsonWriter& writer, const core::TdMessageSender& sender) noexcept {
    writer.raw("{\"type\":");
    writer.quoted(sender_kind(sender.kind));
    writer.raw(",\"id\":");
    writer.integer(sender.id);
    writer.character('}');
}

void write_topic(JsonWriter& writer, const std::optional<core::TdTopic>& topic) noexcept {
    if (!topic) {
        writer.raw("null");
        return;
    }
    writer.raw("{\"kind\":");
    writer.quoted(topic_kind(topic->kind));
    writer.raw(",\"id\":");
    writer.integer(topic->id);
    writer.character('}');
}

void write_message(JsonWriter& writer, const core::TdMessageSummary& message, std::string_view text,
                   StreamRoutingSidecar* routing = nullptr) noexcept {
    const auto message_start = writer.required();
    writer.raw("{\"id\":");
    writer.integer(message.id);
    writer.raw(",\"chat_id\":");
    writer.integer(message.chat_id);
    writer.raw(",\"date\":");
    writer.timestamp(message.date, true);
    writer.raw(",\"sender\":");
    write_sender(writer, message.sender);
    writer.raw(",\"is_outgoing\":");
    writer.boolean(message.is_outgoing);
    writer.raw(",\"topic\":");
    write_topic(writer, message.topic);
    writer.raw(",\"type\":");
    writer.quoted(content_kind(message.content_kind));
    writer.raw(",\"text\":\"");
    const auto text_start = writer.required();
    writer.escaped(text);
    const auto text_end = writer.required();
    writer.character('"');
    writer.character('}');
    if (routing != nullptr) {
        routing->message_offset = static_cast<std::uint32_t>(message_start);
        routing->message_size = static_cast<std::uint32_t>(writer.required() - message_start);
        routing->text_offset = static_cast<std::uint32_t>(text_start);
        routing->text_size = static_cast<std::uint32_t>(text_end - text_start);
    }
}

void write_reaction(JsonWriter& writer, const core::TdReactionType& reaction) noexcept {
    switch (reaction.kind) {
    case core::TdReactionKind::Emoji:
        writer.raw("{\"type\":\"emoji\",\"emoji\":");
        writer.quoted(reaction.emoji);
        writer.character('}');
        return;
    case core::TdReactionKind::CustomEmoji:
        writer.raw("{\"type\":\"custom\",\"custom_emoji_id\":\"");
        writer.integer(reaction.custom_emoji_id);
        writer.raw("\"}");
        return;
    case core::TdReactionKind::Paid:
        writer.raw("{\"type\":\"paid\"}");
        return;
    case core::TdReactionKind::Unknown:
        return;
    }
}

} // namespace

std::int32_t stream_retry_after_seconds(std::string_view message) noexcept {
    return stream_retry_after(message);
}

bool stream_timestamp_utc(std::int32_t seconds, std::span<char, 20> output) noexcept {
    if (seconds <= 0) {
        return false;
    }
    const auto value = static_cast<std::int64_t>(seconds);
    std::int64_t days = value / 86'400;
    const auto day_seconds = value % 86'400;
    const int hour = static_cast<int>(day_seconds / 3'600);
    const int minute = static_cast<int>((day_seconds % 3'600) / 60);
    const int second = static_cast<int>(day_seconds % 60);

    days += 719'468;
    const auto era = (days >= 0 ? days : days - 146'096) / 146'097;
    const auto day_of_era = days - era * 146'097;
    const auto year_of_era =
        (day_of_era - day_of_era / 1'460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
    auto year = static_cast<int>(year_of_era + era * 400);
    const auto day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const auto month_prime = (5 * day_of_year + 2) / 153;
    const int day = static_cast<int>(day_of_year - (153 * month_prime + 2) / 5 + 1);
    const int month = static_cast<int>(month_prime + (month_prime < 10 ? 3 : -9));
    year += month <= 2 ? 1 : 0;
    if (year < 1970 || year > 2038) {
        return false;
    }
    const auto put_two = [&](std::size_t offset, int number) {
        output[offset] = static_cast<char>('0' + number / 10);
        output[offset + 1] = static_cast<char>('0' + number % 10);
    };
    output[0] = static_cast<char>('0' + year / 1000 % 10);
    output[1] = static_cast<char>('0' + year / 100 % 10);
    output[2] = static_cast<char>('0' + year / 10 % 10);
    output[3] = static_cast<char>('0' + year % 10);
    output[4] = '-';
    put_two(5, month);
    output[7] = '-';
    put_two(8, day);
    output[10] = 'T';
    put_two(11, hour);
    output[13] = ':';
    put_two(14, minute);
    output[16] = ':';
    put_two(17, second);
    output[19] = 'Z';
    return true;
}

StreamEscapeResult stream_json_escape(std::string_view value, std::span<char> output) noexcept {
    JsonWriter writer(output);
    writer.escaped(value);
    const auto required = writer.required();
    return {.valid = writer.valid(),
            .written_bytes = std::min(required, output.size()),
            .required_bytes = required};
}

StreamItemView::StreamItemView(std::span<const char> first, std::span<const char> second,
                               std::uint64_t sequence, StreamRoutingSidecar routing) noexcept
    : first_(first), second_(second), sequence_(sequence), routing_(routing) {}

std::array<std::span<const char>, 2> StreamItemView::spans() const noexcept {
    return {first_, second_};
}

std::size_t StreamItemView::size() const noexcept {
    return first_.size() + second_.size();
}

std::uint64_t StreamItemView::receive_sequence() const noexcept {
    return sequence_;
}

const StreamRoutingSidecar& StreamItemView::routing() const noexcept {
    return routing_;
}

StreamMetadataCursor::StreamMetadataCursor(const void* owner, std::size_t position,
                                           std::uint64_t token) noexcept
    : owner_(owner), position_(position), token_(token) {}

StreamMetadataView::StreamMetadataView(void* owner, std::uint64_t token) noexcept
    : owner_(owner), token_(token) {}

StreamMetadataCursor StreamMetadataView::cursor() const noexcept {
    return {owner_, 0, token_};
}

class FixedStreamNormalizer::Impl {
  public:
    enum class EntityKind : std::uint8_t { User, BasicGroup, Supergroup };
    enum class ChatKind : std::uint8_t { Private, BasicGroup, Supergroup, Channel, Secret };
    enum class BufferedKind : std::uint8_t {
        Entity,
        NewChat,
        Title,
        LastMessage,
        ListAdded,
        ListRemoved,
        ReadInbox,
        UnreadMention,
        UnreadReaction,
        UnreadPollVote,
        MarkedUnread
    };

    struct Block {
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
        std::uint32_t previous = kNoHandle;
        std::uint32_t next = kNoHandle;
        bool linked = false;
    };

    struct StoredMessage {
        bool present = false;
        std::int64_t id = 0;
        std::int64_t chat_id = 0;
        std::int32_t date = 0;
        core::TdMessageSender sender;
        bool is_outgoing = false;
        std::optional<core::TdTopic> topic;
        core::TdMessageContentKind content_kind = core::TdMessageContentKind::Other;
        std::uint32_t text_offset = 0;
        std::uint32_t text_size = 0;
    };

    struct EntityRecord {
        bool occupied = false;
        EntityKind kind = EntityKind::User;
        std::int64_t id = 0;
        bool is_bot = false;
        bool is_channel = false;
        std::uint32_t username_count = 0;
        Block block;
    };

    struct ChatRecord {
        bool occupied = false;
        std::int64_t id = 0;
        ChatKind kind = ChatKind::Private;
        std::int64_t related_id = 0;
        std::uint32_t title_size = 0;
        std::array<core::TdChatList, kStreamRawChatLists> lists{};
        std::uint32_t list_count = 0;
        bool is_marked_unread = false;
        std::int32_t unread_count = 0;
        std::int32_t unread_mention_count = 0;
        std::int32_t unread_reaction_count = 0;
        std::int32_t unread_poll_vote_count = 0;
        StoredMessage last_message;
        Block block;
    };

    struct FrozenChat {
        std::int64_t id = 0;
        ChatKind kind = ChatKind::Private;
        std::int64_t related_id = 0;
        std::uint32_t title_size = 0;
        std::array<core::TdChatList, kStreamRawChatLists> lists{};
        std::uint32_t list_count = 0;
        bool is_marked_unread = false;
        std::int32_t unread_count = 0;
        std::int32_t unread_mention_count = 0;
        std::int32_t unread_reaction_count = 0;
        std::int32_t unread_poll_vote_count = 0;
        StoredMessage last_message;
    };

    struct Candidate {
        bool occupied = false;
        bool complete = false;
        std::uint64_t sequence = 0;
        std::uint32_t charge = 0;
        Block block;
        StreamRoutingSidecar routing;
        FrozenChat frozen;
    };

    struct Buffered {
        BufferedKind kind = BufferedKind::Title;
        std::uint64_t sequence = 0;
        std::uint64_t charge = 0;
        EntityKind entity_kind = EntityKind::User;
        ChatKind chat_kind = ChatKind::Private;
        std::int64_t id = 0;
        std::int64_t related_id = 0;
        bool first_bool = false;
        bool second_bool = false;
        std::int32_t first_count = 0;
        std::int32_t second_count = 0;
        std::int32_t third_count = 0;
        std::int32_t fourth_count = 0;
        std::int64_t message_id = 0;
        core::TdChatList list;
        std::array<core::TdChatList, kStreamRawChatLists> lists{};
        std::uint32_t list_count = 0;
        StoredMessage message;
        std::uint32_t blob_offset = 0;
        std::uint32_t blob_size = 0;
        std::uint32_t username_count = 0;
        std::uint32_t title_size = 0;
    };

    class StatusWrite {
      public:
        explicit StatusWrite(Impl& owner) noexcept
            : owner_(owner), published_revision_(owner_.begin_status_write()) {}

        ~StatusWrite() {
            owner_.status_revision.store(published_revision_, std::memory_order_release);
        }

        StatusWrite(const StatusWrite&) = delete;
        StatusWrite& operator=(const StatusWrite&) = delete;
        StatusWrite(StatusWrite&&) = delete;
        StatusWrite& operator=(StatusWrite&&) = delete;

      private:
        Impl& owner_;
        std::uint64_t published_revision_ = 0;
    };

    explicit Impl(StreamReceiveSink* sink_value, detail::StreamStatusPublishProbe probe_value)
        : sink(sink_value), chats(std::make_unique<ChatRecord[]>(kStreamMetadataChats)),
          entities(std::make_unique<EntityRecord[]>(kStreamMetadataEntities)),
          metadata_arena(std::make_unique<char[]>(kStreamMetadataBytes)),
          bootstrap(std::make_unique<Buffered[]>(kStreamMetadataBootstrapItems)),
          bootstrap_arena(std::make_unique<char[]>(kBootstrapPhysicalBytes)),
          candidates(std::make_unique<Candidate[]>(kStreamMetadataOrderItems)),
          order_arena(std::make_unique<char[]>(kStreamMetadataOrderBytes)),
          scratch(std::make_unique<char[]>(kStreamMetadataItemBytes)), status_probe(probe_value),
          status_revision(probe_value.initial_revision) {}

    std::uint64_t begin_status_write() noexcept {
        const auto stable = status_revision.fetch_add(1U, std::memory_order_acq_rel);
        notify_status(detail::StreamStatusPublishPoint::WriterBegin);
        return stable == std::numeric_limits<std::uint64_t>::max() - 1U ? 0 : stable + 2U;
    }

    void notify_status(detail::StreamStatusPublishPoint point) const noexcept {
        if (status_probe.hook != nullptr) {
            status_probe.hook(status_probe.context, point);
        }
    }

    template <typename Function> decltype(auto) publish_status(Function&& function) noexcept {
        const StatusWrite write(*this);
        return std::forward<Function>(function)();
    }

    bool begin(std::int32_t next_client_id, std::uint64_t next_generation) noexcept {
        if (next_client_id <= 0 || next_generation == 0) {
            return false;
        }
        phase.store(StreamNormalizationPhase::Empty, std::memory_order_release);
        for (std::size_t index = 0; index < kStreamMetadataChats; ++index) {
            chats[index].occupied = false;
            chats[index].block = {};
        }
        for (std::size_t index = 0; index < kStreamMetadataEntities; ++index) {
            entities[index].occupied = false;
            entities[index].block = {};
        }
        for (std::size_t index = 0; index < kStreamMetadataOrderItems; ++index) {
            candidates[index] = {};
        }
        chat_count = 0;
        entity_count = 0;
        metadata_used = 0;
        metadata_head = kNoHandle;
        metadata_tail = kNoHandle;
        bootstrap_count = 0;
        bootstrap_charged = 0;
        bootstrap_physical = 0;
        candidate_head = 0;
        candidate_count = 0;
        order_charged = 0;
        order_used = 0;
        order_head = kNoHandle;
        order_tail = kNoHandle;
        ordering_barrier.store(false, std::memory_order_relaxed);
        failure_kind.store(StreamFailureKind::None, std::memory_order_relaxed);
        failure_update_kind.store(core::TdSupportedUpdateKind::CurrentStateEntry,
                                  std::memory_order_relaxed);
        failure_reason.store(core::TdMalformedUpdateReason::MissingObject,
                             std::memory_order_relaxed);
        failure_type_id.store(0, std::memory_order_relaxed);
        failure_error_code.store(0, std::memory_order_relaxed);
        failure_retry_after.store(0, std::memory_order_relaxed);
        failure_index.store(0, std::memory_order_relaxed);
        failure_resource.store(StreamMetadataResource::BootstrapItems, std::memory_order_relaxed);
        failure_capacity_phase.store(StreamMetadataPhase::Bootstrap, std::memory_order_relaxed);
        failure_limit.store(0, std::memory_order_relaxed);
        failure_used.store(0, std::memory_order_relaxed);
        failure_incoming.store(0, std::memory_order_relaxed);
        failure_would_use.store(0, std::memory_order_relaxed);
        last_sequence.store(0, std::memory_order_relaxed);
        notify_status(detail::StreamStatusPublishPoint::Reset);
        client_id.store(next_client_id, std::memory_order_relaxed);
        generation.store(next_generation, std::memory_order_relaxed);
        phase.store(StreamNormalizationPhase::Bootstrap, std::memory_order_release);
        return true;
    }

    bool matches(std::int32_t expected_client, std::uint64_t expected_generation) const noexcept {
        return client_id.load(std::memory_order_relaxed) == expected_client &&
               generation.load(std::memory_order_relaxed) == expected_generation;
    }

    [[nodiscard]] StreamNormalizationStatus read_status() const noexcept {
        for (;;) {
            const auto observed_revision = status_revision.load(std::memory_order_acquire);
            if ((observed_revision & 1U) != 0U) {
                continue;
            }
            StreamNormalizationStatus result{
                .client_id = client_id.load(std::memory_order_relaxed),
                .generation = generation.load(std::memory_order_relaxed),
                .receive_sequence = last_sequence.load(std::memory_order_relaxed),
                .phase = phase.load(std::memory_order_relaxed),
                .ordering_barrier_open = ordering_barrier.load(std::memory_order_relaxed),
                .failure = {
                    .kind = failure_kind.load(std::memory_order_relaxed),
                    .update_kind = failure_update_kind.load(std::memory_order_relaxed),
                    .malformed_reason = failure_reason.load(std::memory_order_relaxed),
                    .tdlib_type_id = failure_type_id.load(std::memory_order_relaxed),
                    .tdlib_error_code = failure_error_code.load(std::memory_order_relaxed),
                    .retry_after = failure_retry_after.load(std::memory_order_relaxed),
                    .current_state_index = failure_index.load(std::memory_order_relaxed),
                    .capacity = {.resource = failure_resource.load(std::memory_order_relaxed),
                                 .phase = failure_capacity_phase.load(std::memory_order_relaxed),
                                 .limit = failure_limit.load(std::memory_order_relaxed),
                                 .used = failure_used.load(std::memory_order_relaxed),
                                 .incoming = failure_incoming.load(std::memory_order_relaxed),
                                 .would_use = failure_would_use.load(std::memory_order_relaxed)}}};
            if (observed_revision == status_revision.load(std::memory_order_acquire)) {
                return result;
            }
        }
    }

    void fail(StreamFailureKind kind) noexcept {
        if (phase.load(std::memory_order_relaxed) == StreamNormalizationPhase::Failed) {
            return;
        }
        clear_candidates();
        ordering_barrier.store(false, std::memory_order_release);
        failure_kind.store(kind, std::memory_order_relaxed);
        notify_status(detail::StreamStatusPublishPoint::FailurePayload);
        phase.store(StreamNormalizationPhase::Failed, std::memory_order_release);
    }

    void fail_malformed(
        core::TdSupportedUpdateKind kind,
        core::TdMalformedUpdateReason reason = core::TdMalformedUpdateReason::InvalidEntity,
        std::int32_t type_id = 0, std::uint32_t index = 0) noexcept {
        failure_update_kind.store(kind, std::memory_order_relaxed);
        failure_reason.store(reason, std::memory_order_relaxed);
        failure_type_id.store(type_id, std::memory_order_relaxed);
        failure_index.store(index, std::memory_order_relaxed);
        fail(StreamFailureKind::MalformedSupported);
    }

    void fail_capacity(StreamMetadataCapacityFailure value) noexcept {
        failure_resource.store(value.resource, std::memory_order_relaxed);
        failure_capacity_phase.store(value.phase, std::memory_order_relaxed);
        failure_limit.store(value.limit, std::memory_order_relaxed);
        failure_used.store(value.used, std::memory_order_relaxed);
        failure_incoming.store(value.incoming, std::memory_order_relaxed);
        failure_would_use.store(value.would_use, std::memory_order_relaxed);
        fail(StreamFailureKind::Capacity);
    }

    static std::uint64_t hash(std::int64_t id, std::uint8_t kind = 0) noexcept {
        auto value = static_cast<std::uint64_t>(id) ^
                     (static_cast<std::uint64_t>(kind) * 0x9e3779b97f4a7c15ULL);
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    std::size_t chat_slot(std::int64_t id, bool& found) const noexcept {
        const auto start = static_cast<std::size_t>(hash(id)) & (kStreamMetadataChats - 1U);
        for (std::size_t probe = 0; probe < kStreamMetadataChats; ++probe) {
            const auto index = (start + probe) & (kStreamMetadataChats - 1U);
            if (!chats[index].occupied) {
                found = false;
                return index;
            }
            if (chats[index].id == id) {
                found = true;
                return index;
            }
        }
        found = false;
        return kStreamMetadataChats;
    }

    std::size_t entity_slot(EntityKind kind, std::int64_t id, bool& found) const noexcept {
        const auto start = static_cast<std::size_t>(hash(id, static_cast<std::uint8_t>(kind))) &
                           (kStreamMetadataEntities - 1U);
        for (std::size_t probe = 0; probe < kStreamMetadataEntities; ++probe) {
            const auto index = (start + probe) & (kStreamMetadataEntities - 1U);
            if (!entities[index].occupied) {
                found = false;
                return index;
            }
            if (entities[index].kind == kind && entities[index].id == id) {
                found = true;
                return index;
            }
        }
        found = false;
        return kStreamMetadataEntities;
    }

    ChatRecord* find_chat(std::int64_t id) noexcept {
        bool found = false;
        const auto index = chat_slot(id, found);
        return found ? &chats[index] : nullptr;
    }

    const ChatRecord* find_chat(std::int64_t id) const noexcept {
        bool found = false;
        const auto index = chat_slot(id, found);
        return found ? &chats[index] : nullptr;
    }

    EntityRecord* find_entity(EntityKind kind, std::int64_t id) noexcept {
        bool found = false;
        const auto index = entity_slot(kind, id, found);
        return found ? &entities[index] : nullptr;
    }

    const EntityRecord* find_entity(EntityKind kind, std::int64_t id) const noexcept {
        bool found = false;
        const auto index = entity_slot(kind, id, found);
        return found ? &entities[index] : nullptr;
    }

    static std::optional<EntityKind> entity_kind(ChatKind kind) noexcept {
        switch (kind) {
        case ChatKind::Private:
            return EntityKind::User;
        case ChatKind::BasicGroup:
            return EntityKind::BasicGroup;
        case ChatKind::Supergroup:
        case ChatKind::Channel:
            return EntityKind::Supergroup;
        case ChatKind::Secret:
            return std::nullopt;
        }
        return std::nullopt;
    }

    static std::optional<ChatKind> chat_kind(core::TdChatKind kind) noexcept {
        switch (kind) {
        case core::TdChatKind::Private:
            return ChatKind::Private;
        case core::TdChatKind::BasicGroup:
            return ChatKind::BasicGroup;
        case core::TdChatKind::Supergroup:
            return ChatKind::Supergroup;
        case core::TdChatKind::Channel:
            return ChatKind::Channel;
        case core::TdChatKind::Secret:
            return ChatKind::Secret;
        case core::TdChatKind::Unknown:
            return std::nullopt;
        }
        return std::nullopt;
    }

    static core::TdSupportedUpdateKind entity_update_kind(EntityKind kind) noexcept {
        switch (kind) {
        case EntityKind::User:
            return core::TdSupportedUpdateKind::User;
        case EntityKind::BasicGroup:
            return core::TdSupportedUpdateKind::BasicGroup;
        case EntityKind::Supergroup:
            return core::TdSupportedUpdateKind::Supergroup;
        }
        return core::TdSupportedUpdateKind::CurrentStateEntry;
    }

    static std::uint32_t entity_handle(std::size_t index) noexcept {
        return kEntityHandleBit | static_cast<std::uint32_t>(index);
    }

    static std::uint32_t chat_handle(std::size_t index) noexcept {
        return static_cast<std::uint32_t>(index);
    }

    Block& metadata_block(std::uint32_t handle) noexcept {
        if ((handle & kEntityHandleBit) != 0) {
            return entities[handle & ~kEntityHandleBit].block;
        }
        return chats[handle].block;
    }

    void unlink_metadata(std::uint32_t handle) noexcept {
        auto& block = metadata_block(handle);
        if (!block.linked) {
            return;
        }
        if (block.previous == kNoHandle) {
            metadata_head = block.next;
        } else {
            metadata_block(block.previous).next = block.next;
        }
        if (block.next == kNoHandle) {
            metadata_tail = block.previous;
        } else {
            metadata_block(block.next).previous = block.previous;
        }
        block.linked = false;
        block.previous = kNoHandle;
        block.next = kNoHandle;
    }

    void append_metadata(std::uint32_t handle) noexcept {
        auto& block = metadata_block(handle);
        if (block.size == 0) {
            return;
        }
        block.previous = metadata_tail;
        block.next = kNoHandle;
        block.linked = true;
        if (metadata_tail == kNoHandle) {
            metadata_head = handle;
        } else {
            metadata_block(metadata_tail).next = handle;
        }
        metadata_tail = handle;
    }

    void compact_metadata() noexcept {
        std::uint32_t cursor = metadata_head;
        std::uint32_t destination = 0;
        while (cursor != kNoHandle) {
            auto& block = metadata_block(cursor);
            if (block.offset != destination) {
                std::memmove(metadata_arena.get() + destination,
                             metadata_arena.get() + block.offset, block.size);
                block.offset = destination;
            }
            destination += block.size;
            cursor = block.next;
        }
        metadata_used = destination;
    }

    bool prepare_metadata_block(std::uint32_t handle, std::uint32_t incoming,
                                StreamMetadataPhase failure_phase) noexcept {
        auto& block = metadata_block(handle);
        const auto outgoing = block.size;
        if (outgoing > metadata_used) {
            fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry);
            return false;
        }
        const auto would_use = saturated_add(metadata_used - outgoing, incoming);
        if (would_use > kStreamMetadataBytes) {
            fail_capacity({.resource = StreamMetadataResource::Bytes,
                           .phase = failure_phase,
                           .limit = kStreamMetadataBytes,
                           .would_use = would_use});
            return false;
        }
        if (outgoing == 0 && !block.linked) {
            block.offset = metadata_used;
            block.size = incoming;
            block.previous = kNoHandle;
            block.next = kNoHandle;
            block.linked = false;
            metadata_used += incoming;
            append_metadata(handle);
            return true;
        }
        if (block.linked) {
            unlink_metadata(handle);
            append_metadata(handle);
        }
        compact_metadata();
        if (outgoing != 0) {
            unlink_metadata(handle);
            metadata_used -= outgoing;
        }
        block.offset = static_cast<std::uint32_t>(metadata_used);
        block.size = incoming;
        block.linked = false;
        block.previous = kNoHandle;
        block.next = kNoHandle;
        metadata_used += incoming;
        append_metadata(handle);
        return true;
    }

    std::string_view entity_username(const EntityRecord& entity,
                                     std::size_t ordinal) const noexcept {
        if (ordinal >= entity.username_count) {
            return {};
        }
        const char* cursor = metadata_arena.get() + entity.block.offset;
        for (std::size_t index = 0; index < ordinal; ++index) {
            cursor += std::strlen(cursor) + 1;
        }
        return cursor;
    }

    std::string_view chat_title(const ChatRecord& chat) const noexcept {
        return {metadata_arena.get() + chat.block.offset, chat.title_size};
    }

    std::string_view chat_message_text(const ChatRecord& chat) const noexcept {
        if (!chat.last_message.present) {
            return {};
        }
        return {metadata_arena.get() + chat.block.offset + chat.last_message.text_offset,
                chat.last_message.text_size};
    }

    static bool compatible(const ChatRecord& chat, const EntityRecord& entity) noexcept {
        const auto expected = entity_kind(chat.kind);
        return expected && *expected == entity.kind && entity.id == chat.related_id &&
               (chat.kind != ChatKind::Channel || entity.is_channel) &&
               (chat.kind != ChatKind::Supergroup || !entity.is_channel) &&
               (chat.kind != ChatKind::BasicGroup || !entity.is_bot) &&
               (chat.kind != ChatKind::Private || !entity.is_channel);
    }

    bool validate_chat(const core::TdChat& value) const noexcept {
        const auto kind = chat_kind(value.kind);
        return kind && valid_int53(value.id) && valid_positive_int53(value.related_id) &&
               common::valid_utf8(value.title) && value.chat_lists.size() <= kStreamRawChatLists &&
               std::ranges::all_of(value.chat_lists, valid_chat_list) && value.unread_count >= 0 &&
               value.unread_mention_count >= 0 && value.unread_reaction_count >= 0 &&
               value.unread_poll_vote_count >= 0 &&
               (!value.last_message ||
                (valid_message(*value.last_message) && value.last_message->chat_id == value.id));
    }

    bool sequence(std::uint64_t value) noexcept {
        const auto previous = last_sequence.load(std::memory_order_relaxed);
        if (value == 0 || value <= previous) {
            fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry,
                           core::TdMalformedUpdateReason::InvalidIdentifier);
            return false;
        }
        last_sequence.store(value, std::memory_order_relaxed);
        notify_status(detail::StreamStatusPublishPoint::Sequence);
        return true;
    }

    bool apply_user(const core::TdUserSummary& value, bool emit, StreamMetadataPhase failure_phase,
                    std::uint64_t sequence_value) noexcept;
    bool apply_basic_group(const core::TdBasicGroup& value, bool emit,
                           StreamMetadataPhase failure_phase,
                           std::uint64_t sequence_value) noexcept;
    bool apply_supergroup(const core::TdSupergroup& value, bool emit,
                          StreamMetadataPhase failure_phase, std::uint64_t sequence_value) noexcept;
    bool apply_entity(EntityKind kind, std::int64_t id, bool is_bot, bool is_channel,
                      const std::string* usernames, const char* username_blob,
                      std::size_t username_count, bool emit, StreamMetadataPhase failure_phase,
                      std::uint64_t sequence_value) noexcept;
    bool apply_chat(const core::TdChat& value, bool emit, StreamMetadataPhase failure_phase,
                    std::uint64_t sequence_value) noexcept;
    bool apply_buffered_chat(const Buffered& value, StreamMetadataPhase failure_phase) noexcept;
    bool apply_title(std::int64_t id, std::string_view title, bool emit,
                     StreamMetadataPhase failure_phase, std::uint64_t sequence_value) noexcept;
    bool apply_last_message(std::int64_t id, const core::TdMessageSummary* message,
                            std::string_view message_text, bool emit,
                            StreamMetadataPhase failure_phase,
                            std::uint64_t sequence_value) noexcept;
    template <typename Render>
    bool append_rendered(std::uint64_t sequence_value, StreamRoutingSidecar routing,
                         Render render) noexcept;
    bool append_identity(std::uint64_t sequence_value, const ChatRecord& chat,
                         const EntityRecord& entity) noexcept;
    bool append_new(std::uint64_t sequence_value, const ChatRecord& chat,
                    const EntityRecord& entity) noexcept;
    bool append_frozen(std::uint64_t sequence_value, const ChatRecord& chat) noexcept;
    bool complete_frozen(EntityKind kind, std::int64_t id, const EntityRecord& entity) noexcept;
    bool reserve_candidate(std::uint64_t charge) noexcept;
    void drain() noexcept;
    void compact_order() noexcept;
    void unlink_order(std::uint32_t index) noexcept;
    void append_order(std::uint32_t index) noexcept;
    std::string_view frozen_title(const Candidate& candidate) const noexcept;
    std::string_view frozen_message_text(const Candidate& candidate) const noexcept;
    bool require_known_chat(std::int64_t id, const ChatRecord*& chat,
                            core::TdSupportedUpdateKind kind) noexcept;

    StreamReceiveSink* sink = nullptr;
    std::unique_ptr<ChatRecord[]> chats;
    std::unique_ptr<EntityRecord[]> entities;
    std::unique_ptr<char[]> metadata_arena;
    std::unique_ptr<Buffered[]> bootstrap;
    std::unique_ptr<char[]> bootstrap_arena;
    std::unique_ptr<Candidate[]> candidates;
    std::unique_ptr<char[]> order_arena;
    std::unique_ptr<char[]> scratch;
    detail::StreamStatusPublishProbe status_probe;
    std::size_t chat_count = 0;
    std::size_t entity_count = 0;
    std::uint32_t metadata_used = 0;
    std::uint32_t metadata_head = kNoHandle;
    std::uint32_t metadata_tail = kNoHandle;
    std::size_t bootstrap_count = 0;
    std::uint64_t bootstrap_charged = 0;
    std::uint32_t bootstrap_physical = 0;
    std::size_t candidate_head = 0;
    std::size_t candidate_count = 0;
    std::uint64_t order_charged = 0;
    std::uint32_t order_used = 0;
    std::uint32_t order_head = kNoHandle;
    std::uint32_t order_tail = kNoHandle;

    std::atomic<std::uint64_t> status_revision{0};
    std::atomic<std::int32_t> client_id{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> last_sequence{0};
    std::atomic<StreamNormalizationPhase> phase{StreamNormalizationPhase::Empty};
    std::atomic<bool> ordering_barrier{false};
    std::atomic<StreamFailureKind> failure_kind{StreamFailureKind::None};
    std::atomic<core::TdSupportedUpdateKind> failure_update_kind{
        core::TdSupportedUpdateKind::CurrentStateEntry};
    std::atomic<core::TdMalformedUpdateReason> failure_reason{
        core::TdMalformedUpdateReason::MissingObject};
    std::atomic<std::int32_t> failure_type_id{0};
    std::atomic<std::int32_t> failure_error_code{0};
    std::atomic<std::int32_t> failure_retry_after{0};
    std::atomic<std::uint32_t> failure_index{0};
    std::atomic<StreamMetadataResource> failure_resource{StreamMetadataResource::BootstrapItems};
    std::atomic<StreamMetadataPhase> failure_capacity_phase{StreamMetadataPhase::Bootstrap};
    std::atomic<std::uint64_t> failure_limit{0};
    std::atomic<std::uint64_t> failure_used{0};
    std::atomic<std::uint64_t> failure_incoming{0};
    std::atomic<std::uint64_t> failure_would_use{0};

    std::uint64_t borrow_epoch = 0;
    bool borrow_active = false;

    std::uint64_t begin_borrow() noexcept {
        ++borrow_epoch;
        if (borrow_epoch == 0) {
            ++borrow_epoch;
        }
        borrow_active = true;
        return borrow_epoch;
    }

    void end_borrow(std::uint64_t token) noexcept {
        if (borrow_active && token == borrow_epoch) {
            borrow_active = false;
        }
    }

    [[nodiscard]] bool valid_borrow(std::uint64_t token) const noexcept {
        return borrow_active && token != 0 && token == borrow_epoch;
    }

    void clear_candidates() noexcept;
    bool apply_value(const core::TdValue& value, bool emit, std::uint64_t sequence_value,
                     std::uint32_t state_index = 0) noexcept;
    bool buffer_value(const core::TdValue& value, std::uint64_t sequence_value) noexcept;
    bool apply_buffered(const Buffered& value) noexcept;
    void update(std::int32_t expected_client, std::uint64_t expected_generation,
                const core::TdValue& value) noexcept;
    void current_state(std::int32_t expected_client, std::uint64_t expected_generation,
                       const core::TdValue& value) noexcept;
};

namespace {

std::string_view stored_chat_type(FixedStreamNormalizer::Impl::ChatKind kind) noexcept {
    switch (kind) {
    case FixedStreamNormalizer::Impl::ChatKind::Private:
        return "private";
    case FixedStreamNormalizer::Impl::ChatKind::BasicGroup:
        return "basic_group";
    case FixedStreamNormalizer::Impl::ChatKind::Supergroup:
        return "supergroup";
    case FixedStreamNormalizer::Impl::ChatKind::Channel:
        return "channel";
    case FixedStreamNormalizer::Impl::ChatKind::Secret:
        return {};
    }
    return {};
}

core::TdMessageSummary
stored_message(const FixedStreamNormalizer::Impl::StoredMessage& value) noexcept {
    return {.id = value.id,
            .chat_id = value.chat_id,
            .date = value.date,
            .sender = value.sender,
            .is_outgoing = value.is_outgoing,
            .topic = value.topic,
            .content_kind = value.content_kind,
            .text = {}};
}

} // namespace

void FixedStreamNormalizer::Impl::unlink_order(std::uint32_t index) noexcept {
    auto& block = candidates[index].block;
    if (!block.linked) {
        return;
    }
    if (block.previous == kNoHandle) {
        order_head = block.next;
    } else {
        candidates[block.previous].block.next = block.next;
    }
    if (block.next == kNoHandle) {
        order_tail = block.previous;
    } else {
        candidates[block.next].block.previous = block.previous;
    }
    block.linked = false;
    block.previous = kNoHandle;
    block.next = kNoHandle;
}

void FixedStreamNormalizer::Impl::append_order(std::uint32_t index) noexcept {
    auto& block = candidates[index].block;
    if (block.size == 0) {
        return;
    }
    block.previous = order_tail;
    block.next = kNoHandle;
    block.linked = true;
    if (order_tail == kNoHandle) {
        order_head = index;
    } else {
        candidates[order_tail].block.next = index;
    }
    order_tail = index;
}

void FixedStreamNormalizer::Impl::compact_order() noexcept {
    std::uint32_t cursor = order_head;
    std::uint32_t destination = 0;
    while (cursor != kNoHandle) {
        auto& block = candidates[cursor].block;
        if (block.offset != destination) {
            std::memmove(order_arena.get() + destination, order_arena.get() + block.offset,
                         block.size);
            block.offset = destination;
        }
        destination += block.size;
        cursor = block.next;
    }
    order_used = destination;
}

void FixedStreamNormalizer::Impl::clear_candidates() noexcept {
    for (std::size_t offset = 0; offset < candidate_count; ++offset) {
        const auto index = (candidate_head + offset) % kStreamMetadataOrderItems;
        candidates[index] = {};
    }
    candidate_head = 0;
    candidate_count = 0;
    order_charged = 0;
    order_used = 0;
    order_head = kNoHandle;
    order_tail = kNoHandle;
}

bool FixedStreamNormalizer::Impl::reserve_candidate(std::uint64_t charge) noexcept {
    if (candidate_count == kStreamMetadataOrderItems) {
        fail_capacity({.resource = StreamMetadataResource::OrderItems,
                       .phase = StreamMetadataPhase::Active,
                       .limit = kStreamMetadataOrderItems,
                       .used = candidate_count,
                       .incoming = 1});
        return false;
    }
    const auto would_use = saturated_add(order_charged, charge);
    if (would_use > kStreamMetadataOrderBytes) {
        fail_capacity({.resource = StreamMetadataResource::OrderBytes,
                       .phase = StreamMetadataPhase::Active,
                       .limit = kStreamMetadataOrderBytes,
                       .would_use = would_use});
        return false;
    }
    return true;
}

template <typename Render>
bool FixedStreamNormalizer::Impl::append_rendered(std::uint64_t sequence_value,
                                                  StreamRoutingSidecar routing,
                                                  Render render) noexcept {
    JsonWriter writer({scratch.get(), kStreamMetadataItemBytes});
    if constexpr (std::is_invocable_v<Render, JsonWriter&, StreamRoutingSidecar&>) {
        render(writer, routing);
    } else {
        render(writer);
    }
    writer.character('\n');
    if (!writer.valid()) {
        fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry);
        return false;
    }
    const auto incoming = static_cast<std::uint64_t>(writer.required());
    if (incoming > kStreamMetadataItemBytes) {
        fail_capacity({.resource = StreamMetadataResource::ItemBytes,
                       .phase = StreamMetadataPhase::Active,
                       .limit = kStreamMetadataItemBytes,
                       .incoming = incoming});
        return false;
    }
    if (!reserve_candidate(incoming)) {
        return false;
    }
    compact_order();
    if (incoming > kStreamMetadataOrderBytes - order_used) {
        fail_capacity({.resource = StreamMetadataResource::OrderBytes,
                       .phase = StreamMetadataPhase::Active,
                       .limit = kStreamMetadataOrderBytes,
                       .would_use = saturated_add(order_charged, incoming)});
        return false;
    }
    const auto index =
        static_cast<std::uint32_t>((candidate_head + candidate_count) % kStreamMetadataOrderItems);
    auto& candidate = candidates[index];
    candidate = {};
    candidate.occupied = true;
    candidate.complete = true;
    candidate.sequence = sequence_value;
    candidate.charge = static_cast<std::uint32_t>(incoming);
    routing.json_offset = 0;
    routing.json_size = static_cast<std::uint32_t>(incoming);
    candidate.routing = routing;
    candidate.block.offset = order_used;
    candidate.block.size = static_cast<std::uint32_t>(incoming);
    std::memcpy(order_arena.get() + order_used, scratch.get(), static_cast<std::size_t>(incoming));
    order_used += static_cast<std::uint32_t>(incoming);
    append_order(index);
    order_charged += incoming;
    ++candidate_count;
    ordering_barrier.store(true, std::memory_order_release);
    notify_status(detail::StreamStatusPublishPoint::Barrier);
    return true;
}

bool FixedStreamNormalizer::Impl::append_identity(std::uint64_t sequence_value,
                                                  const ChatRecord& chat,
                                                  const EntityRecord& entity) noexcept {
    return append_rendered(
        sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = chat.id},
        [&](JsonWriter& writer) noexcept {
            writer.raw("{\"event\":\"chat_change\",\"change\":\"identity\",\"chat\":{");
            writer.raw("\"id\":");
            writer.integer(chat.id);
            writer.raw(",\"title\":");
            writer.quoted(chat_title(chat));
            writer.raw(",\"type\":");
            writer.quoted(stored_chat_type(chat.kind));
            writer.raw(",\"is_bot\":");
            writer.boolean(chat.kind == ChatKind::Private && entity.is_bot);
            writer.raw(",\"usernames\":[");
            for (std::size_t index = 0; index < entity.username_count; ++index) {
                if (index != 0) {
                    writer.character(',');
                }
                writer.quoted(entity_username(entity, index));
            }
            writer.raw("]}}");
        });
}

template <typename ChatValue, typename Title, typename MessageText>
void write_summary(JsonWriter& writer, const ChatValue& chat,
                   const FixedStreamNormalizer::Impl::EntityRecord& entity, Title title,
                   MessageText message_text, const FixedStreamNormalizer::Impl* owner) noexcept {
    writer.raw("{\"id\":");
    writer.integer(chat.id);
    writer.raw(",\"title\":");
    writer.quoted(title());
    writer.raw(",\"type\":");
    writer.quoted(stored_chat_type(chat.kind));
    writer.raw(",\"is_bot\":");
    writer.boolean(chat.kind == FixedStreamNormalizer::Impl::ChatKind::Private && entity.is_bot);
    writer.raw(",\"usernames\":[");
    for (std::size_t index = 0; index < entity.username_count; ++index) {
        if (index != 0) {
            writer.character(',');
        }
        writer.quoted(owner->entity_username(entity, index));
    }
    writer.raw("],\"is_archived\":");
    bool archived = false;
    std::array<std::int32_t, kStreamRawChatLists> folders{};
    std::size_t folder_count = 0;
    for (std::size_t index = 0; index < chat.list_count; ++index) {
        archived = archived || chat.lists[index].kind == core::TdChatListKind::Archive;
        if (chat.lists[index].kind == core::TdChatListKind::Folder) {
            folders[folder_count++] = chat.lists[index].folder_id;
        }
    }
    std::sort(folders.begin(), folders.begin() + static_cast<std::ptrdiff_t>(folder_count));
    folder_count = static_cast<std::size_t>(
        std::unique(folders.begin(), folders.begin() + static_cast<std::ptrdiff_t>(folder_count)) -
        folders.begin());
    writer.boolean(archived);
    writer.raw(",\"folder_ids\":[");
    for (std::size_t index = 0; index < folder_count; ++index) {
        if (index != 0) {
            writer.character(',');
        }
        writer.integer(folders[index]);
    }
    writer.raw("],\"is_marked_unread\":");
    writer.boolean(chat.is_marked_unread);
    writer.raw(",\"unread_count\":");
    writer.integer(chat.unread_count);
    writer.raw(",\"unread_mention_count\":");
    writer.integer(chat.unread_mention_count);
    writer.raw(",\"unread_reaction_count\":");
    writer.integer(chat.unread_reaction_count);
    writer.raw(",\"unread_poll_vote_count\":");
    writer.integer(chat.unread_poll_vote_count);
    writer.raw(",\"last_message\":");
    if (!chat.last_message.present) {
        writer.raw("null");
    } else {
        write_message(writer, stored_message(chat.last_message), message_text());
    }
    writer.character('}');
}

bool FixedStreamNormalizer::Impl::append_new(std::uint64_t sequence_value, const ChatRecord& chat,
                                             const EntityRecord& entity) noexcept {
    return append_rendered(
        sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = chat.id},
        [&](JsonWriter& writer) noexcept {
            writer.raw("{\"event\":\"chat_change\",\"change\":\"new\",\"chat\":");
            write_summary(
                writer, chat, entity, [&] { return chat_title(chat); },
                [&] { return chat_message_text(chat); }, this);
            writer.character('}');
        });
}

std::string_view
FixedStreamNormalizer::Impl::frozen_title(const Candidate& candidate) const noexcept {
    return {order_arena.get() + candidate.block.offset, candidate.frozen.title_size};
}

std::string_view
FixedStreamNormalizer::Impl::frozen_message_text(const Candidate& candidate) const noexcept {
    if (!candidate.frozen.last_message.present) {
        return {};
    }
    return {order_arena.get() + candidate.block.offset + candidate.frozen.last_message.text_offset,
            candidate.frozen.last_message.text_size};
}

bool FixedStreamNormalizer::Impl::append_frozen(std::uint64_t sequence_value,
                                                const ChatRecord& chat) noexcept {
    if (!reserve_candidate(kStreamMetadataItemBytes)) {
        return false;
    }
    const auto physical = chat.block.size;
    if (physical > kStreamMetadataItemBytes) {
        fail_capacity({.resource = StreamMetadataResource::ItemBytes,
                       .phase = StreamMetadataPhase::Active,
                       .limit = kStreamMetadataItemBytes,
                       .incoming = static_cast<std::uint64_t>(physical) + 1});
        return false;
    }
    compact_order();
    if (physical > kStreamMetadataOrderBytes - order_used) {
        fail_capacity({.resource = StreamMetadataResource::OrderBytes,
                       .phase = StreamMetadataPhase::Active,
                       .limit = kStreamMetadataOrderBytes,
                       .would_use = saturated_add(order_charged, kStreamMetadataItemBytes)});
        return false;
    }
    const auto index =
        static_cast<std::uint32_t>((candidate_head + candidate_count) % kStreamMetadataOrderItems);
    auto& candidate = candidates[index];
    candidate = {};
    candidate.occupied = true;
    candidate.complete = false;
    candidate.sequence = sequence_value;
    candidate.charge = kStreamMetadataItemBytes;
    candidate.routing = {.event_class = StreamEventClass::Chat, .chat_id = chat.id};
    candidate.block.offset = order_used;
    candidate.block.size = physical;
    if (physical != 0) {
        std::memcpy(order_arena.get() + order_used, metadata_arena.get() + chat.block.offset,
                    physical);
    }
    order_used += physical;
    append_order(index);
    candidate.frozen = {.id = chat.id,
                        .kind = chat.kind,
                        .related_id = chat.related_id,
                        .title_size = chat.title_size,
                        .lists = chat.lists,
                        .list_count = chat.list_count,
                        .is_marked_unread = chat.is_marked_unread,
                        .unread_count = chat.unread_count,
                        .unread_mention_count = chat.unread_mention_count,
                        .unread_reaction_count = chat.unread_reaction_count,
                        .unread_poll_vote_count = chat.unread_poll_vote_count,
                        .last_message = chat.last_message};
    order_charged += kStreamMetadataItemBytes;
    ++candidate_count;
    ordering_barrier.store(true, std::memory_order_release);
    notify_status(detail::StreamStatusPublishPoint::Barrier);
    return true;
}

bool FixedStreamNormalizer::Impl::complete_frozen(EntityKind kind, std::int64_t id,
                                                  const EntityRecord& entity) noexcept {
    for (std::size_t offset = 0; offset < candidate_count; ++offset) {
        const auto index =
            static_cast<std::uint32_t>((candidate_head + offset) % kStreamMetadataOrderItems);
        auto& candidate = candidates[index];
        if (candidate.complete || candidate.frozen.related_id != id ||
            entity_kind(candidate.frozen.kind) != kind) {
            continue;
        }
        if ((candidate.frozen.kind == ChatKind::Channel && !entity.is_channel) ||
            (candidate.frozen.kind == ChatKind::Supergroup && entity.is_channel)) {
            fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry);
            return false;
        }
        JsonWriter writer({scratch.get(), kStreamMetadataItemBytes});
        writer.raw("{\"event\":\"chat_change\",\"change\":\"new\",\"chat\":");
        write_summary(
            writer, candidate.frozen, entity, [&] { return frozen_title(candidate); },
            [&] { return frozen_message_text(candidate); }, this);
        writer.raw("}\n");
        if (!writer.valid()) {
            fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry);
            return false;
        }
        const auto incoming = writer.required();
        if (incoming > kStreamMetadataItemBytes) {
            fail_capacity({.resource = StreamMetadataResource::ItemBytes,
                           .phase = StreamMetadataPhase::Active,
                           .limit = kStreamMetadataItemBytes,
                           .incoming = incoming});
            return false;
        }
        unlink_order(index);
        compact_order();
        candidate.block.offset = order_used;
        candidate.block.size = static_cast<std::uint32_t>(incoming);
        std::memcpy(order_arena.get() + order_used, scratch.get(), incoming);
        order_used += static_cast<std::uint32_t>(incoming);
        append_order(index);
        order_charged -= candidate.charge;
        candidate.charge = static_cast<std::uint32_t>(incoming);
        order_charged += incoming;
        candidate.complete = true;
    }
    return true;
}

void FixedStreamNormalizer::Impl::drain() noexcept {
    while (candidate_count != 0) {
        auto& candidate = candidates[candidate_head];
        if (!candidate.complete) {
            break;
        }
        if (sink != nullptr) {
            const auto borrow_token = begin_borrow();
            const auto first_size = static_cast<std::size_t>(candidate.block.size) / 2U;
            const StreamItemView item({order_arena.get() + candidate.block.offset, first_size},
                                      {order_arena.get() + candidate.block.offset + first_size,
                                       static_cast<std::size_t>(candidate.block.size) - first_size},
                                      candidate.sequence, candidate.routing);
            const StreamMetadataView metadata(this, borrow_token);
            sink->on_item(item, metadata);
            std::memset(order_arena.get() + candidate.block.offset, kBorrowPoison,
                        candidate.block.size);
        }
        order_charged -= candidate.charge;
        unlink_order(static_cast<std::uint32_t>(candidate_head));
        candidate = {};
        candidate_head = (candidate_head + 1) % kStreamMetadataOrderItems;
        --candidate_count;
    }
    compact_order();
    if (candidate_count == 0) {
        candidate_head = 0;
        order_head = kNoHandle;
        order_tail = kNoHandle;
        order_used = 0;
        ordering_barrier.store(false, std::memory_order_release);
        notify_status(detail::StreamStatusPublishPoint::Barrier);
    }
}

bool FixedStreamNormalizer::Impl::apply_entity(EntityKind kind, std::int64_t id, bool is_bot,
                                               bool is_channel, const std::string* usernames,
                                               const char* username_blob,
                                               std::size_t username_count, bool emit,
                                               StreamMetadataPhase failure_phase,
                                               std::uint64_t sequence_value) noexcept {
    if (!valid_positive_int53(id) || (kind != EntityKind::User && is_bot) ||
        (kind != EntityKind::Supergroup && is_channel)) {
        fail_malformed(entity_update_kind(kind));
        return false;
    }
    std::uint64_t incoming = 0;
    const auto username_at = [&](std::size_t index) -> std::string_view {
        if (usernames != nullptr) {
            return usernames[index];
        }
        const char* cursor = username_blob;
        for (std::size_t current = 0; current < index; ++current) {
            cursor += std::strlen(cursor) + 1;
        }
        return cursor;
    };
    for (std::size_t index = 0; index < username_count; ++index) {
        const auto username = username_at(index);
        if (!valid_username(username)) {
            fail_malformed(entity_update_kind(kind));
            return false;
        }
        incoming = saturated_add(incoming, username.size() + 1U);
    }
    if (incoming > std::numeric_limits<std::uint32_t>::max()) {
        fail_capacity({.resource = StreamMetadataResource::Bytes,
                       .phase = failure_phase,
                       .limit = kStreamMetadataBytes,
                       .would_use = incoming});
        return false;
    }
    bool found = false;
    const auto slot = entity_slot(kind, id, found);
    if (!found && entity_count == kStreamMetadataEntities) {
        fail_capacity({.resource = StreamMetadataResource::Entities,
                       .phase = failure_phase,
                       .limit = kStreamMetadataEntities,
                       .used = entity_count,
                       .incoming = 1});
        return false;
    }
    if (slot == kStreamMetadataEntities) {
        fail_capacity({.resource = StreamMetadataResource::Entities,
                       .phase = failure_phase,
                       .limit = kStreamMetadataEntities,
                       .used = entity_count,
                       .incoming = 1});
        return false;
    }

    ChatRecord* related = nullptr;
    if (chat_count != 0) {
        for (std::size_t index = 0; index < kStreamMetadataChats; ++index) {
            auto& candidate = chats[index];
            if (!candidate.occupied || candidate.kind == ChatKind::Secret ||
                candidate.related_id != id || entity_kind(candidate.kind) != kind) {
                continue;
            }
            if (related != nullptr) {
                fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry);
                return false;
            }
            related = &candidate;
        }
    }

    const auto previous_bot = found && entities[slot].is_bot;
    const auto previous_channel = found && entities[slot].is_channel;
    const auto previous_count = found ? entities[slot].username_count : 0;
    bool changed = !found || previous_bot != is_bot || previous_channel != is_channel ||
                   previous_count != username_count;
    if (found && !changed) {
        for (std::size_t index = 0; index < username_count; ++index) {
            if (entity_username(entities[slot], index) != username_at(index)) {
                changed = true;
                break;
            }
        }
    }
    const EntityRecord prospective{.occupied = true,
                                   .kind = kind,
                                   .id = id,
                                   .is_bot = is_bot,
                                   .is_channel = is_channel,
                                   .username_count = static_cast<std::uint32_t>(username_count),
                                   .block = entities[slot].block};
    if (related != nullptr && !compatible(*related, prospective)) {
        fail_malformed(entity_update_kind(kind));
        return false;
    }
    const auto handle = entity_handle(slot);
    if (!prepare_metadata_block(handle, static_cast<std::uint32_t>(incoming), failure_phase)) {
        return false;
    }
    auto block = entities[slot].block;
    entities[slot] = prospective;
    entities[slot].block = block;
    char* destination = metadata_arena.get() + block.offset;
    for (std::size_t index = 0; index < username_count; ++index) {
        const auto username = username_at(index);
        std::memcpy(destination, username.data(), username.size());
        destination += username.size();
        *destination++ = '\0';
    }
    if (!found) {
        ++entity_count;
    }
    if (emit && !complete_frozen(kind, id, entities[slot])) {
        return false;
    }
    if (emit && related != nullptr && changed &&
        !append_identity(sequence_value, *related, entities[slot])) {
        return false;
    }
    return true;
}

bool FixedStreamNormalizer::Impl::apply_user(const core::TdUserSummary& value, bool emit,
                                             StreamMetadataPhase failure_phase,
                                             std::uint64_t sequence_value) noexcept {
    return apply_entity(EntityKind::User, value.id, value.is_bot, false, value.usernames.data(),
                        nullptr, value.usernames.size(), emit, failure_phase, sequence_value);
}

bool FixedStreamNormalizer::Impl::apply_basic_group(const core::TdBasicGroup& value, bool emit,
                                                    StreamMetadataPhase failure_phase,
                                                    std::uint64_t sequence_value) noexcept {
    if (value.member_count < 0 || value.upgraded_to_supergroup_id < 0 ||
        value.upgraded_to_supergroup_id > kMaximumInt53) {
        fail_malformed(core::TdSupportedUpdateKind::BasicGroup);
        return false;
    }
    return apply_entity(EntityKind::BasicGroup, value.id, false, false, nullptr, nullptr, 0, emit,
                        failure_phase, sequence_value);
}

bool FixedStreamNormalizer::Impl::apply_supergroup(const core::TdSupergroup& value, bool emit,
                                                   StreamMetadataPhase failure_phase,
                                                   std::uint64_t sequence_value) noexcept {
    return apply_entity(EntityKind::Supergroup, value.id, false, value.is_channel,
                        value.usernames.data(), nullptr, value.usernames.size(), emit,
                        failure_phase, sequence_value);
}

bool FixedStreamNormalizer::Impl::apply_chat(const core::TdChat& value, bool emit,
                                             StreamMetadataPhase failure_phase,
                                             std::uint64_t sequence_value) noexcept {
    const auto normalized = chat_kind(value.kind);
    if (!normalized || !validate_chat(value)) {
        fail_malformed(core::TdSupportedUpdateKind::NewChat);
        return false;
    }
    const auto normalized_kind = *normalized;
    bool found = false;
    const auto slot = chat_slot(value.id, found);
    if (!found && chat_count == kStreamMetadataChats) {
        fail_capacity({.resource = StreamMetadataResource::Chats,
                       .phase = failure_phase,
                       .limit = kStreamMetadataChats,
                       .used = chat_count,
                       .incoming = 1});
        return false;
    }
    if (slot == kStreamMetadataChats) {
        fail_capacity({.resource = StreamMetadataResource::Chats,
                       .phase = failure_phase,
                       .limit = kStreamMetadataChats,
                       .used = chat_count,
                       .incoming = 1});
        return false;
    }
    if (found &&
        (chats[slot].kind != normalized_kind || chats[slot].related_id != value.related_id)) {
        fail_malformed(core::TdSupportedUpdateKind::NewChat);
        return false;
    }
    if (normalized_kind != ChatKind::Secret) {
        for (std::size_t index = 0; index < kStreamMetadataChats; ++index) {
            const auto& other = chats[index];
            if (index != slot && other.occupied && other.kind != ChatKind::Secret &&
                other.related_id == value.related_id &&
                entity_kind(other.kind) == entity_kind(normalized_kind)) {
                fail_malformed(core::TdSupportedUpdateKind::NewChat);
                return false;
            }
        }
    }
    auto incoming = saturated_add(value.title.size(), 1);
    if (value.last_message) {
        incoming = saturated_add(incoming, message_string_charge(*value.last_message, true));
    }
    if (incoming > std::numeric_limits<std::uint32_t>::max()) {
        fail_capacity({.resource = StreamMetadataResource::Bytes,
                       .phase = failure_phase,
                       .limit = kStreamMetadataBytes,
                       .would_use = incoming});
        return false;
    }
    const auto handle = chat_handle(slot);
    if (!prepare_metadata_block(handle, static_cast<std::uint32_t>(incoming), failure_phase)) {
        return false;
    }
    auto block = chats[slot].block;
    ChatRecord record{.occupied = true,
                      .id = value.id,
                      .kind = normalized_kind,
                      .related_id = value.related_id,
                      .title_size = static_cast<std::uint32_t>(value.title.size()),
                      .lists = {},
                      .list_count = static_cast<std::uint32_t>(value.chat_lists.size()),
                      .is_marked_unread = value.is_marked_unread,
                      .unread_count = value.unread_count,
                      .unread_mention_count = value.unread_mention_count,
                      .unread_reaction_count = value.unread_reaction_count,
                      .unread_poll_vote_count = value.unread_poll_vote_count,
                      .last_message = {},
                      .block = block};
    std::copy(value.chat_lists.begin(), value.chat_lists.end(), record.lists.begin());
    char* destination = metadata_arena.get() + block.offset;
    std::memcpy(destination, value.title.data(), value.title.size());
    destination[value.title.size()] = '\0';
    if (value.last_message) {
        const auto& message = *value.last_message;
        record.last_message = {.present = true,
                               .id = message.id,
                               .chat_id = message.chat_id,
                               .date = message.date,
                               .sender = message.sender,
                               .is_outgoing = message.is_outgoing,
                               .topic = message.topic,
                               .content_kind = message.content_kind,
                               .text_offset = static_cast<std::uint32_t>(value.title.size() + 1),
                               .text_size = static_cast<std::uint32_t>(message.text.size())};
        destination += value.title.size() + 1;
        std::memcpy(destination, message.text.data(), message.text.size());
        destination += message.text.size();
        *destination++ = '\0';
        if (message.date != 0) {
            std::array<char, 20> date{};
            if (!stream_timestamp_utc(message.date, date)) {
                fail_malformed(core::TdSupportedUpdateKind::NewChat,
                               core::TdMalformedUpdateReason::InvalidDate);
                return false;
            }
            std::memcpy(destination, date.data(), date.size());
            destination[date.size()] = '\0';
        }
    }
    chats[slot] = record;
    if (!found) {
        ++chat_count;
    }
    if (normalized_kind == ChatKind::Secret || !emit) {
        return true;
    }
    const auto expected_entity = entity_kind(normalized_kind);
    const auto* entity = find_entity(*expected_entity, value.related_id);
    if (entity == nullptr) {
        return append_frozen(sequence_value, chats[slot]);
    }
    if (!compatible(chats[slot], *entity)) {
        fail_malformed(core::TdSupportedUpdateKind::NewChat);
        return false;
    }
    return append_new(sequence_value, chats[slot], *entity);
}

bool FixedStreamNormalizer::Impl::apply_buffered_chat(const Buffered& value,
                                                      StreamMetadataPhase failure_phase) noexcept {
    const std::string_view title(bootstrap_arena.get() + value.blob_offset, value.title_size);
    if (!valid_int53(value.id) || !valid_positive_int53(value.related_id) ||
        !common::valid_utf8(title) || value.list_count > kStreamRawChatLists ||
        !std::all_of(value.lists.begin(), value.lists.begin() + value.list_count,
                     valid_chat_list) ||
        value.first_count < 0 || value.second_count < 0 || value.third_count < 0 ||
        value.fourth_count < 0) {
        fail_malformed(core::TdSupportedUpdateKind::NewChat);
        return false;
    }
    bool found = false;
    const auto slot = chat_slot(value.id, found);
    if ((!found && chat_count == kStreamMetadataChats) || slot == kStreamMetadataChats) {
        fail_capacity({.resource = StreamMetadataResource::Chats,
                       .phase = failure_phase,
                       .limit = kStreamMetadataChats,
                       .used = chat_count,
                       .incoming = 1});
        return false;
    }
    if (found &&
        (chats[slot].kind != value.chat_kind || chats[slot].related_id != value.related_id)) {
        fail_malformed(core::TdSupportedUpdateKind::NewChat);
        return false;
    }
    if (value.chat_kind != ChatKind::Secret) {
        for (std::size_t index = 0; index < kStreamMetadataChats; ++index) {
            const auto& other = chats[index];
            if (index != slot && other.occupied && other.kind != ChatKind::Secret &&
                other.related_id == value.related_id &&
                entity_kind(other.kind) == entity_kind(value.chat_kind)) {
                fail_malformed(core::TdSupportedUpdateKind::NewChat);
                return false;
            }
        }
    }
    std::uint64_t incoming = title.size() + 1U;
    if (value.message.present) {
        incoming = saturated_add(incoming, value.message.text_size + 1U +
                                               (value.message.date != 0 ? 21U : 0U));
    }
    if (!prepare_metadata_block(chat_handle(slot), static_cast<std::uint32_t>(incoming),
                                failure_phase)) {
        return false;
    }
    auto block = chats[slot].block;
    ChatRecord record{.occupied = true,
                      .id = value.id,
                      .kind = value.chat_kind,
                      .related_id = value.related_id,
                      .title_size = value.title_size,
                      .lists = value.lists,
                      .list_count = value.list_count,
                      .is_marked_unread = value.first_bool,
                      .unread_count = value.first_count,
                      .unread_mention_count = value.second_count,
                      .unread_reaction_count = value.third_count,
                      .unread_poll_vote_count = value.fourth_count,
                      .last_message = value.message,
                      .block = block};
    char* destination = metadata_arena.get() + block.offset;
    std::memcpy(destination, title.data(), title.size());
    destination += title.size();
    *destination++ = '\0';
    if (value.message.present) {
        const std::string_view text(bootstrap_arena.get() + value.blob_offset +
                                        value.message.text_offset,
                                    value.message.text_size);
        record.last_message.text_offset = value.title_size + 1;
        std::memcpy(destination, text.data(), text.size());
        destination += text.size();
        *destination++ = '\0';
        if (value.message.date != 0) {
            std::array<char, 20> date{};
            if (!stream_timestamp_utc(value.message.date, date)) {
                fail_malformed(core::TdSupportedUpdateKind::NewChat,
                               core::TdMalformedUpdateReason::InvalidDate);
                return false;
            }
            std::memcpy(destination, date.data(), date.size());
            destination[date.size()] = '\0';
        }
    }
    chats[slot] = record;
    if (!found) {
        ++chat_count;
    }
    return true;
}

bool FixedStreamNormalizer::Impl::apply_title(std::int64_t id, std::string_view title, bool emit,
                                              StreamMetadataPhase failure_phase,
                                              std::uint64_t sequence_value) noexcept {
    auto* chat = find_chat(id);
    if (chat == nullptr || !valid_int53(id) || !common::valid_utf8(title)) {
        fail_malformed(core::TdSupportedUpdateKind::ChatTitle);
        return false;
    }
    const auto old_text_offset = chat->last_message.text_offset;
    const auto old_text_size = chat->last_message.text_size;
    auto incoming = saturated_add(title.size(), 1);
    if (chat->last_message.present) {
        incoming =
            saturated_add(incoming, old_text_size + 1U + (chat->last_message.date != 0 ? 21U : 0U));
    }
    const auto slot = static_cast<std::size_t>(chat - chats.get());
    if (!prepare_metadata_block(chat_handle(slot), static_cast<std::uint32_t>(incoming),
                                failure_phase)) {
        return false;
    }
    chat = &chats[slot];
    char* base = metadata_arena.get() + chat->block.offset;
    if (chat->last_message.present) {
        std::memmove(base + title.size() + 1, base + old_text_offset, old_text_size);
        base[title.size() + 1 + old_text_size] = '\0';
        chat->last_message.text_offset = static_cast<std::uint32_t>(title.size() + 1);
        if (chat->last_message.date != 0) {
            std::array<char, 20> date{};
            if (!stream_timestamp_utc(chat->last_message.date, date)) {
                fail_malformed(core::TdSupportedUpdateKind::ChatTitle);
                return false;
            }
            auto* date_destination = base + title.size() + 2 + old_text_size;
            std::memcpy(date_destination, date.data(), date.size());
            date_destination[date.size()] = '\0';
        }
    }
    std::memcpy(base, title.data(), title.size());
    base[title.size()] = '\0';
    chat->title_size = static_cast<std::uint32_t>(title.size());
    if (!emit || chat->kind == ChatKind::Secret) {
        return true;
    }
    return append_rendered(sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = id},
                           [&](JsonWriter& writer) noexcept {
                               writer.raw(
                                   "{\"event\":\"chat_change\",\"change\":\"title\",\"chat_id\":");
                               writer.integer(id);
                               writer.raw(",\"title\":");
                               writer.quoted(title);
                               writer.character('}');
                           });
}

bool FixedStreamNormalizer::Impl::apply_last_message(std::int64_t id,
                                                     const core::TdMessageSummary* message,
                                                     std::string_view message_text, bool emit,
                                                     StreamMetadataPhase failure_phase,
                                                     std::uint64_t sequence_value) noexcept {
    auto* chat = find_chat(id);
    if (chat == nullptr || !valid_int53(id) ||
        (message != nullptr &&
         ((!valid_message(*message) &&
           !(message->text.empty() && common::valid_utf8(message_text) &&
             valid_int53(message->id) && valid_int53(message->chat_id) && message->date >= 0 &&
             valid_sender(message->sender) && (!message->topic || valid_topic(*message->topic)))) ||
          message->chat_id != id))) {
        fail_malformed(core::TdSupportedUpdateKind::ChatLastMessage);
        return false;
    }
    auto incoming = static_cast<std::uint64_t>(chat->title_size) + 1U;
    if (message != nullptr) {
        incoming =
            saturated_add(incoming, message_text.size() + 1U + (message->date != 0 ? 21U : 0U));
    }
    const auto slot = static_cast<std::size_t>(chat - chats.get());
    if (!prepare_metadata_block(chat_handle(slot), static_cast<std::uint32_t>(incoming),
                                failure_phase)) {
        return false;
    }
    chat = &chats[slot];
    char* base = metadata_arena.get() + chat->block.offset;
    chat->last_message = {};
    if (message != nullptr) {
        chat->last_message = {.present = true,
                              .id = message->id,
                              .chat_id = message->chat_id,
                              .date = message->date,
                              .sender = message->sender,
                              .is_outgoing = message->is_outgoing,
                              .topic = message->topic,
                              .content_kind = message->content_kind,
                              .text_offset = chat->title_size + 1,
                              .text_size = static_cast<std::uint32_t>(message_text.size())};
        char* destination = base + chat->title_size + 1;
        std::memcpy(destination, message_text.data(), message_text.size());
        destination += message_text.size();
        *destination++ = '\0';
        if (message->date != 0) {
            std::array<char, 20> date{};
            if (!stream_timestamp_utc(message->date, date)) {
                fail_malformed(core::TdSupportedUpdateKind::ChatLastMessage);
                return false;
            }
            std::memcpy(destination, date.data(), date.size());
            destination[date.size()] = '\0';
        }
    }
    if (!emit || chat->kind == ChatKind::Secret) {
        return true;
    }
    return append_rendered(
        sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = id},
        [&](JsonWriter& writer) noexcept {
            writer.raw("{\"event\":\"chat_change\",\"change\":\"last_message\",\"chat_id\":");
            writer.integer(id);
            writer.raw(",\"last_message\":");
            if (message == nullptr) {
                writer.raw("null");
            } else {
                write_message(writer, *message, message_text);
            }
            writer.character('}');
        });
}

bool FixedStreamNormalizer::Impl::require_known_chat(std::int64_t id, const ChatRecord*& chat,
                                                     core::TdSupportedUpdateKind kind) noexcept {
    if (!valid_int53(id)) {
        fail_malformed(kind, core::TdMalformedUpdateReason::InvalidIdentifier);
        return false;
    }
    chat = find_chat(id);
    if (chat == nullptr) {
        fail_malformed(kind, core::TdMalformedUpdateReason::InvalidEntity);
        return false;
    }
    return true;
}

namespace {

void write_chat_list(JsonWriter& writer, const core::TdChatList& list) noexcept {
    switch (list.kind) {
    case core::TdChatListKind::Main:
        writer.raw("{\"type\":\"main\"}");
        return;
    case core::TdChatListKind::Archive:
        writer.raw("{\"type\":\"archive\"}");
        return;
    case core::TdChatListKind::Folder:
        writer.raw("{\"type\":\"folder\",\"folder_id\":");
        writer.integer(list.folder_id);
        writer.character('}');
        return;
    case core::TdChatListKind::Unknown:
        return;
    }
}

void write_reaction_array(JsonWriter& writer,
                          const std::vector<core::TdReactionType>& reactions) noexcept {
    writer.character('[');
    for (std::size_t index = 0; index < reactions.size(); ++index) {
        if (index != 0) {
            writer.character(',');
        }
        write_reaction(writer, reactions[index]);
    }
    writer.character(']');
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed supported update union.
bool FixedStreamNormalizer::Impl::apply_value(const core::TdValue& value, bool emit,
                                              std::uint64_t sequence_value,
                                              std::uint32_t state_index) noexcept {
    const auto failure_phase = emit ? StreamMetadataPhase::Active : StreamMetadataPhase::Bootstrap;
    if (const auto* malformed = value.get_if<core::TdMalformedSupportedUpdate>()) {
        fail_malformed(malformed->kind, malformed->reason, malformed->tdlib_type_id, state_index);
        return false;
    }
    if (const auto* direct = value.get_if<core::TdDirectConversionError>()) {
        failure_type_id.store(direct->tdlib_type_id.value_or(0), std::memory_order_relaxed);
        failure_index.store(state_index, std::memory_order_relaxed);
        fail(StreamFailureKind::DirectConversion);
        return false;
    }
    if (const auto* error = value.get_if<core::TdError>()) {
        failure_error_code.store(error->code, std::memory_order_relaxed);
        failure_retry_after.store(error->code == 429 ? stream_retry_after(error->message) : 0,
                                  std::memory_order_relaxed);
        failure_index.store(state_index, std::memory_order_relaxed);
        fail(error->code == 429 ? StreamFailureKind::RateLimited : StreamFailureKind::TdlibError);
        return false;
    }
    if (const auto* update = value.get_if<core::TdUpdateUser>()) {
        return apply_user(update->user, emit, failure_phase, sequence_value);
    }
    if (const auto* update = value.get_if<core::TdUpdateBasicGroup>()) {
        return apply_basic_group(update->basic_group, emit, failure_phase, sequence_value);
    }
    if (const auto* update = value.get_if<core::TdUpdateSupergroup>()) {
        return apply_supergroup(update->supergroup, emit, failure_phase, sequence_value);
    }
    if (const auto* update = value.get_if<core::TdUpdateNewChat>()) {
        return apply_chat(update->chat, emit, failure_phase, sequence_value);
    }
    if (const auto* update = value.get_if<core::TdUpdateChatTitle>()) {
        return apply_title(update->chat_id, update->title, emit, failure_phase, sequence_value);
    }
    if (const auto* update = value.get_if<core::TdUpdateChatLastMessage>()) {
        return apply_last_message(
            update->chat_id, update->last_message ? &*update->last_message : nullptr,
            update->last_message ? std::string_view(update->last_message->text)
                                 : std::string_view{},
            emit, failure_phase, sequence_value);
    }

    auto metadata_chat = [&](std::int64_t id, core::TdSupportedUpdateKind kind) -> ChatRecord* {
        if (!valid_int53(id)) {
            fail_malformed(kind, core::TdMalformedUpdateReason::InvalidIdentifier);
            return nullptr;
        }
        auto* found = find_chat(id);
        if (found == nullptr) {
            fail_malformed(kind, core::TdMalformedUpdateReason::InvalidEntity);
        }
        return found;
    };
    if (const auto* update = value.get_if<core::TdUpdateChatAddedToList>()) {
        auto* chat = metadata_chat(update->chat_id, core::TdSupportedUpdateKind::ChatAddedToList);
        if (chat == nullptr || !valid_chat_list(update->list)) {
            if (chat != nullptr) {
                fail_malformed(core::TdSupportedUpdateKind::ChatAddedToList,
                               core::TdMalformedUpdateReason::InvalidChatList);
            }
            return false;
        }
        const bool present =
            std::any_of(chat->lists.begin(), chat->lists.begin() + chat->list_count,
                        [&](const auto& item) { return same_chat_list(item, update->list); });
        if (!present) {
            if (chat->list_count == kStreamRawChatLists) {
                fail_malformed(core::TdSupportedUpdateKind::ChatAddedToList,
                               core::TdMalformedUpdateReason::InvalidChatList);
                return false;
            }
            chat->lists[chat->list_count++] = update->list;
        }
        if (!emit || chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"chat_change\",\"change\":\"list_added\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"list\":");
                write_chat_list(writer, update->list);
                writer.character('}');
            });
    }
    if (const auto* update = value.get_if<core::TdUpdateChatRemovedFromList>()) {
        auto* chat =
            metadata_chat(update->chat_id, core::TdSupportedUpdateKind::ChatRemovedFromList);
        if (chat == nullptr || !valid_chat_list(update->list)) {
            if (chat != nullptr) {
                fail_malformed(core::TdSupportedUpdateKind::ChatRemovedFromList,
                               core::TdMalformedUpdateReason::InvalidChatList);
            }
            return false;
        }
        std::size_t destination = 0;
        for (std::size_t index = 0; index < chat->list_count; ++index) {
            if (!same_chat_list(chat->lists[index], update->list)) {
                chat->lists[destination++] = chat->lists[index];
            }
        }
        chat->list_count = static_cast<std::uint32_t>(destination);
        if (!emit || chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"chat_change\",\"change\":\"list_removed\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"list\":");
                write_chat_list(writer, update->list);
                writer.character('}');
            });
    }

    auto counter_event = [&](std::int64_t id, std::int32_t count, core::TdSupportedUpdateKind kind,
                             std::string_view change, std::string_view field,
                             std::int32_t ChatRecord::*member) {
        auto* chat = metadata_chat(id, kind);
        if (chat == nullptr || count < 0) {
            if (chat != nullptr) {
                fail_malformed(kind, core::TdMalformedUpdateReason::InvalidCount);
            }
            return false;
        }
        chat->*member = count;
        if (!emit || chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(sequence_value,
                               {.event_class = StreamEventClass::Chat, .chat_id = id},
                               [&](JsonWriter& writer) noexcept {
                                   writer.raw("{\"event\":\"chat_change\",\"change\":");
                                   writer.quoted(change);
                                   writer.raw(",\"chat_id\":");
                                   writer.integer(id);
                                   writer.raw(",");
                                   writer.quoted(field);
                                   writer.character(':');
                                   writer.integer(count);
                                   writer.character('}');
                               });
    };
    if (const auto* update = value.get_if<core::TdUpdateChatReadInbox>()) {
        auto* chat = metadata_chat(update->chat_id, core::TdSupportedUpdateKind::ChatReadInbox);
        if (chat == nullptr || !valid_nonnegative_int53(update->last_read_inbox_message_id) ||
            update->unread_count < 0) {
            if (chat != nullptr) {
                fail_malformed(core::TdSupportedUpdateKind::ChatReadInbox,
                               core::TdMalformedUpdateReason::InvalidCount);
            }
            return false;
        }
        chat->unread_count = update->unread_count;
        if (!emit || chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"chat_change\",\"change\":\"read_inbox\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"last_read_inbox_message_id\":");
                writer.integer(update->last_read_inbox_message_id);
                writer.raw(",\"unread_count\":");
                writer.integer(update->unread_count);
                writer.character('}');
            });
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageMentionRead>()) {
        if (!valid_int53(update->message_id)) {
            fail_malformed(core::TdSupportedUpdateKind::MessageMentionRead,
                           core::TdMalformedUpdateReason::InvalidIdentifier);
            return false;
        }
        return counter_event(update->chat_id, update->unread_mention_count,
                             core::TdSupportedUpdateKind::MessageMentionRead,
                             "unread_mention_count", "unread_mention_count",
                             &ChatRecord::unread_mention_count);
    }
    if (const auto* update = value.get_if<core::TdUpdateChatUnreadMentionCount>()) {
        return counter_event(update->chat_id, update->unread_mention_count,
                             core::TdSupportedUpdateKind::ChatUnreadMentionCount,
                             "unread_mention_count", "unread_mention_count",
                             &ChatRecord::unread_mention_count);
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageUnreadReactions>()) {
        if (!valid_int53(update->message_id)) {
            fail_malformed(core::TdSupportedUpdateKind::MessageUnreadReactions,
                           core::TdMalformedUpdateReason::InvalidIdentifier);
            return false;
        }
        return counter_event(update->chat_id, update->unread_reaction_count,
                             core::TdSupportedUpdateKind::MessageUnreadReactions,
                             "unread_reaction_count", "unread_reaction_count",
                             &ChatRecord::unread_reaction_count);
    }
    if (const auto* update = value.get_if<core::TdUpdateChatUnreadReactionCount>()) {
        return counter_event(update->chat_id, update->unread_reaction_count,
                             core::TdSupportedUpdateKind::ChatUnreadReactionCount,
                             "unread_reaction_count", "unread_reaction_count",
                             &ChatRecord::unread_reaction_count);
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageContainsUnreadPollVotes>()) {
        if (!valid_int53(update->message_id)) {
            fail_malformed(core::TdSupportedUpdateKind::MessageContainsUnreadPollVotes,
                           core::TdMalformedUpdateReason::InvalidIdentifier);
            return false;
        }
        return counter_event(update->chat_id, update->unread_poll_vote_count,
                             core::TdSupportedUpdateKind::MessageContainsUnreadPollVotes,
                             "unread_poll_vote_count", "unread_poll_vote_count",
                             &ChatRecord::unread_poll_vote_count);
    }
    if (const auto* update = value.get_if<core::TdUpdateChatUnreadPollVoteCount>()) {
        return counter_event(update->chat_id, update->unread_poll_vote_count,
                             core::TdSupportedUpdateKind::ChatUnreadPollVoteCount,
                             "unread_poll_vote_count", "unread_poll_vote_count",
                             &ChatRecord::unread_poll_vote_count);
    }
    if (const auto* update = value.get_if<core::TdUpdateChatIsMarkedAsUnread>()) {
        auto* chat =
            metadata_chat(update->chat_id, core::TdSupportedUpdateKind::ChatIsMarkedAsUnread);
        if (chat == nullptr) {
            return false;
        }
        chat->is_marked_unread = update->is_marked_unread;
        if (!emit || chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Chat, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"chat_change\",\"change\":\"marked_unread\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"is_marked_unread\":");
                writer.boolean(update->is_marked_unread);
                writer.character('}');
            });
    }

    if (!emit) {
        return true;
    }
    if (const auto* update = value.get_if<core::TdUpdateNewMessage>()) {
        const ChatRecord* chat = nullptr;
        if (!valid_message(update->message) ||
            !require_known_chat(update->message.chat_id, chat,
                                core::TdSupportedUpdateKind::NewMessage)) {
            if (phase.load(std::memory_order_relaxed) != StreamNormalizationPhase::Failed) {
                fail_malformed(core::TdSupportedUpdateKind::NewMessage);
            }
            return false;
        }
        if (chat->kind == ChatKind::Secret) {
            return true;
        }
        const StreamRoutingSidecar routing{.event_class = StreamEventClass::Message,
                                           .chat_id = update->message.chat_id,
                                           .sender_kind = update->message.sender.kind ==
                                                                  core::TdMessageSenderKind::User
                                                              ? StreamSenderKind::User
                                                              : StreamSenderKind::Chat,
                                           .sender_id = update->message.sender.id};
        return append_rendered(sequence_value, routing,
                               [&](JsonWriter& writer, StreamRoutingSidecar& sidecar) noexcept {
                                   writer.raw("{\"event\":\"message\",\"message\":");
                                   write_message(writer, update->message, update->message.text,
                                                 &sidecar);
                                   writer.character('}');
                               });
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageContent>()) {
        const ChatRecord* chat = nullptr;
        if (!valid_int53(update->message_id) || !common::valid_utf8(update->content.text) ||
            !require_known_chat(update->chat_id, chat,
                                core::TdSupportedUpdateKind::MessageContent)) {
            if (phase.load(std::memory_order_relaxed) != StreamNormalizationPhase::Failed) {
                fail_malformed(core::TdSupportedUpdateKind::MessageContent);
            }
            return false;
        }
        if (chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(sequence_value,
                               {.event_class = StreamEventClass::Edit, .chat_id = update->chat_id},
                               [&](JsonWriter& writer) noexcept {
                                   writer.raw("{\"event\":\"edit_content\",\"chat_id\":");
                                   writer.integer(update->chat_id);
                                   writer.raw(",\"message_id\":");
                                   writer.integer(update->message_id);
                                   writer.raw(",\"content\":{\"type\":");
                                   writer.quoted(content_kind(update->content.kind));
                                   writer.raw(",\"text\":");
                                   writer.quoted(update->content.text);
                                   writer.raw("}}");
                               });
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageEdited>()) {
        const ChatRecord* chat = nullptr;
        if (!valid_int53(update->message_id) || update->edit_date < 0 ||
            !require_known_chat(update->chat_id, chat,
                                core::TdSupportedUpdateKind::MessageEdited)) {
            if (phase.load(std::memory_order_relaxed) != StreamNormalizationPhase::Failed) {
                fail_malformed(core::TdSupportedUpdateKind::MessageEdited);
            }
            return false;
        }
        if (chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(sequence_value,
                               {.event_class = StreamEventClass::Edit, .chat_id = update->chat_id},
                               [&](JsonWriter& writer) noexcept {
                                   writer.raw("{\"event\":\"edit_metadata\",\"chat_id\":");
                                   writer.integer(update->chat_id);
                                   writer.raw(",\"message_id\":");
                                   writer.integer(update->message_id);
                                   writer.raw(",\"edit_date\":");
                                   writer.timestamp(update->edit_date, true);
                                   writer.raw(",\"has_reply_markup\":");
                                   writer.boolean(update->has_reply_markup);
                                   writer.character('}');
                               });
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageInteractionInfo>()) {
        const ChatRecord* chat = nullptr;
        bool valid = valid_int53(update->message_id) &&
                     require_known_chat(update->chat_id, chat,
                                        core::TdSupportedUpdateKind::MessageInteractionInfo);
        if (valid && update->reactions) {
            for (const auto& item : update->reactions->items) {
                valid = valid && valid_reaction(item.reaction) && item.total_count >= 0 &&
                        (!item.used_sender || valid_sender(*item.used_sender)) &&
                        std::ranges::all_of(item.recent_senders, valid_sender);
            }
        }
        if (!valid) {
            if (phase.load(std::memory_order_relaxed) != StreamNormalizationPhase::Failed) {
                fail_malformed(core::TdSupportedUpdateKind::MessageInteractionInfo);
            }
            return false;
        }
        if (chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Reaction, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"reaction_snapshot\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"message_id\":");
                writer.integer(update->message_id);
                writer.raw(",\"reactions\":");
                if (!update->reactions) {
                    writer.raw("null");
                } else {
                    writer.raw("{\"items\":[");
                    for (std::size_t index = 0; index < update->reactions->items.size(); ++index) {
                        if (index != 0) {
                            writer.character(',');
                        }
                        const auto& item = update->reactions->items[index];
                        writer.raw("{\"reaction\":");
                        write_reaction(writer, item.reaction);
                        writer.raw(",\"total_count\":");
                        writer.integer(item.total_count);
                        writer.raw(",\"is_chosen\":");
                        writer.boolean(item.is_chosen);
                        writer.raw(",\"used_sender\":");
                        if (item.used_sender) {
                            write_sender(writer, *item.used_sender);
                        } else {
                            writer.raw("null");
                        }
                        writer.raw(",\"recent_senders\":[");
                        for (std::size_t sender_index = 0;
                             sender_index < item.recent_senders.size(); ++sender_index) {
                            if (sender_index != 0) {
                                writer.character(',');
                            }
                            write_sender(writer, item.recent_senders[sender_index]);
                        }
                        writer.raw("]}");
                    }
                    writer.raw("],\"are_tags\":");
                    writer.boolean(update->reactions->are_tags);
                    writer.raw(",\"can_get_added_reactions\":");
                    writer.boolean(update->reactions->can_get_added_reactions);
                    writer.character('}');
                }
                writer.character('}');
            });
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageReaction>()) {
        const ChatRecord* chat = nullptr;
        const bool valid =
            valid_int53(update->message_id) && valid_sender(update->actor) && update->date > 0 &&
            std::ranges::all_of(update->old_reactions, valid_reaction) &&
            std::ranges::all_of(update->new_reactions, valid_reaction) &&
            require_known_chat(update->chat_id, chat, core::TdSupportedUpdateKind::MessageReaction);
        if (!valid) {
            if (phase.load(std::memory_order_relaxed) != StreamNormalizationPhase::Failed) {
                fail_malformed(core::TdSupportedUpdateKind::MessageReaction);
            }
            return false;
        }
        if (chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Reaction, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"bot_reaction_change\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"message_id\":");
                writer.integer(update->message_id);
                writer.raw(",\"actor\":");
                write_sender(writer, update->actor);
                writer.raw(",\"date\":");
                writer.timestamp(update->date, false);
                writer.raw(",\"old_reactions\":");
                write_reaction_array(writer, update->old_reactions);
                writer.raw(",\"new_reactions\":");
                write_reaction_array(writer, update->new_reactions);
                writer.character('}');
            });
    }
    if (const auto* update = value.get_if<core::TdUpdateMessageReactions>()) {
        const ChatRecord* chat = nullptr;
        bool valid = valid_int53(update->message_id) && update->date > 0 &&
                     require_known_chat(update->chat_id, chat,
                                        core::TdSupportedUpdateKind::MessageReactions);
        for (const auto& item : update->reactions) {
            valid = valid && valid_reaction(item.reaction) && item.total_count >= 0;
        }
        if (!valid) {
            if (phase.load(std::memory_order_relaxed) != StreamNormalizationPhase::Failed) {
                fail_malformed(core::TdSupportedUpdateKind::MessageReactions);
            }
            return false;
        }
        if (chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Reaction, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"bot_reaction_snapshot\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"message_id\":");
                writer.integer(update->message_id);
                writer.raw(",\"date\":");
                writer.timestamp(update->date, false);
                writer.raw(",\"reactions\":[");
                for (std::size_t index = 0; index < update->reactions.size(); ++index) {
                    if (index != 0) {
                        writer.character(',');
                    }
                    writer.raw("{\"reaction\":");
                    write_reaction(writer, update->reactions[index].reaction);
                    writer.raw(",\"total_count\":");
                    writer.integer(update->reactions[index].total_count);
                    writer.character('}');
                }
                writer.raw("]}");
            });
    }
    if (const auto* update = value.get_if<core::TdUpdateDeleteMessages>()) {
        const ChatRecord* chat = nullptr;
        const bool valid =
            std::ranges::all_of(update->message_ids, valid_int53) &&
            require_known_chat(update->chat_id, chat, core::TdSupportedUpdateKind::DeleteMessages);
        if (!valid) {
            if (phase.load(std::memory_order_relaxed) != StreamNormalizationPhase::Failed) {
                fail_malformed(core::TdSupportedUpdateKind::DeleteMessages);
            }
            return false;
        }
        if (chat->kind == ChatKind::Secret) {
            return true;
        }
        return append_rendered(
            sequence_value, {.event_class = StreamEventClass::Delete, .chat_id = update->chat_id},
            [&](JsonWriter& writer) noexcept {
                writer.raw("{\"event\":\"delete_batch\",\"chat_id\":");
                writer.integer(update->chat_id);
                writer.raw(",\"message_ids\":[");
                for (std::size_t index = 0; index < update->message_ids.size(); ++index) {
                    if (index != 0) {
                        writer.character(',');
                    }
                    writer.integer(update->message_ids[index]);
                }
                writer.raw("],\"is_permanent\":");
                writer.boolean(update->is_permanent);
                writer.raw(",\"from_cache\":");
                writer.boolean(update->from_cache);
                writer.character('}');
            });
    }
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed bootstrap metadata union.
bool FixedStreamNormalizer::Impl::buffer_value(const core::TdValue& value,
                                               std::uint64_t sequence_value) noexcept {
    if (const auto* malformed = value.get_if<core::TdMalformedSupportedUpdate>()) {
        fail_malformed(malformed->kind, malformed->reason, malformed->tdlib_type_id);
        return false;
    }
    if (const auto* direct = value.get_if<core::TdDirectConversionError>()) {
        failure_type_id.store(direct->tdlib_type_id.value_or(0), std::memory_order_relaxed);
        fail(StreamFailureKind::DirectConversion);
        return false;
    }
    if (const auto* error = value.get_if<core::TdError>()) {
        failure_error_code.store(error->code, std::memory_order_relaxed);
        failure_retry_after.store(error->code == 429 ? stream_retry_after(error->message) : 0,
                                  std::memory_order_relaxed);
        fail(error->code == 429 ? StreamFailureKind::RateLimited : StreamFailureKind::TdlibError);
        return false;
    }

    Buffered item;
    std::uint64_t copied = 0;
    std::uint64_t physical = 0;
    const std::string* usernames = nullptr;
    std::size_t username_count = 0;
    std::string_view first_string;
    std::string_view second_string;
    if (const auto* update = value.get_if<core::TdUpdateUser>()) {
        if (!valid_positive_int53(update->user.id)) {
            fail_malformed(core::TdSupportedUpdateKind::User);
            return false;
        }
        item.kind = BufferedKind::Entity;
        item.entity_kind = EntityKind::User;
        item.id = update->user.id;
        item.first_bool = update->user.is_bot;
        usernames = update->user.usernames.data();
        username_count = update->user.usernames.size();
    } else if (const auto* update = value.get_if<core::TdUpdateBasicGroup>()) {
        if (!valid_positive_int53(update->basic_group.id) || update->basic_group.member_count < 0 ||
            update->basic_group.upgraded_to_supergroup_id < 0 ||
            update->basic_group.upgraded_to_supergroup_id > kMaximumInt53) {
            fail_malformed(core::TdSupportedUpdateKind::BasicGroup);
            return false;
        }
        item.kind = BufferedKind::Entity;
        item.entity_kind = EntityKind::BasicGroup;
        item.id = update->basic_group.id;
    } else if (const auto* update = value.get_if<core::TdUpdateSupergroup>()) {
        if (!valid_positive_int53(update->supergroup.id)) {
            fail_malformed(core::TdSupportedUpdateKind::Supergroup);
            return false;
        }
        item.kind = BufferedKind::Entity;
        item.entity_kind = EntityKind::Supergroup;
        item.id = update->supergroup.id;
        item.second_bool = update->supergroup.is_channel;
        usernames = update->supergroup.usernames.data();
        username_count = update->supergroup.usernames.size();
    } else if (const auto* update = value.get_if<core::TdUpdateNewChat>()) {
        const auto normalized = chat_kind(update->chat.kind);
        if (!normalized || !validate_chat(update->chat)) {
            fail_malformed(core::TdSupportedUpdateKind::NewChat);
            return false;
        }
        item.kind = BufferedKind::NewChat;
        item.id = update->chat.id;
        item.related_id = update->chat.related_id;
        item.chat_kind = *normalized;
        item.first_bool = update->chat.is_marked_unread;
        item.first_count = update->chat.unread_count;
        item.second_count = update->chat.unread_mention_count;
        item.third_count = update->chat.unread_reaction_count;
        item.fourth_count = update->chat.unread_poll_vote_count;
        item.list_count = static_cast<std::uint32_t>(update->chat.chat_lists.size());
        std::copy(update->chat.chat_lists.begin(), update->chat.chat_lists.end(),
                  item.lists.begin());
        first_string = update->chat.title;
        item.title_size = static_cast<std::uint32_t>(first_string.size());
        copied = first_string.size();
        physical = first_string.size() + 1U;
        if (update->chat.last_message) {
            const auto& message = *update->chat.last_message;
            second_string = message.text;
            item.message = {.present = true,
                            .id = message.id,
                            .chat_id = message.chat_id,
                            .date = message.date,
                            .sender = message.sender,
                            .is_outgoing = message.is_outgoing,
                            .topic = message.topic,
                            .content_kind = message.content_kind,
                            .text_offset = static_cast<std::uint32_t>(physical),
                            .text_size = static_cast<std::uint32_t>(message.text.size())};
            copied = saturated_add(copied, message.text.size() + (message.date != 0 ? 20U : 0U));
            physical =
                saturated_add(physical, message.text.size() + 1U + (message.date != 0 ? 21U : 0U));
        }
    } else if (const auto* update = value.get_if<core::TdUpdateChatTitle>()) {
        if (!valid_int53(update->chat_id) || !common::valid_utf8(update->title)) {
            fail_malformed(core::TdSupportedUpdateKind::ChatTitle);
            return false;
        }
        item.kind = BufferedKind::Title;
        item.id = update->chat_id;
        first_string = update->title;
        item.title_size = static_cast<std::uint32_t>(first_string.size());
        copied = first_string.size();
        physical = copied + 1U;
    } else if (const auto* update = value.get_if<core::TdUpdateChatLastMessage>()) {
        if (!valid_int53(update->chat_id) ||
            (update->last_message && (!valid_message(*update->last_message) ||
                                      update->last_message->chat_id != update->chat_id))) {
            fail_malformed(core::TdSupportedUpdateKind::ChatLastMessage);
            return false;
        }
        item.kind = BufferedKind::LastMessage;
        item.id = update->chat_id;
        if (update->last_message) {
            const auto& message = *update->last_message;
            first_string = message.text;
            item.message = {.present = true,
                            .id = message.id,
                            .chat_id = message.chat_id,
                            .date = message.date,
                            .sender = message.sender,
                            .is_outgoing = message.is_outgoing,
                            .topic = message.topic,
                            .content_kind = message.content_kind,
                            .text_offset = 0,
                            .text_size = static_cast<std::uint32_t>(message.text.size())};
            copied = message.text.size() + (message.date != 0 ? 20U : 0U);
            physical = message.text.size() + 1U + (message.date != 0 ? 21U : 0U);
        }
    } else if (const auto* update = value.get_if<core::TdUpdateChatAddedToList>()) {
        item.kind = BufferedKind::ListAdded;
        item.id = update->chat_id;
        item.list = update->list;
    } else if (const auto* update = value.get_if<core::TdUpdateChatRemovedFromList>()) {
        item.kind = BufferedKind::ListRemoved;
        item.id = update->chat_id;
        item.list = update->list;
    } else if (const auto* update = value.get_if<core::TdUpdateChatReadInbox>()) {
        item.kind = BufferedKind::ReadInbox;
        item.id = update->chat_id;
        item.message_id = update->last_read_inbox_message_id;
        item.first_count = update->unread_count;
    } else if (const auto* update = value.get_if<core::TdUpdateMessageMentionRead>()) {
        item.kind = BufferedKind::UnreadMention;
        item.id = update->chat_id;
        item.first_count = update->unread_mention_count;
    } else if (const auto* update = value.get_if<core::TdUpdateChatUnreadMentionCount>()) {
        item.kind = BufferedKind::UnreadMention;
        item.id = update->chat_id;
        item.first_count = update->unread_mention_count;
    } else if (const auto* update = value.get_if<core::TdUpdateMessageUnreadReactions>()) {
        item.kind = BufferedKind::UnreadReaction;
        item.id = update->chat_id;
        item.first_count = update->unread_reaction_count;
    } else if (const auto* update = value.get_if<core::TdUpdateChatUnreadReactionCount>()) {
        item.kind = BufferedKind::UnreadReaction;
        item.id = update->chat_id;
        item.first_count = update->unread_reaction_count;
    } else if (const auto* update = value.get_if<core::TdUpdateMessageContainsUnreadPollVotes>()) {
        item.kind = BufferedKind::UnreadPollVote;
        item.id = update->chat_id;
        item.first_count = update->unread_poll_vote_count;
    } else if (const auto* update = value.get_if<core::TdUpdateChatUnreadPollVoteCount>()) {
        item.kind = BufferedKind::UnreadPollVote;
        item.id = update->chat_id;
        item.first_count = update->unread_poll_vote_count;
    } else if (const auto* update = value.get_if<core::TdUpdateChatIsMarkedAsUnread>()) {
        item.kind = BufferedKind::MarkedUnread;
        item.id = update->chat_id;
        item.first_bool = update->is_marked_unread;
    } else {
        return true;
    }

    if (usernames != nullptr) {
        for (std::size_t index = 0; index < username_count; ++index) {
            if (!valid_username(usernames[index])) {
                fail_malformed(item.entity_kind == EntityKind::User
                                   ? core::TdSupportedUpdateKind::User
                                   : core::TdSupportedUpdateKind::Supergroup);
                return false;
            }
            copied = saturated_add(copied, usernames[index].size());
            physical = saturated_add(physical, usernames[index].size() + 1U);
        }
        item.username_count = static_cast<std::uint32_t>(username_count);
    }
    const auto charge = saturated_add(64U, copied);
    if (bootstrap_count == kStreamMetadataBootstrapItems) {
        fail_capacity({.resource = StreamMetadataResource::BootstrapItems,
                       .phase = StreamMetadataPhase::Bootstrap,
                       .limit = kStreamMetadataBootstrapItems,
                       .used = bootstrap_count,
                       .incoming = 1});
        return false;
    }
    const auto would_use = saturated_add(bootstrap_charged, charge);
    if (would_use > kStreamMetadataBootstrapBytes) {
        fail_capacity({.resource = StreamMetadataResource::BootstrapBytes,
                       .phase = StreamMetadataPhase::Bootstrap,
                       .limit = kStreamMetadataBootstrapBytes,
                       .would_use = would_use});
        return false;
    }
    if (physical > kBootstrapPhysicalBytes - bootstrap_physical) {
        fail_capacity({.resource = StreamMetadataResource::BootstrapBytes,
                       .phase = StreamMetadataPhase::Bootstrap,
                       .limit = kStreamMetadataBootstrapBytes,
                       .would_use = would_use});
        return false;
    }
    item.sequence = sequence_value;
    item.charge = charge;
    item.blob_offset = bootstrap_physical;
    item.blob_size = static_cast<std::uint32_t>(physical);
    char* destination = bootstrap_arena.get() + bootstrap_physical;
    if (usernames != nullptr) {
        for (std::size_t index = 0; index < username_count; ++index) {
            std::memcpy(destination, usernames[index].data(), usernames[index].size());
            destination += usernames[index].size();
            *destination++ = '\0';
        }
    } else {
        if (!first_string.empty() || physical != 0) {
            std::memcpy(destination, first_string.data(), first_string.size());
            destination += first_string.size();
            *destination++ = '\0';
        }
        if (!second_string.empty() || item.message.present) {
            std::memcpy(destination, second_string.data(), second_string.size());
            destination += second_string.size();
            *destination++ = '\0';
        }
        if (item.message.present && item.message.date != 0) {
            std::array<char, 20> date{};
            if (!stream_timestamp_utc(item.message.date, date)) {
                fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry,
                               core::TdMalformedUpdateReason::InvalidDate);
                return false;
            }
            std::memcpy(destination, date.data(), date.size());
            destination[date.size()] = '\0';
        }
    }
    bootstrap[bootstrap_count++] = item;
    bootstrap_charged = would_use;
    bootstrap_physical += static_cast<std::uint32_t>(physical);
    return true;
}

bool FixedStreamNormalizer::Impl::apply_buffered(const Buffered& value) noexcept {
    switch (value.kind) {
    case BufferedKind::Entity:
        return apply_entity(value.entity_kind, value.id, value.first_bool, value.second_bool,
                            nullptr, bootstrap_arena.get() + value.blob_offset,
                            value.username_count, false, StreamMetadataPhase::Bootstrap,
                            value.sequence);
    case BufferedKind::NewChat:
        return apply_buffered_chat(value, StreamMetadataPhase::Bootstrap);
    case BufferedKind::Title:
        return apply_title(value.id, {bootstrap_arena.get() + value.blob_offset, value.title_size},
                           false, StreamMetadataPhase::Bootstrap, value.sequence);
    case BufferedKind::LastMessage: {
        if (!value.message.present) {
            return apply_last_message(value.id, nullptr, {}, false, StreamMetadataPhase::Bootstrap,
                                      value.sequence);
        }
        const auto message = stored_message(value.message);
        return apply_last_message(
            value.id, &message,
            {bootstrap_arena.get() + value.blob_offset + value.message.text_offset,
             value.message.text_size},
            false, StreamMetadataPhase::Bootstrap, value.sequence);
    }
    case BufferedKind::ListAdded:
    case BufferedKind::ListRemoved: {
        auto* chat = find_chat(value.id);
        if (chat == nullptr || !valid_chat_list(value.list)) {
            fail_malformed(value.kind == BufferedKind::ListAdded
                               ? core::TdSupportedUpdateKind::ChatAddedToList
                               : core::TdSupportedUpdateKind::ChatRemovedFromList);
            return false;
        }
        if (value.kind == BufferedKind::ListAdded) {
            const bool present =
                std::any_of(chat->lists.begin(), chat->lists.begin() + chat->list_count,
                            [&](const auto& item) { return same_chat_list(item, value.list); });
            if (!present) {
                if (chat->list_count == kStreamRawChatLists) {
                    fail_malformed(core::TdSupportedUpdateKind::ChatAddedToList);
                    return false;
                }
                chat->lists[chat->list_count++] = value.list;
            }
        } else {
            std::size_t destination = 0;
            for (std::size_t index = 0; index < chat->list_count; ++index) {
                if (!same_chat_list(chat->lists[index], value.list)) {
                    chat->lists[destination++] = chat->lists[index];
                }
            }
            chat->list_count = static_cast<std::uint32_t>(destination);
        }
        return true;
    }
    case BufferedKind::ReadInbox:
    case BufferedKind::UnreadMention:
    case BufferedKind::UnreadReaction:
    case BufferedKind::UnreadPollVote:
    case BufferedKind::MarkedUnread: {
        auto* chat = find_chat(value.id);
        if (chat == nullptr || value.first_count < 0 ||
            (value.kind == BufferedKind::ReadInbox && !valid_nonnegative_int53(value.message_id))) {
            fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry);
            return false;
        }
        switch (value.kind) {
        case BufferedKind::ReadInbox:
            chat->unread_count = value.first_count;
            break;
        case BufferedKind::UnreadMention:
            chat->unread_mention_count = value.first_count;
            break;
        case BufferedKind::UnreadReaction:
            chat->unread_reaction_count = value.first_count;
            break;
        case BufferedKind::UnreadPollVote:
            chat->unread_poll_vote_count = value.first_count;
            break;
        case BufferedKind::MarkedUnread:
            chat->is_marked_unread = value.first_bool;
            break;
        default:
            break;
        }
        return true;
    }
    }
    return false;
}

namespace {

bool is_current_state_update(const core::TdValue& value) noexcept {
    return value.get_if<core::TdUpdateNewMessage>() != nullptr ||
           value.get_if<core::TdUpdateMessageContent>() != nullptr ||
           value.get_if<core::TdUpdateMessageEdited>() != nullptr ||
           value.get_if<core::TdUpdateMessageInteractionInfo>() != nullptr ||
           value.get_if<core::TdUpdateMessageReaction>() != nullptr ||
           value.get_if<core::TdUpdateMessageReactions>() != nullptr ||
           value.get_if<core::TdUpdateDeleteMessages>() != nullptr ||
           value.get_if<core::TdUpdateUser>() != nullptr ||
           value.get_if<core::TdUpdateBasicGroup>() != nullptr ||
           value.get_if<core::TdUpdateSupergroup>() != nullptr ||
           value.get_if<core::TdUpdateNewChat>() != nullptr ||
           value.get_if<core::TdUpdateChatTitle>() != nullptr ||
           value.get_if<core::TdUpdateChatLastMessage>() != nullptr ||
           value.get_if<core::TdUpdateChatAddedToList>() != nullptr ||
           value.get_if<core::TdUpdateChatRemovedFromList>() != nullptr ||
           value.get_if<core::TdUpdateChatReadInbox>() != nullptr ||
           value.get_if<core::TdUpdateMessageMentionRead>() != nullptr ||
           value.get_if<core::TdUpdateMessageUnreadReactions>() != nullptr ||
           value.get_if<core::TdUpdateMessageContainsUnreadPollVotes>() != nullptr ||
           value.get_if<core::TdUpdateChatUnreadMentionCount>() != nullptr ||
           value.get_if<core::TdUpdateChatUnreadReactionCount>() != nullptr ||
           value.get_if<core::TdUpdateChatUnreadPollVoteCount>() != nullptr ||
           value.get_if<core::TdUpdateChatIsMarkedAsUnread>() != nullptr ||
           value.get_if<core::TdMalformedSupportedUpdate>() != nullptr ||
           value.get_if<core::TdDirectConversionError>() != nullptr ||
           value.get_if<core::TdError>() != nullptr;
}

} // namespace

void FixedStreamNormalizer::Impl::update(std::int32_t expected_client,
                                         std::uint64_t expected_generation,
                                         const core::TdValue& value) noexcept {
    if (!matches(expected_client, expected_generation)) {
        return;
    }
    const auto current_phase = phase.load(std::memory_order_acquire);
    if (current_phase == StreamNormalizationPhase::Failed ||
        current_phase == StreamNormalizationPhase::Empty) {
        return;
    }
    if (!sequence(value.receive_event_sequence())) {
        return;
    }
    if (current_phase == StreamNormalizationPhase::Bootstrap) {
        static_cast<void>(buffer_value(value, value.receive_event_sequence()));
        return;
    }
    if (current_phase != StreamNormalizationPhase::Ready) {
        return;
    }
    if (apply_value(value, true, value.receive_event_sequence())) {
        drain();
    }
}

void FixedStreamNormalizer::Impl::current_state(std::int32_t expected_client,
                                                std::uint64_t expected_generation,
                                                const core::TdValue& value) noexcept {
    if (!matches(expected_client, expected_generation) ||
        phase.load(std::memory_order_acquire) != StreamNormalizationPhase::Bootstrap) {
        return;
    }
    const auto barrier = value.receive_event_sequence();
    if (barrier == 0) {
        fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry,
                       core::TdMalformedUpdateReason::InvalidIdentifier);
        return;
    }
    if (const auto* error = value.get_if<core::TdError>()) {
        failure_error_code.store(error->code, std::memory_order_relaxed);
        failure_retry_after.store(error->code == 429 ? stream_retry_after(error->message) : 0,
                                  std::memory_order_relaxed);
        fail(error->code == 429 ? StreamFailureKind::RateLimited : StreamFailureKind::TdlibError);
        return;
    }
    if (const auto* direct = value.get_if<core::TdDirectConversionError>()) {
        failure_type_id.store(direct->tdlib_type_id.value_or(0), std::memory_order_relaxed);
        fail(StreamFailureKind::DirectConversion);
        return;
    }
    const auto* state = value.get_if<core::TdCurrentState>();
    if (state == nullptr) {
        fail(StreamFailureKind::WrongCurrentState);
        return;
    }
    for (std::size_t index = 0; index < state->updates.size(); ++index) {
        if (!is_current_state_update(state->updates[index])) {
            failure_index.store(static_cast<std::uint32_t>(index), std::memory_order_relaxed);
            fail(StreamFailureKind::WrongCurrentState);
            return;
        }
        if (!apply_value(state->updates[index], false, 0, static_cast<std::uint32_t>(index))) {
            return;
        }
    }
    for (std::size_t index = 0; index < bootstrap_count; ++index) {
        if (bootstrap[index].sequence > barrier && !apply_buffered(bootstrap[index])) {
            return;
        }
    }
    for (std::size_t index = 0; index < kStreamMetadataChats; ++index) {
        const auto& chat = chats[index];
        if (!chat.occupied || chat.kind == ChatKind::Secret) {
            continue;
        }
        const auto expected = entity_kind(chat.kind);
        const auto* entity = expected ? find_entity(*expected, chat.related_id) : nullptr;
        if (entity == nullptr || !compatible(chat, *entity)) {
            fail_malformed(core::TdSupportedUpdateKind::CurrentStateEntry,
                           core::TdMalformedUpdateReason::InvalidEntity);
            return;
        }
    }
    const auto previous = last_sequence.load(std::memory_order_relaxed);
    last_sequence.store(std::max(previous, barrier), std::memory_order_relaxed);
    notify_status(detail::StreamStatusPublishPoint::Sequence);
    bootstrap_count = 0;
    bootstrap_charged = 0;
    bootstrap_physical = 0;
    phase.store(StreamNormalizationPhase::Ready, std::memory_order_release);
}

FixedStreamNormalizer::FixedStreamNormalizer(StreamReceiveSink* sink,
                                             detail::StreamStatusPublishProbe status_probe)
    : impl_(std::make_unique<Impl>(sink, status_probe)) {}

FixedStreamNormalizer::~FixedStreamNormalizer() = default;

bool FixedStreamNormalizer::begin(std::int32_t client_id, std::uint64_t generation) noexcept {
    return impl_->publish_status([&] { return impl_->begin(client_id, generation); });
}

void FixedStreamNormalizer::on_update(std::int32_t client_id, std::uint64_t generation,
                                      const core::TdValue& update) noexcept {
    impl_->publish_status([&] { impl_->update(client_id, generation, update); });
}

void FixedStreamNormalizer::on_current_state(std::int32_t client_id, std::uint64_t generation,
                                             const core::TdValue& state) noexcept {
    impl_->publish_status([&] { impl_->current_state(client_id, generation, state); });
}

void FixedStreamNormalizer::on_current_state_failure(std::int32_t client_id,
                                                     std::uint64_t generation) noexcept {
    impl_->publish_status([&] {
        if (impl_->matches(client_id, generation) &&
            impl_->phase.load(std::memory_order_acquire) == StreamNormalizationPhase::Bootstrap) {
            impl_->fail(StreamFailureKind::DispatchFailure);
        }
    });
}

StreamNormalizationStatus FixedStreamNormalizer::status() const noexcept {
    return impl_->read_status();
}

StreamMetadataView::~StreamMetadataView() noexcept {
    auto* owner = static_cast<FixedStreamNormalizer::Impl*>(owner_);
    if (owner != nullptr) {
        owner->end_borrow(token_);
    }
}

bool StreamMetadataCursor::next(StreamMetadataItemView& item) noexcept {
    const auto* owner = static_cast<const FixedStreamNormalizer::Impl*>(owner_);
    if (owner == nullptr || !owner->valid_borrow(token_)) {
        return false;
    }
    while (position_ < kStreamMetadataChats) {
        const auto index = position_++;
        const auto& chat = owner->chats[index];
        if (!chat.occupied || chat.kind == FixedStreamNormalizer::Impl::ChatKind::Secret) {
            continue;
        }
        const auto expected = FixedStreamNormalizer::Impl::entity_kind(chat.kind);
        const auto* entity = expected ? owner->find_entity(*expected, chat.related_id) : nullptr;
        if (entity == nullptr || !FixedStreamNormalizer::Impl::compatible(chat, *entity)) {
            continue;
        }
        StreamMetadataChatKind kind = StreamMetadataChatKind::Private;
        switch (chat.kind) {
        case FixedStreamNormalizer::Impl::ChatKind::Private:
            kind = StreamMetadataChatKind::Private;
            break;
        case FixedStreamNormalizer::Impl::ChatKind::BasicGroup:
            kind = StreamMetadataChatKind::BasicGroup;
            break;
        case FixedStreamNormalizer::Impl::ChatKind::Supergroup:
            kind = StreamMetadataChatKind::Supergroup;
            break;
        case FixedStreamNormalizer::Impl::ChatKind::Channel:
            kind = StreamMetadataChatKind::Channel;
            break;
        case FixedStreamNormalizer::Impl::ChatKind::Secret:
            continue;
        }
        current_ = index;
        item = {.chat_id = chat.id,
                .title = owner->chat_title(chat),
                .kind = kind,
                .is_bot =
                    chat.kind == FixedStreamNormalizer::Impl::ChatKind::Private && entity->is_bot,
                .username_count = entity->username_count};
        return true;
    }
    return false;
}

bool StreamMetadataCursor::username(std::size_t index, std::string_view& value) const noexcept {
    const auto* owner = static_cast<const FixedStreamNormalizer::Impl*>(owner_);
    if (owner == nullptr || !owner->valid_borrow(token_) || current_ >= kStreamMetadataChats) {
        return false;
    }
    const auto& chat = owner->chats[current_];
    const auto expected = FixedStreamNormalizer::Impl::entity_kind(chat.kind);
    const auto* entity = expected ? owner->find_entity(*expected, chat.related_id) : nullptr;
    if (entity == nullptr || index >= entity->username_count) {
        return false;
    }
    value = owner->entity_username(*entity, index);
    return true;
}

// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-constant-array-index,modernize-raw-string-literal,readability-function-cognitive-complexity,bugprone-unchecked-optional-access,modernize-use-nodiscard,readability-make-member-function-const,readability-convert-member-functions-to-static,readability-simplify-boolean-expr)

} // namespace tgcli::daemon
