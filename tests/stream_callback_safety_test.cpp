#include "daemon/stream_ingress.hpp"
#include "daemon/stream_service.hpp"
#include "support/scripted_td_runtime.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/epoll.h>
#endif

using AppleRecorder = void (*)(std::size_t) noexcept;

#if defined(__APPLE__)
extern "C" void tgcli_stream_callback_safety_install_apple_recorder(AppleRecorder) noexcept;
#endif

namespace {

enum class InstrumentationClass : std::size_t {
    CppAllocation,
    CAllocation,
    Synchronization,
    Io,
    TdSubmission,
    Teardown,
    Count
};

using InstrumentationCounts =
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(InstrumentationClass::Count)>;

using ObserverFactoryResult =
    std::invoke_result_t<tgcli::core::TdGenerationObserverFactory&, std::int32_t, std::uint64_t>;
static_assert(
    std::is_same_v<ObserverFactoryResult, std::unique_ptr<tgcli::core::TdGenerationObserver>>);
static_assert(!std::is_copy_constructible_v<tgcli::daemon::StreamService>);
static_assert(!std::is_move_constructible_v<tgcli::daemon::StreamService>);

InstrumentationCounts& instrumentation_counts() noexcept {
    static InstrumentationCounts values{};
    return values;
}

InstrumentationCounts& observed_counts() noexcept {
    static InstrumentationCounts values{};
    return values;
}

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

void note_violation(InstrumentationClass instrumentation) noexcept {
    observed_counts()
        .at(static_cast<std::size_t>(instrumentation))
        .fetch_add(1, std::memory_order_relaxed);
    if (guarded()) {
        callback_violations().fetch_add(1, std::memory_order_relaxed);
        instrumentation_counts()
            .at(static_cast<std::size_t>(instrumentation))
            .fetch_add(1, std::memory_order_relaxed);
    }
}

void record_apple_instrumentation(std::size_t raw_instrumentation) noexcept {
    if (raw_instrumentation >= static_cast<std::size_t>(InstrumentationClass::Count)) {
        callback_violations().fetch_add(1, std::memory_order_relaxed);
        return;
    }
    note_violation(static_cast<InstrumentationClass>(raw_instrumentation));
}

static_assert(std::is_same_v<decltype(&record_apple_instrumentation), AppleRecorder>);

std::uint64_t observed_count(InstrumentationClass instrumentation) noexcept {
    return observed_counts()
        .at(static_cast<std::size_t>(instrumentation))
        .load(std::memory_order_acquire);
}

std::uint64_t instrumentation_count(InstrumentationClass instrumentation) noexcept {
    return instrumentation_counts()
        .at(static_cast<std::size_t>(instrumentation))
        .load(std::memory_order_acquire);
}

void reset_instrumentation() noexcept {
#if defined(__APPLE__)
    tgcli_stream_callback_safety_install_apple_recorder(&record_apple_instrumentation);
#endif
    callback_violations().store(0, std::memory_order_relaxed);
    for (auto& value : instrumentation_counts()) {
        value.store(0, std::memory_order_relaxed);
    }
    for (auto& value : observed_counts()) {
        value.store(0, std::memory_order_relaxed);
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
        callback_guard_missing_ =
            callback_guard_missing_ || !tgcli::daemon::detail::stream_callback_active();
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
    [[nodiscard]] bool callback_guard_missing() const noexcept {
        return callback_guard_missing_;
    }

  private:
    std::array<char, tgcli::daemon::kStreamMetadataItemBytes> line_{};
    std::size_t size_ = 0;
    std::size_t count_ = 0;
    bool invalid_metadata_ = false;
    bool callback_guard_missing_ = false;
};

struct StatusProbe {
    bool callback_guard_missing = false;
    std::size_t calls = 0;

    static void notify(void* context,
                       tgcli::daemon::detail::StreamStatusPublishPoint point) noexcept {
        if (point == tgcli::daemon::detail::StreamStatusPublishPoint::Reset ||
            (point == tgcli::daemon::detail::StreamStatusPublishPoint::WriterBegin &&
             !tgcli::daemon::detail::stream_callback_active())) {
            return;
        }
        auto& probe = *static_cast<StatusProbe*>(context);
        probe.callback_guard_missing =
            probe.callback_guard_missing || !tgcli::daemon::detail::stream_callback_active();
        ++probe.calls;
    }
};

struct TeardownProbe {
    ~TeardownProbe() {
        note_violation(InstrumentationClass::Teardown);
    }
    TeardownProbe() = default;
    TeardownProbe(const TeardownProbe&) = delete;
    TeardownProbe& operator=(const TeardownProbe&) = delete;
    TeardownProbe(TeardownProbe&&) = delete;
    TeardownProbe& operator=(TeardownProbe&&) = delete;
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

// The target intentionally replaces every global allocation form to make callback allocations
// observable; ownership is delegated directly to the corresponding C allocation primitive.
// NOLINTBEGIN(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
void* operator new(std::size_t size) {
    note_violation(InstrumentationClass::CppAllocation);
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
    note_violation(InstrumentationClass::CppAllocation);
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

void* operator new(std::size_t size, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* value, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    ::operator delete(value);
}

void operator delete[](void* value, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    ::operator delete[](value);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    note_violation(InstrumentationClass::CppAllocation);
    const auto alignment_value = static_cast<std::size_t>(alignment);
    const auto effective_size = size == 0 ? alignment_value : size;
    if (effective_size > std::numeric_limits<std::size_t>::max() - (alignment_value - 1U)) {
        throw std::bad_alloc();
    }
    const auto rounded =
        (effective_size + alignment_value - 1U) / alignment_value * alignment_value;
    if (void* value = std::aligned_alloc(alignment_value, rounded)) {
        return value;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}

void operator delete(void* value, std::align_val_t alignment) noexcept {
    static_cast<void>(alignment);
    note_violation(InstrumentationClass::CppAllocation);
    std::free(value);
}

void operator delete[](void* value, std::align_val_t alignment) noexcept {
    ::operator delete(value, alignment);
}

void operator delete(void* value, std::size_t size, std::align_val_t alignment) noexcept {
    static_cast<void>(size);
    ::operator delete(value, alignment);
}

void operator delete[](void* value, std::size_t size, std::align_val_t alignment) noexcept {
    static_cast<void>(size);
    ::operator delete[](value, alignment);
}

void operator delete(void* value, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    ::operator delete(value, alignment);
}

void operator delete[](void* value, std::align_val_t alignment,
                       const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    ::operator delete[](value, alignment);
}

#if defined(__linux__)
// GNU ld --wrap requires these reserved external spellings.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void* __real_malloc(size_t size);
extern "C" void* __real_calloc(size_t count, size_t size);
extern "C" void* __real_realloc(void* value, size_t size);
extern "C" void __real_free(void* value);
extern "C" void* __real_aligned_alloc(size_t alignment, size_t size);
extern "C" int __real_posix_memalign(void** value, size_t alignment, size_t size);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
extern "C" int __real_pthread_mutex_trylock(pthread_mutex_t* mutex);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t* mutex);
extern "C" int __real_pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                                             const timespec* timeout);
extern "C" int __real_pthread_cond_signal(pthread_cond_t* condition);
extern "C" int __real_pthread_cond_broadcast(pthread_cond_t* condition);
extern "C" int __real_open(const char* filename, int flags, ...);
extern "C" int __real_openat(int directory, const char* filename, int flags, ...);
extern "C" ssize_t __real_read(int descriptor, void* buffer, size_t count);
extern "C" ssize_t __real_pread(int descriptor, void* buffer, size_t count, off_t offset);
extern "C" ssize_t __real_write(int descriptor, const void* buffer, size_t count);
extern "C" ssize_t __real_pwrite(int descriptor, const void* buffer, size_t count, off_t offset);
extern "C" int __real_close(int descriptor);
extern "C" int __real_fsync(int descriptor);
extern "C" int __real_fdatasync(int descriptor);
extern "C" int __real_poll(pollfd* descriptors, nfds_t count, int timeout);
extern "C" int __real_select(int descriptor_count, fd_set* read_set, fd_set* write_set,
                             fd_set* error_set, timeval* timeout);
extern "C" int __real_epoll_wait(int descriptor, epoll_event* events, int maximum_events,
                                 int timeout);
extern "C" ssize_t __real_send(int socket, const void* buffer, size_t length, int flags);
extern "C" ssize_t __real_recv(int socket, void* buffer, size_t length, int flags);

extern "C" void* __wrap_malloc(size_t size) {
    note_violation(InstrumentationClass::CAllocation);
    return __real_malloc(size);
}
extern "C" void* __wrap_calloc(size_t count, size_t size) {
    note_violation(InstrumentationClass::CAllocation);
    return __real_calloc(count, size);
}
extern "C" void* __wrap_realloc(void* value, size_t size) {
    note_violation(InstrumentationClass::CAllocation);
    return __real_realloc(value, size);
}
extern "C" void __wrap_free(void* value) {
    note_violation(InstrumentationClass::CAllocation);
    __real_free(value);
}
extern "C" void* __wrap_aligned_alloc(size_t alignment, size_t size) {
    note_violation(InstrumentationClass::CAllocation);
    return __real_aligned_alloc(alignment, size);
}
extern "C" int __wrap_posix_memalign(void** value, size_t alignment, size_t size) {
    note_violation(InstrumentationClass::CAllocation);
    return __real_posix_memalign(value, alignment, size);
}

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    note_violation(InstrumentationClass::Synchronization);
    return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    note_violation(InstrumentationClass::Synchronization);
    return __real_pthread_mutex_trylock(mutex);
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    note_violation(InstrumentationClass::Synchronization);
    return __real_pthread_mutex_unlock(mutex);
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex) {
    note_violation(InstrumentationClass::Synchronization);
    return __real_pthread_cond_wait(condition, mutex);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                                             const timespec* timeout) {
    note_violation(InstrumentationClass::Synchronization);
    return __real_pthread_cond_timedwait(condition, mutex, timeout);
}
extern "C" int __wrap_pthread_cond_signal(pthread_cond_t* condition) {
    note_violation(InstrumentationClass::Synchronization);
    return __real_pthread_cond_signal(condition);
}
extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* condition) {
    note_violation(InstrumentationClass::Synchronization);
    return __real_pthread_cond_broadcast(condition);
}
extern "C" int __wrap_open(const char* filename, int flags, ...) {
    note_violation(InstrumentationClass::Io);
    if ((flags & O_CREAT) == 0) {
        return __real_open(filename, flags);
    }
    va_list arguments{};
    va_start(arguments, flags);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    const auto mode = static_cast<mode_t>(va_arg(arguments, int));
    va_end(arguments);
    return __real_open(filename, flags, mode);
}
extern "C" int __wrap_openat(int directory, const char* filename, int flags, ...) {
    note_violation(InstrumentationClass::Io);
    if ((flags & O_CREAT) == 0) {
        return __real_openat(directory, filename, flags);
    }
    va_list arguments{};
    va_start(arguments, flags);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    const auto mode = static_cast<mode_t>(va_arg(arguments, int));
    va_end(arguments);
    return __real_openat(directory, filename, flags, mode);
}
extern "C" ssize_t __wrap_read(int descriptor, void* buffer, size_t count) {
    note_violation(InstrumentationClass::Io);
    return __real_read(descriptor, buffer, count);
}
extern "C" ssize_t __wrap_pread(int descriptor, void* buffer, size_t count, off_t offset) {
    note_violation(InstrumentationClass::Io);
    return __real_pread(descriptor, buffer, count, offset);
}
extern "C" ssize_t __wrap_write(int descriptor, const void* buffer, size_t count) {
    note_violation(InstrumentationClass::Io);
    return __real_write(descriptor, buffer, count);
}
extern "C" ssize_t __wrap_pwrite(int descriptor, const void* buffer, size_t count, off_t offset) {
    note_violation(InstrumentationClass::Io);
    return __real_pwrite(descriptor, buffer, count, offset);
}
extern "C" int __wrap_close(int descriptor) {
    note_violation(InstrumentationClass::Io);
    return __real_close(descriptor);
}
extern "C" int __wrap_fsync(int descriptor) {
    note_violation(InstrumentationClass::Io);
    return __real_fsync(descriptor);
}
extern "C" int __wrap_fdatasync(int descriptor) {
    note_violation(InstrumentationClass::Io);
    return __real_fdatasync(descriptor);
}
extern "C" int __wrap_poll(pollfd* descriptors, nfds_t count, int timeout) {
    note_violation(InstrumentationClass::Io);
    return __real_poll(descriptors, count, timeout);
}
extern "C" int __wrap_select(int descriptor_count, fd_set* read_set, fd_set* write_set,
                             fd_set* error_set, timeval* timeout) {
    note_violation(InstrumentationClass::Io);
    return __real_select(descriptor_count, read_set, write_set, error_set, timeout);
}
extern "C" int __wrap_epoll_wait(int descriptor, epoll_event* events, int maximum_events,
                                 int timeout) {
    note_violation(InstrumentationClass::Io);
    return __real_epoll_wait(descriptor, events, maximum_events, timeout);
}
extern "C" ssize_t __wrap_send(int socket, const void* buffer, size_t length, int flags) {
    note_violation(InstrumentationClass::Io);
    return __real_send(socket, buffer, length, flags);
}
extern "C" ssize_t __wrap_recv(int socket, void* buffer, size_t length, int flags) {
    note_violation(InstrumentationClass::Io);
    return __real_recv(socket, buffer, length, flags);
}
// NOLINTEND(bugprone-reserved-identifier)
#endif
// NOLINTEND(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)

TEST_CASE("production observer callbacks use only fixed nonblocking storage",
          "[stream][callback-safety][fake-boundary]") {
    reset_instrumentation();
    FixedProbeSink sink;
    StatusProbe status_probe;
    tgcli::daemon::StreamService service(&sink,
                                         {.context = &status_probe, .hook = &StatusProbe::notify});
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
    CHECK_FALSE(sink.callback_guard_missing());
    CHECK_FALSE(status_probe.callback_guard_missing);
    CHECK(status_probe.calls > 0);
    CHECK(sink.count() >= 20);

    auto malformed = stamped(
        tgcli::core::TdMalformedSupportedUpdate{
            .kind = tgcli::core::TdSupportedUpdateKind::MessageContent,
            .reason = tgcli::core::TdMalformedUpdateReason::InvalidContent,
            .tdlib_type_id = 44},
        sequence++);
    observer->on_update(malformed);
    CHECK(service.status().phase == tgcli::daemon::StreamNormalizationPhase::Failed);
    const auto deletes_before_teardown = observed_count(InstrumentationClass::CppAllocation);
    observer.reset();
    CHECK(observed_count(InstrumentationClass::CppAllocation) > deletes_before_teardown);

    tgcli::daemon::FixedStreamNormalizer reset_probe;
    bool reset_ok = false;
    {
        const ForcedGuard guard;
        // These calls are the positive controls for the allocation wrappers above.
        // NOLINTBEGIN(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
        reset_ok = reset_probe.begin(2001, 2);
    }
    REQUIRE(reset_ok);
    CHECK(callback_violations().load(std::memory_order_acquire) == 0);
}

TEST_CASE("callback guard covers every retained failure path",
          "[stream][callback-safety][failure][fake-boundary]") {
    reset_instrumentation();
    StatusProbe status_probe;
    tgcli::daemon::StreamService service(nullptr,
                                         {.context = &status_probe, .hook = &StatusProbe::notify});
    auto factory = service.observer_factory();

    auto dispatch = factory(1001, 1);
    dispatch->on_current_state_failure(std::make_exception_ptr(std::runtime_error("dispatch")));
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::DispatchFailure);

