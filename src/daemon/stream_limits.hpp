#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tgcli::daemon {

inline constexpr std::size_t kStreamMetadataBootstrapItems = 4'096;
inline constexpr std::size_t kStreamMetadataBootstrapBytes = 16'777'216;
inline constexpr std::size_t kStreamMetadataChats = 65'536;
inline constexpr std::size_t kStreamMetadataEntities = 131'072;
inline constexpr std::size_t kStreamMetadataBytes = 67'108'864;
inline constexpr std::size_t kStreamMetadataOrderItems = 4'096;
inline constexpr std::size_t kStreamMetadataOrderBytes = 16'777'216;
inline constexpr std::size_t kStreamMetadataItemBytes = 262'144;
inline constexpr std::size_t kStreamRawChatLists = 102;
inline constexpr std::size_t kStreamSubscriberSlots = 32;
inline constexpr std::size_t kStreamQueueItems = 1'024;
inline constexpr std::size_t kStreamQueueBytes = 8'388'608;
inline constexpr std::size_t kStreamQueueItemBytes = 262'144;
inline constexpr std::size_t kStreamChatFilters = 64;
inline constexpr auto kStreamWorkerPollInterval = std::chrono::milliseconds{2};

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

} // namespace tgcli::daemon
