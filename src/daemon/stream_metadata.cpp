#include "daemon/stream_metadata.hpp"

#include "common/utf8.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace tgcli::daemon {

namespace {

constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

template <typename... T> struct Overloaded : T... {
    using T::operator()...;
};

template <typename... T> Overloaded(T...) -> Overloaded<T...>;

enum class GenerationPhase { Empty, Bootstrap, Ready, Failed };

struct EntityKey {
    StreamEntityKind kind = StreamEntityKind::User;
    std::int64_t id = 0;

    bool operator==(const EntityKey&) const = default;
};

struct EntityKeyHash {
    std::size_t operator()(const EntityKey& key) const noexcept {
        const auto id = static_cast<std::uint64_t>(key.id);
        return static_cast<std::size_t>(id ^ (id >> 32U) ^
                                        (static_cast<std::uint64_t>(key.kind) << 1U));
    }
};

struct BufferedDelta {
    std::uint64_t sequence = 0;
    StreamMetadataDelta delta;
    std::uint64_t charge = 0;
};

struct FrozenNewChat {
    StreamChat chat;
    EntityKey missing_entity;
};

struct OrderedCandidate {
    std::uint64_t sequence = 0;
    std::uint64_t charge = 0;
    std::optional<std::string> line;
    std::optional<FrozenNewChat> frozen;
};

bool valid_int53(std::int64_t value) {
    return value != 0 && value >= -kMaximumInt53 && value <= kMaximumInt53;
}

bool valid_positive_int53(std::int64_t value) {
    return value > 0 && value <= kMaximumInt53;
}

std::uint64_t checked_sum(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

std::uint64_t copied_string_bytes(const std::string& value) {
    return static_cast<std::uint64_t>(value.size());
}

std::uint64_t persistent_string_bytes(const std::string& value) {
    return checked_sum(copied_string_bytes(value), 1);
}

std::uint64_t message_copied_bytes(const MessageSummary& message) {
    auto result = copied_string_bytes(message.text);
    if (message.date) {
        result = checked_sum(result, copied_string_bytes(*message.date));
    }
    return result;
}

std::uint64_t message_persistent_bytes(const MessageSummary& message) {
    auto result = persistent_string_bytes(message.text);
    if (message.date) {
        result = checked_sum(result, persistent_string_bytes(*message.date));
    }
    return result;
}

std::uint64_t entity_string_bytes(const StreamEntity& entity, bool persistent) {
    std::uint64_t result = 0;
    for (const auto& username : entity.usernames) {
        result = checked_sum(result, persistent ? persistent_string_bytes(username)
                                                : copied_string_bytes(username));
    }
    return result;
}

std::uint64_t chat_string_bytes(const StreamChat& chat, bool persistent) {
    std::uint64_t result =
        persistent ? persistent_string_bytes(chat.title) : copied_string_bytes(chat.title);
    if (chat.last_message) {
        result = checked_sum(result, persistent ? message_persistent_bytes(*chat.last_message)
                                                : message_copied_bytes(*chat.last_message));
    }
    return result;
}

std::uint64_t delta_copied_bytes(const StreamMetadataDelta& delta) {
    return std::visit(
        Overloaded{
            [](const StreamEntityDelta& value) { return entity_string_bytes(value.entity, false); },
            [](const StreamNewChatDelta& value) { return chat_string_bytes(value.chat, false); },
            [](const StreamTitleDelta& value) { return copied_string_bytes(value.title); },
            [](const StreamLastMessageDelta& value) -> std::uint64_t {
                return value.last_message ? message_copied_bytes(*value.last_message) : 0ULL;
            },
            [](const auto&) -> std::uint64_t { return 0; }},
        delta);
}

bool valid_entity(const StreamEntity& entity) {
    if (!valid_positive_int53(entity.id) ||
        !std::ranges::all_of(entity.usernames, [](const std::string& username) {
            return !username.empty() && common::valid_utf8(username);
        })) {
        return false;
    }
    switch (entity.kind) {
    case StreamEntityKind::User:
        return !entity.is_channel;
    case StreamEntityKind::BasicGroup:
        return entity.usernames.empty() && !entity.is_bot && !entity.is_channel;
    case StreamEntityKind::Supergroup:
        return !entity.is_bot;
    }
    return false;
}

std::optional<EntityKey> entity_key(const StreamChat& chat) {
    switch (chat.kind) {
    case StreamChatKind::Private:
        return EntityKey{.kind = StreamEntityKind::User, .id = chat.related_id};
    case StreamChatKind::BasicGroup:
        return EntityKey{.kind = StreamEntityKind::BasicGroup, .id = chat.related_id};
    case StreamChatKind::Supergroup:
    case StreamChatKind::Channel:
        return EntityKey{.kind = StreamEntityKind::Supergroup, .id = chat.related_id};
    case StreamChatKind::Secret:
        return std::nullopt;
    }
    return std::nullopt;
}

bool valid_chat(const StreamChat& chat) {
    if (!valid_int53(chat.id) || !valid_positive_int53(chat.related_id) ||
        !common::valid_utf8(chat.title) || chat.unread_count < 0 || chat.unread_mention_count < 0 ||
        chat.unread_reaction_count < 0 || chat.unread_poll_vote_count < 0 ||
        !std::ranges::all_of(chat.chat_lists, valid_chat_list) ||
        (chat.last_message &&
         (!valid_stream_message(*chat.last_message) || chat.last_message->chat_id != chat.id))) {
        return false;
    }
    switch (chat.kind) {
    case StreamChatKind::Private:
    case StreamChatKind::BasicGroup:
    case StreamChatKind::Supergroup:
    case StreamChatKind::Channel:
    case StreamChatKind::Secret:
        return true;
    }
    return false;
}

bool same_list(const ChatListRef& left, const ChatListRef& right) {
    return left.kind == right.kind &&
           (left.kind != ChatListKind::Folder || left.folder_id == right.folder_id);
}

std::string_view phase_name(StreamMetadataPhase phase) {
    return phase == StreamMetadataPhase::Bootstrap ? "bootstrap" : "active";
}

} // namespace

// NOLINTBEGIN(readability-function-cognitive-complexity): closed capacity schema union.
std::optional<nlohmann::json>
stream_metadata_capacity_details(const StreamMetadataCapacityFailure& failure,
                                 std::string_view operation) {
    if (operation != "listen" && operation != "wait_for") {
        return std::nullopt;
    }
    const auto phase = phase_name(failure.phase);
    switch (failure.resource) {
    case StreamMetadataResource::BootstrapItems:
        if (failure.phase != StreamMetadataPhase::Bootstrap ||
            failure.limit != kStreamMetadataBootstrapItems || failure.used != failure.limit ||
            failure.incoming != 1) {
            return std::nullopt;
        }
        return nlohmann::json{{"operation", operation},
                              {"phase", phase},
                              {"resource", "metadata_bootstrap_items"},
                              {"limit_items", failure.limit},
                              {"used_items", failure.used},
                              {"incoming_items", failure.incoming}};
    case StreamMetadataResource::BootstrapBytes:
        if (failure.phase != StreamMetadataPhase::Bootstrap ||
            failure.limit != kStreamMetadataBootstrapBytes || failure.would_use <= failure.limit) {
            return std::nullopt;
        }
        return nlohmann::json{{"operation", operation},
                              {"phase", phase},
                              {"resource", "metadata_bootstrap_bytes"},
                              {"limit_bytes", failure.limit},
                              {"would_use_bytes", failure.would_use}};
    case StreamMetadataResource::Chats:
        if (failure.limit != kStreamMetadataChats || failure.used != failure.limit ||
            failure.incoming != 1) {
            return std::nullopt;
        }
        return nlohmann::json{{"operation", operation},       {"phase", phase},
                              {"resource", "metadata_chats"}, {"limit", failure.limit},
                              {"used", failure.used},         {"incoming", failure.incoming}};
    case StreamMetadataResource::Entities:
        if (failure.limit != kStreamMetadataEntities || failure.used != failure.limit ||
            failure.incoming != 1) {
            return std::nullopt;
        }
        return nlohmann::json{
            {"operation", operation}, {"phase", phase},       {"resource", "metadata_entities"},
            {"limit", failure.limit}, {"used", failure.used}, {"incoming", failure.incoming}};
    case StreamMetadataResource::Bytes:
        if (failure.limit != kStreamMetadataBytes || failure.would_use <= failure.limit) {
            return std::nullopt;
        }
        return nlohmann::json{{"operation", operation},
                              {"phase", phase},
                              {"resource", "metadata_bytes"},
                              {"limit_bytes", failure.limit},
                              {"would_use_bytes", failure.would_use}};
    case StreamMetadataResource::OrderItems:
        if (failure.phase != StreamMetadataPhase::Active ||
            failure.limit != kStreamMetadataOrderItems || failure.used != failure.limit ||
            failure.incoming != 1) {
            return std::nullopt;
        }
        return nlohmann::json{{"operation", operation},
                              {"phase", phase},
                              {"resource", "metadata_order_items"},
                              {"limit_items", failure.limit},
                              {"used_items", failure.used},
                              {"incoming_items", failure.incoming}};
    case StreamMetadataResource::OrderBytes:
        if (failure.phase != StreamMetadataPhase::Active ||
            failure.limit != kStreamMetadataOrderBytes || failure.would_use <= failure.limit) {
            return std::nullopt;
        }
        return nlohmann::json{{"operation", operation},
                              {"phase", phase},
                              {"resource", "metadata_order_bytes"},
                              {"limit_bytes", failure.limit},
                              {"would_use_bytes", failure.would_use}};
    case StreamMetadataResource::ItemBytes:
        if (failure.phase != StreamMetadataPhase::Active ||
            failure.limit != kStreamMetadataItemBytes || failure.incoming <= failure.limit) {
            return std::nullopt;
        }
        return nlohmann::json{{"operation", operation},
                              {"phase", phase},
                              {"resource", "metadata_item_bytes"},
                              {"limit_bytes", failure.limit},
                              {"incoming_bytes", failure.incoming}};
    }
    return std::nullopt;
}
// NOLINTEND(readability-function-cognitive-complexity)

class StreamGenerationState::Impl {
  public:
    Impl() {
        bootstrap_.reserve(kStreamMetadataBootstrapItems);
        ordered_.reserve(kStreamMetadataOrderItems);
        chats_.reserve(kStreamMetadataChats);
        entities_.reserve(kStreamMetadataEntities);
        relations_.reserve(kStreamMetadataChats);
    }

