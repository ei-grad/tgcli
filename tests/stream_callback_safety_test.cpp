#include "daemon/stream_service.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)
#include <pthread.h>
#include <unistd.h>
#endif

namespace {

std::atomic<std::uint64_t>& callback_violations() noexcept {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

bool& forced_callback_guard() noexcept {
    static thread_local bool value = false;
    return value;
}

bool guarded() noexcept {
    return forced_callback_guard() || tgcli::daemon::detail::stream_callback_active();
}

void note_violation() noexcept {
    if (guarded()) {
        callback_violations().fetch_add(1, std::memory_order_relaxed);
    }
}

class ForcedGuard {
  public:
    ForcedGuard() noexcept {
        forced_callback_guard() = true;
    }
    ~ForcedGuard() {
        forced_callback_guard() = false;
    }
    ForcedGuard(const ForcedGuard&) = delete;
    ForcedGuard& operator=(const ForcedGuard&) = delete;
    ForcedGuard(ForcedGuard&&) = delete;
    ForcedGuard& operator=(ForcedGuard&&) = delete;
};

class FixedProbeSink final : public tgcli::daemon::StreamReceiveSink {
  public:
    void on_item(const tgcli::daemon::StreamItemView& item,
                 const tgcli::daemon::StreamMetadataView& metadata) noexcept override {
        std::size_t copied = 0;
        for (const auto bytes : item.spans()) {
            if (!bytes.empty()) {
                std::memcpy(line_.data() + copied, bytes.data(), bytes.size());
                copied += bytes.size();
            }
        }
        size_ = copied;
        ++count_;
        auto cursor = metadata.cursor();
        tgcli::daemon::StreamMetadataItemView chat;
        while (cursor.next(chat)) {
            for (std::size_t index = 0; index < chat.username_count; ++index) {
                std::string_view username;
                if (!cursor.username(index, username)) {
                    invalid_metadata_ = true;
                }
            }
        }
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }
    [[nodiscard]] bool invalid_metadata() const noexcept {
        return invalid_metadata_;
    }

  private:
    std::array<char, tgcli::daemon::kStreamMetadataItemBytes> line_{};
    std::size_t size_ = 0;
    std::size_t count_ = 0;
    bool invalid_metadata_ = false;
};

tgcli::core::TdUserSummary user(std::int64_t id, std::string username = "ada") {
    return {.id = id,
            .first_name = "Ada",
            .last_name = "Lovelace",
            .usernames = {std::move(username)},
            .phone_number = {},
            .is_bot = false,
            .is_premium = false,
            .presence = tgcli::core::TdUserPresence::Online};
}

tgcli::core::TdChat chat(std::int64_t id, std::int64_t related_id, tgcli::core::TdChatKind kind) {
    return {.id = id,
            .title = "Project",
            .kind = kind,
            .related_id = related_id,
            .tdlib_type_id = 0,
            .positions = {},
            .chat_lists = {{.kind = tgcli::core::TdChatListKind::Main, .folder_id = 0}},
            .is_marked_unread = false,
            .unread_count = 0,
            .unread_mention_count = 0,
            .unread_reaction_count = 0,
            .unread_poll_vote_count = 0,
            .last_message = std::nullopt,
            .notification_settings = std::nullopt};
}

tgcli::core::TdMessageSummary message(std::int64_t chat_id = -1001) {
    return {
        .id = 123,
        .chat_id = chat_id,
        .date = 1'785'924'000,
        .sender = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 0},
        .is_outgoing = false,
        .topic = std::nullopt,
        .content_kind = tgcli::core::TdMessageContentKind::Text,
        .text = "callback"};
}

template <typename Value> tgcli::core::TdValue stamped(Value value, std::uint64_t sequence) {
    auto result = tgcli::core::TdValue::from(std::move(value));
    result.set_receive_event_metadata(sequence, tgcli::core::TdEventClock::time_point{});
    return result;
}

} // namespace

