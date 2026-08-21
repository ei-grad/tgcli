#pragma once

#include "daemon/stream_model.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace tgcli::daemon {

inline constexpr std::size_t kStreamMetadataBootstrapItems = 4'096;
inline constexpr std::size_t kStreamMetadataBootstrapBytes = 16'777'216;
inline constexpr std::size_t kStreamMetadataChats = 65'536;
inline constexpr std::size_t kStreamMetadataEntities = 131'072;
inline constexpr std::size_t kStreamMetadataBytes = 67'108'864;
inline constexpr std::size_t kStreamMetadataOrderItems = 4'096;
inline constexpr std::size_t kStreamMetadataOrderBytes = 16'777'216;
inline constexpr std::size_t kStreamMetadataItemBytes = 262'144;

enum class StreamEntityKind { User, BasicGroup, Supergroup };

struct StreamEntity {
    StreamEntityKind kind = StreamEntityKind::User;
    std::int64_t id = 0;
    std::vector<std::string> usernames;
    bool is_bot = false;
    bool is_channel = false;

    bool operator==(const StreamEntity&) const = default;
};

enum class StreamChatKind { Private, BasicGroup, Supergroup, Channel, Secret };

struct StreamChat {
    std::int64_t id = 0;
    std::string title;
    StreamChatKind kind = StreamChatKind::Private;
    std::int64_t related_id = 0;
    std::vector<ChatListRef> chat_lists;
    bool is_marked_unread = false;
    std::int32_t unread_count = 0;
    std::int32_t unread_mention_count = 0;
    std::int32_t unread_reaction_count = 0;
    std::int32_t unread_poll_vote_count = 0;
    std::optional<MessageSummary> last_message;

    bool operator==(const StreamChat&) const = default;
};

struct StreamEntityDelta {
    StreamEntity entity;
};

struct StreamNewChatDelta {
    StreamChat chat;
};

struct StreamTitleDelta {
    std::int64_t chat_id = 0;
    std::string title;
};

struct StreamLastMessageDelta {
    std::int64_t chat_id = 0;
    std::optional<MessageSummary> last_message;
};

struct StreamListAddedDelta {
    std::int64_t chat_id = 0;
    ChatListRef list;
};

struct StreamListRemovedDelta {
    std::int64_t chat_id = 0;
    ChatListRef list;
};

struct StreamReadInboxDelta {
    std::int64_t chat_id = 0;
    std::int64_t last_read_inbox_message_id = 0;
    std::int32_t unread_count = 0;
};

struct StreamUnreadMentionDelta {
    std::int64_t chat_id = 0;
    std::int32_t unread_mention_count = 0;
};

struct StreamUnreadReactionDelta {
    std::int64_t chat_id = 0;
    std::int32_t unread_reaction_count = 0;
};

struct StreamUnreadPollVoteDelta {
    std::int64_t chat_id = 0;
    std::int32_t unread_poll_vote_count = 0;
};

struct StreamMarkedUnreadDelta {
    std::int64_t chat_id = 0;
    bool is_marked_unread = false;
};

using StreamMetadataDelta =
    std::variant<StreamEntityDelta, StreamNewChatDelta, StreamTitleDelta, StreamLastMessageDelta,
                 StreamListAddedDelta, StreamListRemovedDelta, StreamReadInboxDelta,
                 StreamUnreadMentionDelta, StreamUnreadReactionDelta, StreamUnreadPollVoteDelta,
                 StreamMarkedUnreadDelta>;

enum class StreamMetadataPhase { Bootstrap, Active };
enum class StreamMetadataResource {
    BootstrapItems,
    BootstrapBytes,
    Chats,
    Entities,
    Bytes,
    OrderItems,
    OrderBytes,
    ItemBytes
};

struct StreamMetadataCapacityFailure {
    StreamMetadataResource resource = StreamMetadataResource::BootstrapItems;
    StreamMetadataPhase phase = StreamMetadataPhase::Bootstrap;
    std::uint64_t limit = 0;
    std::uint64_t used = 0;
    std::uint64_t incoming = 0;
    std::uint64_t would_use = 0;

    bool operator==(const StreamMetadataCapacityFailure&) const = default;
};

enum class StreamMetadataStatus { Accepted, StaleGeneration, WrongPhase, Malformed, Failed };

struct StreamMetadataResult {
    StreamMetadataStatus status = StreamMetadataStatus::Accepted;
    std::vector<std::string> items;
    std::optional<StreamMetadataCapacityFailure> capacity;
};

struct StreamMetadataSnapshot {
    std::uint64_t generation = 0;
    std::vector<ChatSummary> chats;
};

std::optional<nlohmann::json>
stream_metadata_capacity_details(const StreamMetadataCapacityFailure& failure,
                                 std::string_view operation);

class StreamGenerationState {
  public:
    StreamGenerationState();
    ~StreamGenerationState();
    StreamGenerationState(const StreamGenerationState&) = delete;
    StreamGenerationState& operator=(const StreamGenerationState&) = delete;
    StreamGenerationState(StreamGenerationState&&) noexcept;
    StreamGenerationState& operator=(StreamGenerationState&&) noexcept;

    bool reset(std::uint64_t generation);
    StreamMetadataResult buffer_bootstrap_delta(std::uint64_t generation,
                                                std::uint64_t receive_sequence,
                                                StreamMetadataDelta delta);
    StreamMetadataResult complete_bootstrap(std::uint64_t generation,
                                            std::uint64_t response_barrier,
                                            std::vector<StreamMetadataDelta> current_state);
    StreamMetadataResult ingest(std::uint64_t generation, std::uint64_t receive_sequence,
                                StreamMetadataDelta delta);
    StreamMetadataResult ingest(std::uint64_t generation, std::uint64_t receive_sequence,
                                const StreamEvent& event);

    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] bool bootstrapping() const;
    [[nodiscard]] bool stream_ready() const;
    [[nodiscard]] bool failed() const;
    [[nodiscard]] bool ordering_barrier_open() const;
    [[nodiscard]] bool ready_for_admission() const;
    [[nodiscard]] std::optional<StreamMetadataCapacityFailure> capacity_failure() const;
    [[nodiscard]] std::optional<StreamMetadataSnapshot> snapshot() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tgcli::daemon