    bool reset(std::uint64_t generation) {
        if (generation == 0) {
            return false;
        }
        generation_ = generation;
        phase_ = GenerationPhase::Bootstrap;
        last_sequence_ = 0;
        metadata_bytes_ = 0;
        bootstrap_bytes_ = 0;
        ordered_bytes_ = 0;
        capacity_.reset();
        malformed_ = false;
        bootstrap_.clear();
        ordered_.clear();
        chats_.clear();
        entities_.clear();
        relations_.clear();
        return true;
    }

    StreamMetadataResult buffer(std::uint64_t generation, std::uint64_t sequence,
                                StreamMetadataDelta delta) {
        if (generation != generation_) {
            return result(StreamMetadataStatus::StaleGeneration);
        }
        if (phase_ == GenerationPhase::Failed) {
            return failed_result();
        }
        if (phase_ != GenerationPhase::Bootstrap) {
            return result(StreamMetadataStatus::WrongPhase);
        }
        if (sequence == 0 || sequence <= last_sequence_ || !valid_delta(delta)) {
            return fail_malformed();
        }
        if (bootstrap_.size() == kStreamMetadataBootstrapItems) {
            return fail_capacity({.resource = StreamMetadataResource::BootstrapItems,
                                  .phase = StreamMetadataPhase::Bootstrap,
                                  .limit = kStreamMetadataBootstrapItems,
                                  .used = bootstrap_.size(),
                                  .incoming = 1});
        }
        const auto charge = checked_sum(64, delta_copied_bytes(delta));
        const auto would_use = checked_sum(bootstrap_bytes_, charge);
        if (would_use > kStreamMetadataBootstrapBytes) {
            return fail_capacity({.resource = StreamMetadataResource::BootstrapBytes,
                                  .phase = StreamMetadataPhase::Bootstrap,
                                  .limit = kStreamMetadataBootstrapBytes,
                                  .would_use = would_use});
        }
        bootstrap_.push_back({.sequence = sequence, .delta = std::move(delta), .charge = charge});
        bootstrap_bytes_ = would_use;
        last_sequence_ = sequence;
        return result(StreamMetadataStatus::Accepted);
    }