    auto rate = factory(1002, 2);
    auto rate_error = stamped(tgcli::core::TdError{.code = 429, .message = "FLOOD_WAIT_17"}, 1);
    rate->on_update(rate_error);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::RateLimited);
    CHECK(service.status().failure.retry_after == 17);

    auto current_rate = factory(1007, 7);
    auto current_rate_error =
        stamped(tgcli::core::TdError{.code = 429, .message = "retry after 19"}, 1);
    current_rate->on_current_state(current_rate_error);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::RateLimited);
    CHECK(service.status().failure.retry_after == 19);

    auto active_rate = factory(1008, 8);
    auto empty_state = stamped(tgcli::core::TdCurrentState{}, 1);
    active_rate->on_current_state(empty_state);
    auto active_rate_error =
        stamped(tgcli::core::TdError{.code = 429, .message = "FLOOD_WAIT_23"}, 2);
    active_rate->on_update(active_rate_error);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::RateLimited);
    CHECK(service.status().failure.retry_after == 23);

    auto tdlib = factory(1009, 9);
    auto tdlib_error = stamped(tgcli::core::TdError{.code = 500, .message = "failure"}, 1);
    tdlib->on_update(tdlib_error);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::TdlibError);

    auto direct = factory(1003, 3);
    auto conversion = stamped(tgcli::core::TdDirectConversionError{.tdlib_type_id = 99}, 1);
    direct->on_update(conversion);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::DirectConversion);

    auto wrong = factory(1004, 4);
    auto wrong_state = stamped(tgcli::core::TdOk{}, 1);
    wrong->on_current_state(wrong_state);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::WrongCurrentState);

    auto malformed = factory(1005, 5);
    auto malformed_value = stamped(
        tgcli::core::TdMalformedSupportedUpdate{
            .kind = tgcli::core::TdSupportedUpdateKind::MessageContent,
            .reason = tgcli::core::TdMalformedUpdateReason::InvalidContent,
            .tdlib_type_id = 77},
        1);
    malformed->on_update(malformed_value);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::MalformedSupported);

    auto capacity = factory(1006, 6);
    tgcli::core::TdCurrentState base;
    base.updates.push_back(tgcli::core::TdValue::from(tgcli::core::TdUpdateUser{.user = user(42)}));
    base.updates.push_back(tgcli::core::TdValue::from(
        tgcli::core::TdUpdateNewChat{.chat = chat(-1001, 42, tgcli::core::TdChatKind::Private)}));
    auto state = stamped(std::move(base), 1);
    capacity->on_current_state(state);
    std::string oversized(tgcli::daemon::kStreamMetadataItemBytes + 1, 'x');
    auto overflow =
        stamped(tgcli::core::TdUpdateChatTitle{.chat_id = -1001, .title = std::move(oversized)}, 2);
    capacity->on_update(overflow);
    CHECK(service.status().failure.kind == tgcli::daemon::StreamFailureKind::Capacity);

    CHECK_FALSE(status_probe.callback_guard_missing);
    CHECK(status_probe.calls > 0);
    CHECK(callback_violations().load(std::memory_order_acquire) == 0);
}