void* operator new(std::size_t size) {
    note_violation();
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
    if (void* value = std::malloc(size == 0 ? 1 : size)) {
        return value;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* value) noexcept {
    note_violation();
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(value);
}

void operator delete[](void* value) noexcept {
    ::operator delete(value);
}

void operator delete(void* value, std::size_t size) noexcept {
    static_cast<void>(size);
    ::operator delete(value);
}

void operator delete[](void* value, std::size_t size) noexcept {
    static_cast<void>(size);
    ::operator delete(value);
}

#if defined(__linux__)
// GNU ld --wrap requires these reserved external spellings.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t* mutex);
extern "C" int __real_pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                                             const timespec* timeout);
extern "C" ssize_t __real_read(int descriptor, void* buffer, size_t count);
extern "C" ssize_t __real_write(int descriptor, const void* buffer, size_t count);
extern "C" int __real_close(int descriptor);
extern "C" int __real_fsync(int descriptor);
extern "C" int __real_fdatasync(int descriptor);

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    note_violation();
    return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    note_violation();
    return __real_pthread_mutex_unlock(mutex);
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex) {
    note_violation();
    return __real_pthread_cond_wait(condition, mutex);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                                             const timespec* timeout) {
    note_violation();
    return __real_pthread_cond_timedwait(condition, mutex, timeout);
}
extern "C" ssize_t __wrap_read(int descriptor, void* buffer, size_t count) {
    note_violation();
    return __real_read(descriptor, buffer, count);
}
extern "C" ssize_t __wrap_write(int descriptor, const void* buffer, size_t count) {
    note_violation();
    return __real_write(descriptor, buffer, count);
}
extern "C" int __wrap_close(int descriptor) {
    note_violation();
    return __real_close(descriptor);
}
extern "C" int __wrap_fsync(int descriptor) {
    note_violation();
    return __real_fsync(descriptor);
}
extern "C" int __wrap_fdatasync(int descriptor) {
    note_violation();
    return __real_fdatasync(descriptor);
}
// NOLINTEND(bugprone-reserved-identifier)
#endif