    StreamMetadataResult complete(std::uint64_t generation, std::uint64_t barrier,
                                  std::vector<StreamMetadataDelta> current_state) {
        if (generation != generation_) {
            return result(StreamMetadataStatus::StaleGeneration);
        }
        if (phase_ == GenerationPhase::Failed) {
            return failed_result();
        }
        if (phase_ != GenerationPhase::Bootstrap) {
            return result(StreamMetadataStatus::WrongPhase);
        }
        if (barrier == 0) {
            return fail_malformed();
        }
        if (!std::ranges::all_of(current_state, [](const StreamMetadataDelta& delta) {
                return valid_delta(delta);
            })) {
            return fail_malformed();
        }
        for (auto& delta : current_state) {
            if (!apply_delta(std::move(delta), false, StreamMetadataPhase::Bootstrap, barrier)) {
                return failed_result();
            }
        }
        for (auto& buffered : bootstrap_) {
            if (buffered.sequence > barrier &&
                !apply_delta(std::move(buffered.delta), false, StreamMetadataPhase::Bootstrap,
                             buffered.sequence)) {
                return failed_result();
            }
        }
        bootstrap_.clear();
        bootstrap_bytes_ = 0;
        last_sequence_ = std::max(last_sequence_, barrier);
        phase_ = GenerationPhase::Ready;
        return result(StreamMetadataStatus::Accepted);
    }