TEST_CASE("callback instrumentation has guarded positive controls",
          "[stream][callback-safety][instrumentation]") {
    reset_instrumentation();
    tgcli::test::ScriptedTdRuntime runtime;
    runtime.set_before_send([](const tgcli::core::TdFunctionData&) {
        note_violation(InstrumentationClass::TdSubmission);
    });
    auto function = tgcli::core::TdValue::scripted_function(
        tgcli::core::TdFunctionData{tgcli::core::TdFunctionKind::GetCurrentState});
#if defined(__linux__) || defined(__APPLE__)
    pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t wait_condition = PTHREAD_COND_INITIALIZER;
    std::atomic<bool> waiter_ready{false};
    std::thread signaler([&] {
        while (!waiter_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        static_cast<void>(pthread_mutex_lock(&wait_mutex));
        static_cast<void>(pthread_cond_signal(&wait_condition));
        static_cast<void>(pthread_mutex_unlock(&wait_mutex));
    });
#endif

    {
        const ForcedGuard guard;
        void* scalar = ::operator new(8);
        ::operator delete(scalar);
        void* array = ::operator new[](8);
        ::operator delete[](array);
        void* aligned = ::operator new(64, std::align_val_t{64});
        ::operator delete(aligned, std::align_val_t{64});
        void* nothrow = ::operator new(8, std::nothrow);
        ::operator delete(nothrow, std::nothrow);
        void* nothrow_array = ::operator new[](8, std::nothrow);
        ::operator delete[](nothrow_array, std::nothrow);
        void* sized = ::operator new(8);
        ::operator delete(sized, std::size_t{8});
        void* sized_array = ::operator new[](8);
        ::operator delete[](sized_array, std::size_t{8});
        void* aligned_nothrow = ::operator new(64, std::align_val_t{64}, std::nothrow);
        ::operator delete(aligned_nothrow, std::align_val_t{64}, std::nothrow);
        void* sized_aligned = ::operator new(64, std::align_val_t{64});
        ::operator delete(sized_aligned, std::size_t{64}, std::align_val_t{64});

        void* allocated = std::malloc(8);
        if (void* reallocated = std::realloc(allocated, 16)) {
            allocated = reallocated;
        }
        std::free(allocated);
        void* cleared = std::calloc(1, 8);
        std::free(cleared);
        void* aligned_c = std::aligned_alloc(64, 64);
        std::free(aligned_c);
        void* positioned = nullptr;
        const int positioned_result = posix_memalign(&positioned, 64, 64);
        static_cast<void>(positioned_result);
        std::free(positioned);
        // NOLINTEND(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)

#if defined(__linux__) || defined(__APPLE__)
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
        static_cast<void>(pthread_mutex_lock(&mutex));
        static_cast<void>(pthread_mutex_unlock(&mutex));
        const int try_lock_result = pthread_mutex_trylock(&mutex);
        if (try_lock_result == 0) {
            static_cast<void>(pthread_mutex_unlock(&mutex));
        }
        static_cast<void>(pthread_cond_signal(&condition));
        static_cast<void>(pthread_cond_broadcast(&condition));
        static_cast<void>(pthread_mutex_destroy(&mutex));
        static_cast<void>(pthread_cond_destroy(&condition));
        static_cast<void>(pthread_mutex_lock(&wait_mutex));
        waiter_ready.store(true, std::memory_order_release);
        static_cast<void>(pthread_cond_wait(&wait_condition, &wait_mutex));
        static_cast<void>(pthread_mutex_unlock(&wait_mutex));
        static_cast<void>(pthread_mutex_lock(&wait_mutex));
        const timespec expired{};
        static_cast<void>(pthread_cond_timedwait(&wait_condition, &wait_mutex, &expired));
        static_cast<void>(pthread_mutex_unlock(&wait_mutex));

        const int descriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
        const int descriptor_at = openat(AT_FDCWD, "/dev/null", O_RDONLY | O_CLOEXEC);
        std::array<char, 1> byte{};
        const auto read_result = read(descriptor, byte.data(), byte.size());
        const auto pread_result = pread(descriptor, byte.data(), byte.size(), 0);
        const auto write_result = write(descriptor, byte.data(), byte.size());
        const auto pwrite_result = pwrite(descriptor, byte.data(), byte.size(), 0);
        static_cast<void>(read_result);
        static_cast<void>(pread_result);
        static_cast<void>(write_result);
        static_cast<void>(pwrite_result);
        static_cast<void>(fsync(descriptor));
#if defined(__linux__)
        static_cast<void>(fdatasync(descriptor));
#endif
        static_cast<void>(poll(nullptr, 0, 0));
        timeval timeout{};
        static_cast<void>(select(0, nullptr, nullptr, nullptr, &timeout));
#if defined(__linux__)
        epoll_event event{};
        static_cast<void>(epoll_wait(-1, &event, 1, 0));
#endif
        static_cast<void>(send(-1, byte.data(), byte.size(), 0));
        static_cast<void>(recv(-1, byte.data(), byte.size(), 0));
        static_cast<void>(close(descriptor_at));
        static_cast<void>(close(descriptor));
#endif

        runtime.send(1, 1, 1, function);
        const TeardownProbe teardown;
        static_cast<void>(teardown);
    }

#if defined(__linux__) || defined(__APPLE__)
    signaler.join();
    static_cast<void>(pthread_mutex_destroy(&wait_mutex));
    static_cast<void>(pthread_cond_destroy(&wait_condition));
#endif

    CHECK(instrumentation_count(InstrumentationClass::CppAllocation) > 0);
    CHECK(instrumentation_count(InstrumentationClass::CAllocation) > 0);
    CHECK(instrumentation_count(InstrumentationClass::Synchronization) > 0);
    CHECK(instrumentation_count(InstrumentationClass::Io) > 0);
    CHECK(instrumentation_count(InstrumentationClass::TdSubmission) > 0);
    CHECK(instrumentation_count(InstrumentationClass::Teardown) > 0);

    const auto guarded_count = callback_violations().load(std::memory_order_acquire);
    void* outside = ::operator new(8);
    ::operator delete(outside);
    CHECK(callback_violations().load(std::memory_order_acquire) == guarded_count);
}

TEST_CASE("Apple callback recorder rejects unknown instrumentation classes",
          "[stream][callback-safety][instrumentation]") {
    reset_instrumentation();
    record_apple_instrumentation(static_cast<std::size_t>(InstrumentationClass::Count));
    CHECK(callback_violations().load(std::memory_order_acquire) == 1);
    for (const auto& value : instrumentation_counts()) {
        CHECK(value.load(std::memory_order_acquire) == 0);
    }
    for (const auto& value : observed_counts()) {
        CHECK(value.load(std::memory_order_acquire) == 0);
    }
}

TEST_CASE("fixed ingress scan enqueue overflow and removal stay callback safe",
          "[stream][callback-safety][ingress]") {
    reset_instrumentation();
    tgcli::daemon::StreamIngressHub hub;
    const tgcli::daemon::StreamIngressRequest request{
        .client_id = 1001,
        .generation = 1,
        .operation = tgcli::daemon::StreamOperation::Listen,
        .mode = tgcli::daemon::StreamMode::Items,
        .type_mask = tgcli::daemon::stream_event_mask(tgcli::daemon::StreamEventClass::Chat)};
    auto reserved = hub.reserve(request);
    REQUIRE(reserved);
    REQUIRE(hub.commit_activation(*reserved));
    REQUIRE(hub.activate_armed(1001, 1, 1) == 1);
    const tgcli::daemon::StreamRoutingSidecar routing{
        .event_class = tgcli::daemon::StreamEventClass::Chat, .chat_id = -1001, .json_size = 3};
    auto item = tgcli::daemon::StreamIngressTestAccess::item("{", "}\n", 2, routing);

    {
        const ForcedGuard guard;
        hub.publish(item);
    }
    auto front = hub.poll_front(*reserved);
    REQUIRE(front);
    REQUIRE(hub.consume(*reserved, *front));
    tgcli::daemon::StreamIngressTestAccess::set_tickets(hub, *reserved,
                                                        tgcli::daemon::kStreamQueueItems, 0, 0, 0);
    bool detached = false;
    {
        const ForcedGuard guard;
        hub.publish(item);
        detached = hub.detach(*reserved);
    }
    REQUIRE(detached);
    REQUIRE(hub.claim_terminal(*reserved));
    REQUIRE(hub.poll_reclaim(*reserved));
    CHECK(callback_violations().load(std::memory_order_acquire) == 0);
}

TEST_CASE("stream lifecycle activation authorization and enqueue stay callback safe",
          "[stream][callback-safety][ingress][boundary][authorization]") {
    reset_instrumentation();
    tgcli::daemon::StreamService service;
    auto observer = service.observer_factory()(1001, 1);
    REQUIRE(observer);

    tgcli::core::TdCurrentState base;
    base.updates.push_back(tgcli::core::TdValue::from(tgcli::core::TdUpdateUser{.user = user(42)}));
    base.updates.push_back(tgcli::core::TdValue::from(
        tgcli::core::TdUpdateNewChat{.chat = chat(-1001, 42, tgcli::core::TdChatKind::Private)}));
    observer->on_current_state(stamped(std::move(base), 1));
    observer->on_authorization_state(tgcli::core::AuthStateData{tgcli::core::AuthState::Ready}, 2);

    const tgcli::daemon::StreamIngressRequest request{
        .client_id = 1001,
        .generation = 1,
        .operation = tgcli::daemon::StreamOperation::Listen,
        .mode = tgcli::daemon::StreamMode::Items,
        .type_mask = tgcli::daemon::stream_event_mask(tgcli::daemon::StreamEventClass::Message)};
    auto reserved = service.ingress_hub().reserve(request);
    REQUIRE(reserved);
    REQUIRE(service.ingress_hub().commit_activation(*reserved));
    observer->on_receive_boundary(2);
    REQUIRE(service.ingress_hub().activation_state(*reserved) ==
            tgcli::daemon::StreamIngressState::Published);

    observer->on_update(stamped(tgcli::core::TdUpdateNewMessage{.message = message()}, 3));
    observer->on_receive_boundary(3);
    REQUIRE(service.ingress_hub().poll_front(*reserved));
    observer->on_authorization_state(tgcli::core::AuthStateData{tgcli::core::AuthState::Closing},
                                     4);
    observer->on_receive_boundary(4);
    const auto terminal = service.ingress_hub().claim_terminal(*reserved);
    REQUIRE(terminal);
    CHECK(terminal->cause == tgcli::daemon::StreamTerminalCause::AuthorizationLost);
    CHECK(terminal->auth_state == static_cast<std::int32_t>(tgcli::core::AuthState::Closing));
    CHECK(callback_violations().load(std::memory_order_acquire) == 0);
}