TEST_CASE("production observer callbacks use only fixed nonblocking storage",
          "[stream][callback-safety][fake-boundary]") {
    callback_violations().store(0, std::memory_order_relaxed);
    FixedProbeSink sink;
    tgcli::daemon::StreamService service(&sink);
    auto observer = service.observer_factory()(1001, 1);
    REQUIRE(observer);

    tgcli::core::TdCurrentState base;
    base.updates.push_back(tgcli::core::TdValue::from(tgcli::core::TdUpdateUser{.user = user(42)}));
    base.updates.push_back(tgcli::core::TdValue::from(
        tgcli::core::TdUpdateNewChat{.chat = chat(-1001, 42, tgcli::core::TdChatKind::Private)}));
    base.updates.push_back(tgcli::core::TdValue::from(tgcli::core::TdUpdateSupergroup{
        .supergroup = {.id = 55, .usernames = {"group"}, .is_channel = false, .is_forum = false}}));
    base.updates.push_back(tgcli::core::TdValue::from(tgcli::core::TdUpdateNewChat{
        .chat = chat(-3000, 55, tgcli::core::TdChatKind::Supergroup)}));
    auto state = stamped(std::move(base), 1);
    observer->on_current_state(state);
    REQUIRE(service.status().phase == tgcli::daemon::StreamNormalizationPhase::Ready);

    std::vector<tgcli::core::TdValue> updates;
    updates.reserve(25);
    std::uint64_t sequence = 2;
    updates.push_back(stamped(tgcli::core::TdUpdateNewMessage{.message = message()}, sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateMessageContent{
            .chat_id = -1001,
            .message_id = 123,
            .content = {.kind = tgcli::core::TdMessageContentKind::Text,
                        .text = "edited",
                        .tdlib_type_id = 0}},
        sequence++));
    updates.push_back(stamped(tgcli::core::TdUpdateMessageEdited{.chat_id = -1001,
                                                                 .message_id = 123,
                                                                 .edit_date = 1'785'924'000,
                                                                 .has_reply_markup = false},
                              sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateMessageInteractionInfo{
            .chat_id = -1001, .message_id = 123, .reactions = std::nullopt},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateMessageReaction{
            .chat_id = -1001,
            .message_id = 123,
            .actor = {.kind = tgcli::core::TdMessageSenderKind::User, .id = 42, .tdlib_type_id = 0},
            .date = 1'785'924'000,
            .old_reactions = {},
            .new_reactions = {{.kind = tgcli::core::TdReactionKind::Paid,
                               .emoji = {},
                               .custom_emoji_id = 0,
                               .tdlib_type_id = 0}}},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateMessageReactions{
            .chat_id = -1001, .message_id = 123, .date = 1'785'924'000, .reactions = {}},
        sequence++));
    updates.push_back(stamped(tgcli::core::TdUpdateDeleteMessages{.client_generation = 1,
                                                                  .chat_id = -1001,
                                                                  .message_ids = {123},
                                                                  .is_permanent = false,
                                                                  .from_cache = false},
                              sequence++));
    updates.push_back(stamped(tgcli::core::TdUpdateUser{.user = user(42, "changed")}, sequence++));
    updates.push_back(
        stamped(tgcli::core::TdUpdateBasicGroup{.basic_group = {.id = 77,
                                                                .member_count = 1,
                                                                .is_active = true,
                                                                .upgraded_to_supergroup_id = 0}},
                sequence++));
    updates.push_back(
        stamped(tgcli::core::TdUpdateSupergroup{.supergroup = {.id = 55,
                                                               .usernames = {"changed_group"},
                                                               .is_channel = false,
                                                               .is_forum = false}},
                sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateNewChat{.chat = chat(-4000, 77, tgcli::core::TdChatKind::BasicGroup)},
        sequence++));
    updates.push_back(
        stamped(tgcli::core::TdUpdateChatTitle{.chat_id = -1001, .title = "title"}, sequence++));
    updates.push_back(
        stamped(tgcli::core::TdUpdateChatLastMessage{.chat_id = -1001, .last_message = message()},
                sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateChatAddedToList{
            .chat_id = -1001,
            .list = {.kind = tgcli::core::TdChatListKind::Archive, .folder_id = 0}},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateChatRemovedFromList{
            .chat_id = -1001,
            .list = {.kind = tgcli::core::TdChatListKind::Archive, .folder_id = 0}},
        sequence++));
    updates.push_back(stamped(tgcli::core::TdUpdateChatReadInbox{.chat_id = -1001,
                                                                 .last_read_inbox_message_id = 123,
                                                                 .unread_count = 1},
                              sequence++));
    updates.push_back(stamped(tgcli::core::TdUpdateMessageMentionRead{.chat_id = -1001,
                                                                      .message_id = 123,
                                                                      .unread_mention_count = 1},
                              sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateMessageUnreadReactions{
            .chat_id = -1001, .message_id = 123, .unread_reaction_count = 1},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateMessageContainsUnreadPollVotes{.chat_id = -1001,
                                                            .message_id = 123,
                                                            .contains_unread_poll_votes = true,
                                                            .unread_poll_vote_count = 1},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateChatUnreadMentionCount{.chat_id = -1001, .unread_mention_count = 1},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateChatUnreadReactionCount{.chat_id = -1001, .unread_reaction_count = 1},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateChatUnreadPollVoteCount{.chat_id = -1001, .unread_poll_vote_count = 1},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateChatIsMarkedAsUnread{.chat_id = -1001, .is_marked_unread = true},
        sequence++));
    updates.push_back(stamped(
        tgcli::core::TdUpdateNewChat{.chat = chat(-5000, 88, tgcli::core::TdChatKind::Supergroup)},
        sequence++));
    updates.push_back(
        stamped(tgcli::core::TdUpdateChatTitle{.chat_id = -1001, .title = "queued"}, sequence++));
    updates.push_back(stamped(tgcli::core::TdUpdateSupergroup{.supergroup = {.id = 88,
                                                                             .usernames = {"late"},
                                                                             .is_channel = false,
                                                                             .is_forum = false}},
                              sequence++));
    for (const auto& update : updates) {
        observer->on_update(update);
    }
    REQUIRE(service.status().phase == tgcli::daemon::StreamNormalizationPhase::Ready);
    CHECK_FALSE(sink.invalid_metadata());
    CHECK(sink.count() >= 20);

    auto malformed = stamped(
        tgcli::core::TdMalformedSupportedUpdate{
            .kind = tgcli::core::TdSupportedUpdateKind::MessageContent,
            .reason = tgcli::core::TdMalformedUpdateReason::InvalidContent,
            .tdlib_type_id = 44},
        sequence++);
    observer->on_update(malformed);
    CHECK(service.status().phase == tgcli::daemon::StreamNormalizationPhase::Failed);

    tgcli::daemon::FixedStreamNormalizer reset_probe;
    bool reset_ok = false;
    {
        const ForcedGuard guard;
        reset_ok = reset_probe.begin(2001, 2);
    }
    REQUIRE(reset_ok);
    CHECK(callback_violations().load(std::memory_order_acquire) == 0);
}