    StreamMetadataResult ingest(std::uint64_t generation, std::uint64_t sequence,
                                StreamMetadataDelta delta) {
        if (const auto status = live_preflight(generation, sequence);
            status != StreamMetadataStatus::Accepted) {
            return result(status);
        }
        if (!valid_delta(delta)) {
            return fail_malformed();
        }
        last_sequence_ = sequence;
        if (!apply_delta(std::move(delta), true, StreamMetadataPhase::Active, sequence)) {
            return failed_result();
        }
        return accepted_with_drain();
    }

    StreamMetadataResult ingest(std::uint64_t generation, std::uint64_t sequence,
                                const StreamEvent& event) {
        if (const auto status = live_preflight(generation, sequence);
            status != StreamMetadataStatus::Accepted) {
            return result(status);
        }
        last_sequence_ = sequence;
        if (!append_complete(sequence, event)) {
            return failed_result();
        }
        return accepted_with_drain();
    }

    [[nodiscard]] bool barrier_open() const {
        return !ordered_.empty();
    }

    [[nodiscard]] bool ready_for_admission() const {
        return phase_ == GenerationPhase::Ready && ordered_.empty();
    }

    [[nodiscard]] std::optional<StreamMetadataSnapshot> snapshot() const {
        if (!ready_for_admission()) {
            return std::nullopt;
        }
        StreamMetadataSnapshot value{.generation = generation_, .chats = {}};
        value.chats.reserve(chats_.size());
        for (const auto& [id, chat] : chats_) {
            static_cast<void>(id);
            if (chat.kind == StreamChatKind::Secret) {
                continue;
            }
            const auto key = entity_key(chat);
            if (!key) {
                continue;
            }
            const auto entity = entities_.find(*key);
            if (entity == entities_.end()) {
                continue;
            }
            const auto summary = derive_summary(chat, entity->second);
            if (summary) {
                value.chats.push_back(*summary);
            }
        }
        std::ranges::sort(value.chats, [](const ChatSummary& left, const ChatSummary& right) {
            return left.identity.id < right.identity.id;
        });
        return value;
    }

    [[nodiscard]] std::uint64_t generation() const {
        return generation_;
    }

    [[nodiscard]] GenerationPhase phase() const {
        return phase_;
    }

    [[nodiscard]] const std::optional<StreamMetadataCapacityFailure>& capacity_failure() const {
        return capacity_;
    }

  private:
    std::uint64_t generation_ = 0;
    GenerationPhase phase_ = GenerationPhase::Empty;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t metadata_bytes_ = 0;
    std::uint64_t bootstrap_bytes_ = 0;
    std::uint64_t ordered_bytes_ = 0;
    std::optional<StreamMetadataCapacityFailure> capacity_;
    bool malformed_ = false;

    static bool valid_delta(const StreamMetadataDelta& delta) {
        return std::visit(
            Overloaded{
                [](const StreamEntityDelta& value) { return valid_entity(value.entity); },
                [](const StreamNewChatDelta& value) { return valid_chat(value.chat); },
                [](const StreamTitleDelta& value) {
                    return valid_int53(value.chat_id) && common::valid_utf8(value.title);
                },
                [](const StreamLastMessageDelta& value) {
                    return valid_int53(value.chat_id) &&
                           (!value.last_message || valid_stream_message(*value.last_message)) &&
                           (!value.last_message || value.last_message->chat_id == value.chat_id);
                },
                [](const StreamListAddedDelta& value) {
                    return valid_int53(value.chat_id) && valid_chat_list(value.list);
                },
                [](const StreamListRemovedDelta& value) {
                    return valid_int53(value.chat_id) && valid_chat_list(value.list);
                },
                [](const StreamReadInboxDelta& value) {
                    return valid_int53(value.chat_id) && value.last_read_inbox_message_id >= 0 &&
                           value.last_read_inbox_message_id <= kMaximumInt53 &&
                           value.unread_count >= 0;
                },
                [](const StreamUnreadMentionDelta& value) {
                    return valid_int53(value.chat_id) && value.unread_mention_count >= 0;
                },
                [](const StreamUnreadReactionDelta& value) {
                    return valid_int53(value.chat_id) && value.unread_reaction_count >= 0;
                },
                [](const StreamUnreadPollVoteDelta& value) {
                    return valid_int53(value.chat_id) && value.unread_poll_vote_count >= 0;
                },
                [](const StreamMarkedUnreadDelta& value) { return valid_int53(value.chat_id); }},
            delta);
    }

    StreamMetadataStatus live_preflight(std::uint64_t generation, std::uint64_t sequence) {
        if (generation != generation_) {
            return StreamMetadataStatus::StaleGeneration;
        }
        if (phase_ == GenerationPhase::Failed) {
            return malformed_ ? StreamMetadataStatus::Malformed : StreamMetadataStatus::Failed;
        }
        if (phase_ != GenerationPhase::Ready) {
            return StreamMetadataStatus::WrongPhase;
        }
        if (sequence == 0 || sequence <= last_sequence_) {
            fail_malformed();
            return StreamMetadataStatus::Malformed;
        }
        return StreamMetadataStatus::Accepted;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): closed metadata delta union.
    bool apply_delta(StreamMetadataDelta delta, bool emit, StreamMetadataPhase phase,
                     std::uint64_t sequence) {
        return std::visit(
            Overloaded{
                [&](StreamEntityDelta& value) {
                    return apply_entity(std::move(value.entity), emit, phase, sequence);
                },
                [&](StreamNewChatDelta& value) {
                    return apply_new_chat(std::move(value.chat), emit, phase, sequence);
                },
                [&](StreamTitleDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    const auto incoming = persistent_string_bytes(value.title);
                    const auto outgoing = persistent_string_bytes(chat->title);
                    if (!replace_metadata_bytes(outgoing, incoming, phase)) {
                        return false;
                    }
                    chat->title = std::move(value.title);
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(sequence,
                                           StreamEvent{ChatChangeEvent{TitleChatChange{
                                               .chat_id = chat->id, .title = chat->title}}});
                },
                [&](StreamLastMessageDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    const auto outgoing =
                        chat->last_message ? message_persistent_bytes(*chat->last_message) : 0;
                    const auto incoming =
                        value.last_message ? message_persistent_bytes(*value.last_message) : 0;
                    if (!replace_metadata_bytes(outgoing, incoming, phase)) {
                        return false;
                    }
                    chat->last_message = std::move(value.last_message);
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(
                               sequence,
                               StreamEvent{ChatChangeEvent{LastMessageChatChange{
                                   .chat_id = chat->id, .last_message = chat->last_message}}});
                },
                [&](StreamListAddedDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    if (std::ranges::none_of(chat->chat_lists, [&](const ChatListRef& current) {
                            return same_list(current, value.list);
                        })) {
                        chat->chat_lists.push_back(value.list);
                    }
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(sequence,
                                           StreamEvent{ChatChangeEvent{ListAddedChatChange{
                                               .chat_id = chat->id, .list = value.list}}});
                },
                [&](StreamListRemovedDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    std::erase_if(chat->chat_lists, [&](const ChatListRef& current) {
                        return same_list(current, value.list);
                    });
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(sequence,
                                           StreamEvent{ChatChangeEvent{ListRemovedChatChange{
                                               .chat_id = chat->id, .list = value.list}}});
                },
                [&](StreamReadInboxDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    chat->unread_count = value.unread_count;
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(
                               sequence,
                               StreamEvent{ChatChangeEvent{ReadInboxChatChange{
                                   .chat_id = chat->id,
                                   .last_read_inbox_message_id = value.last_read_inbox_message_id,
                                   .unread_count = value.unread_count}}});
                },
                [&](StreamUnreadMentionDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    chat->unread_mention_count = value.unread_mention_count;
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(
                               sequence, StreamEvent{ChatChangeEvent{UnreadMentionChatChange{
                                             .chat_id = chat->id,
                                             .unread_mention_count = value.unread_mention_count}}});
                },
                [&](StreamUnreadReactionDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    chat->unread_reaction_count = value.unread_reaction_count;
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(
                               sequence,
                               StreamEvent{ChatChangeEvent{UnreadReactionChatChange{
                                   .chat_id = chat->id,
                                   .unread_reaction_count = value.unread_reaction_count}}});
                },
                [&](StreamUnreadPollVoteDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    chat->unread_poll_vote_count = value.unread_poll_vote_count;
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(
                               sequence,
                               StreamEvent{ChatChangeEvent{UnreadPollVoteChatChange{
                                   .chat_id = chat->id,
                                   .unread_poll_vote_count = value.unread_poll_vote_count}}});
                },
                [&](StreamMarkedUnreadDelta& value) {
                    auto* chat = find_chat(value.chat_id);
                    if (chat == nullptr) {
                        return fail_malformed_bool();
                    }
                    chat->is_marked_unread = value.is_marked_unread;
                    return !emit || chat->kind == StreamChatKind::Secret ||
                           append_complete(sequence,
                                           StreamEvent{ChatChangeEvent{MarkedUnreadChatChange{
                                               .chat_id = chat->id,
                                               .is_marked_unread = value.is_marked_unread}}});
                }},
            delta);
    }

    bool apply_entity(StreamEntity entity, bool emit, StreamMetadataPhase phase,
                      std::uint64_t sequence) {
        const EntityKey key{.kind = entity.kind, .id = entity.id};
        const auto existing = entities_.find(key);
        if (existing == entities_.end() && entities_.size() == kStreamMetadataEntities) {
            fail_capacity({.resource = StreamMetadataResource::Entities,
                           .phase = phase,
                           .limit = kStreamMetadataEntities,
                           .used = entities_.size(),
                           .incoming = 1});
            return false;
        }
        const auto outgoing =
            existing == entities_.end() ? 0 : entity_string_bytes(existing->second, true);
        const auto incoming = entity_string_bytes(entity, true);
        if (!replace_metadata_bytes(outgoing, incoming, phase)) {
            return false;
        }

        StreamChat* related_chat = nullptr;
        if (const auto relation = relations_.find(key); relation != relations_.end()) {
            related_chat = find_chat(relation->second);
            if (related_chat == nullptr) {
                return fail_malformed_bool();
            }
        }
        std::optional<ChatIdentity> before;
        if (related_chat != nullptr && existing != entities_.end()) {
            const auto summary = derive_summary(*related_chat, existing->second);
            if (!summary) {
                return fail_malformed_bool();
            }
            before = summary->identity;
        }

        entities_.insert_or_assign(key, std::move(entity));
        const auto& current = entities_.find(key)->second;
        std::optional<ChatSummary> after;
        if (related_chat != nullptr) {
            after = derive_summary(*related_chat, current);
            if (!after) {
                return fail_malformed_bool();
            }
        }
        if (emit && !complete_frozen(key, current)) {
            return false;
        }
        if (emit && after) {
            if (!before || *before != after->identity) {
                return append_complete(
                    sequence, StreamEvent{ChatChangeEvent{IdentityChatChange{after->identity}}});
            }
        }
        return true;
    }

    bool apply_new_chat(StreamChat chat, bool emit, StreamMetadataPhase phase,
                        std::uint64_t sequence) {
        const auto existing = chats_.find(chat.id);
        if (existing == chats_.end() && chats_.size() == kStreamMetadataChats) {
            fail_capacity({.resource = StreamMetadataResource::Chats,
                           .phase = phase,
                           .limit = kStreamMetadataChats,
                           .used = chats_.size(),
                           .incoming = 1});
            return false;
        }
        const auto key = entity_key(chat);
        if (key) {
            const auto relation = relations_.find(*key);
            if (relation != relations_.end() && relation->second != chat.id) {
                return fail_malformed_bool();
            }
        }
        const auto outgoing =
            existing == chats_.end() ? 0 : chat_string_bytes(existing->second, true);
        const auto incoming = chat_string_bytes(chat, true);
        if (!replace_metadata_bytes(outgoing, incoming, phase)) {
            return false;
        }
        if (existing != chats_.end()) {
            const auto old_key = entity_key(existing->second);
            if (old_key && old_key != key) {
                relations_.erase(*old_key);
            }
        }
        chats_.insert_or_assign(chat.id, chat);
        if (key) {
            relations_.insert_or_assign(*key, chat.id);
        }
        if (chat.kind == StreamChatKind::Secret) {
            return true;
        }
        if (!key) {
            return fail_malformed_bool();
        }
        const auto entity = entities_.find(*key);
        if (entity == entities_.end()) {
            return !emit || append_incomplete(sequence, std::move(chat), *key);
        }
        const auto summary = derive_summary(chat, entity->second);
        if (!summary) {
            return fail_malformed_bool();
        }
        return !emit ||
               append_complete(sequence, StreamEvent{ChatChangeEvent{NewChatChange{*summary}}});
    }

    static std::optional<ChatSummary> derive_summary(const StreamChat& chat,
                                                     const StreamEntity& entity) {
        const auto key = entity_key(chat);
        if (!key || key->kind != entity.kind || key->id != entity.id ||
            (chat.kind == StreamChatKind::Channel && !entity.is_channel) ||
            (chat.kind == StreamChatKind::Supergroup && entity.is_channel)) {
            return std::nullopt;
        }
        std::string type;
        switch (chat.kind) {
        case StreamChatKind::Private:
            type = "private";
            break;
        case StreamChatKind::BasicGroup:
            type = "basic_group";
            break;
        case StreamChatKind::Supergroup:
            type = "supergroup";
            break;
        case StreamChatKind::Channel:
            type = "channel";
            break;
        case StreamChatKind::Secret:
            return std::nullopt;
        }
        const bool is_archived = std::ranges::any_of(chat.chat_lists, [](const ChatListRef& list) {
            return list.kind == ChatListKind::Archive;
        });
        std::vector<std::int32_t> folders;
        for (const auto& list : chat.chat_lists) {
            if (list.kind == ChatListKind::Folder) {
                folders.push_back(list.folder_id);
            }
        }
        std::ranges::sort(folders);
        folders.erase(std::unique(folders.begin(), folders.end()), folders.end());
        ChatSummary result{
            .identity = {.id = chat.id,
                         .title = chat.title,
                         .type = std::move(type),
                         .is_bot = chat.kind == StreamChatKind::Private && entity.is_bot,
                         .usernames = entity.usernames},
            .is_archived = is_archived,
            .folder_ids = std::move(folders),
            .is_marked_unread = chat.is_marked_unread,
            .unread_count = chat.unread_count,
            .unread_mention_count = chat.unread_mention_count,
            .unread_reaction_count = chat.unread_reaction_count,
            .unread_poll_vote_count = chat.unread_poll_vote_count,
            .last_message = chat.last_message};
        return valid_chat_summary(result) ? std::optional<ChatSummary>{std::move(result)}
                                          : std::nullopt;
    }

    bool replace_metadata_bytes(std::uint64_t outgoing, std::uint64_t incoming,
                                StreamMetadataPhase phase) {
        if (outgoing > metadata_bytes_) {
            return fail_malformed_bool();
        }
        const auto retained = metadata_bytes_ - outgoing;
        const auto would_use = checked_sum(retained, incoming);
        if (would_use > kStreamMetadataBytes) {
            fail_capacity({.resource = StreamMetadataResource::Bytes,
                           .phase = phase,
                           .limit = kStreamMetadataBytes,
                           .would_use = would_use});
            return false;
        }
        metadata_bytes_ = would_use;
        return true;
    }

    bool append_complete(std::uint64_t sequence, const StreamEvent& event) {
        auto line = stream_event_line(event);
        if (!line) {
            return fail_malformed_bool();
        }
        const auto incoming = static_cast<std::uint64_t>(line->size());
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
        ordered_.push_back({.sequence = sequence,
                            .charge = incoming,
                            .line = std::move(line),
                            .frozen = std::nullopt});
        return true;
    }

    bool append_incomplete(std::uint64_t sequence, StreamChat chat, EntityKey key) {
        if (!reserve_candidate(kStreamMetadataItemBytes)) {
            return false;
        }
        ordered_.push_back(
            {.sequence = sequence,
             .charge = kStreamMetadataItemBytes,
             .line = std::nullopt,
             .frozen = FrozenNewChat{.chat = std::move(chat), .missing_entity = key}});
        return true;
    }

    bool reserve_candidate(std::uint64_t incoming) {
        if (ordered_.size() == kStreamMetadataOrderItems) {
            fail_capacity({.resource = StreamMetadataResource::OrderItems,
                           .phase = StreamMetadataPhase::Active,
                           .limit = kStreamMetadataOrderItems,
                           .used = ordered_.size(),
                           .incoming = 1});
            return false;
        }
        const auto would_use = checked_sum(ordered_bytes_, incoming);
        if (would_use > kStreamMetadataOrderBytes) {
            fail_capacity({.resource = StreamMetadataResource::OrderBytes,
                           .phase = StreamMetadataPhase::Active,
                           .limit = kStreamMetadataOrderBytes,
                           .would_use = would_use});
            return false;
        }
        ordered_bytes_ = would_use;
        return true;
    }

    bool complete_frozen(const EntityKey& key, const StreamEntity& entity) {
        for (auto& candidate : ordered_) {
            if (!candidate.frozen || candidate.frozen->missing_entity != key) {
                continue;
            }
            const auto summary = derive_summary(candidate.frozen->chat, entity);
            if (!summary) {
                return fail_malformed_bool();
            }
            auto line =
                stream_event_line(StreamEvent{ChatChangeEvent{NewChatChange{.chat = *summary}}});
            if (!line) {
                return fail_malformed_bool();
            }
            const auto actual = static_cast<std::uint64_t>(line->size());
            if (actual > kStreamMetadataItemBytes) {
                fail_capacity({.resource = StreamMetadataResource::ItemBytes,
                               .phase = StreamMetadataPhase::Active,
                               .limit = kStreamMetadataItemBytes,
                               .incoming = actual});
                return false;
            }
            ordered_bytes_ -= candidate.charge;
            candidate.charge = actual;
            ordered_bytes_ += actual;
            candidate.line = std::move(line);
            candidate.frozen.reset();
        }
        return true;
    }

    StreamMetadataResult accepted_with_drain() {
        std::vector<std::string> items;
        while (!ordered_.empty() && ordered_.front().line) {
            ordered_bytes_ -= ordered_.front().charge;
            items.push_back(std::move(ordered_.front().line).value_or(std::string{}));
            ordered_.erase(ordered_.begin());
        }
        return {.status = StreamMetadataStatus::Accepted,
                .items = std::move(items),
                .capacity = std::nullopt};
    }

    StreamChat* find_chat(std::int64_t id) {
        const auto found = chats_.find(id);
        return found == chats_.end() ? nullptr : &found->second;
    }

    bool fail_malformed_bool() {
        fail_malformed();
        return false;
    }

    StreamMetadataResult fail_malformed() {
        phase_ = GenerationPhase::Failed;
        malformed_ = true;
        ordered_.clear();
        ordered_bytes_ = 0;
        return result(StreamMetadataStatus::Malformed);
    }

    StreamMetadataResult fail_capacity(StreamMetadataCapacityFailure failure) {
        phase_ = GenerationPhase::Failed;
        capacity_ = failure;
        ordered_.clear();
        ordered_bytes_ = 0;
        return result(StreamMetadataStatus::Failed);
    }

    StreamMetadataResult failed_result() const {
        return result(malformed_ ? StreamMetadataStatus::Malformed : StreamMetadataStatus::Failed);
    }

    StreamMetadataResult result(StreamMetadataStatus status) const {
        return {.status = status, .items = {}, .capacity = capacity_};
    }

    std::vector<BufferedDelta> bootstrap_;
    std::vector<OrderedCandidate> ordered_;
    std::unordered_map<std::int64_t, StreamChat> chats_;
    std::unordered_map<EntityKey, StreamEntity, EntityKeyHash> entities_;
    std::unordered_map<EntityKey, std::int64_t, EntityKeyHash> relations_;
};

StreamGenerationState::StreamGenerationState() : impl_(std::make_unique<Impl>()) {}

StreamGenerationState::~StreamGenerationState() = default;
StreamGenerationState::StreamGenerationState(StreamGenerationState&&) noexcept = default;
StreamGenerationState& StreamGenerationState::operator=(StreamGenerationState&&) noexcept = default;

bool StreamGenerationState::reset(std::uint64_t generation) {
    return impl_->reset(generation);
}

StreamMetadataResult StreamGenerationState::buffer_bootstrap_delta(std::uint64_t generation,
                                                                   std::uint64_t receive_sequence,
                                                                   StreamMetadataDelta delta) {
    return impl_->buffer(generation, receive_sequence, std::move(delta));
}

StreamMetadataResult
StreamGenerationState::complete_bootstrap(std::uint64_t generation, std::uint64_t response_barrier,
                                          std::vector<StreamMetadataDelta> current_state) {
    return impl_->complete(generation, response_barrier, std::move(current_state));
}

StreamMetadataResult StreamGenerationState::ingest(std::uint64_t generation,
                                                   std::uint64_t receive_sequence,
                                                   StreamMetadataDelta delta) {
    return impl_->ingest(generation, receive_sequence, std::move(delta));
}

StreamMetadataResult StreamGenerationState::ingest(std::uint64_t generation,
                                                   std::uint64_t receive_sequence,
                                                   const StreamEvent& event) {
    return impl_->ingest(generation, receive_sequence, event);
}

std::uint64_t StreamGenerationState::generation() const {
    return impl_->generation();
}

bool StreamGenerationState::bootstrapping() const {
    return impl_->phase() == GenerationPhase::Bootstrap;
}

bool StreamGenerationState::stream_ready() const {
    return impl_->phase() == GenerationPhase::Ready;
}

bool StreamGenerationState::failed() const {
    return impl_->phase() == GenerationPhase::Failed;
}

bool StreamGenerationState::ordering_barrier_open() const {
    return impl_->barrier_open();
}

bool StreamGenerationState::ready_for_admission() const {
    return impl_->ready_for_admission();
}

std::optional<StreamMetadataCapacityFailure> StreamGenerationState::capacity_failure() const {
    return impl_->capacity_failure();
}

std::optional<StreamMetadataSnapshot> StreamGenerationState::snapshot() const {
    return impl_->snapshot();
}

} // namespace tgcli::daemon
