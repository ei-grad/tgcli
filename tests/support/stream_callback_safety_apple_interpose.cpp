#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#if !defined(__APPLE__)
#error "Mach-O callback-safety interposition is Apple-only"
#endif

namespace {

using Recorder = void (*)(std::size_t) noexcept;
std::atomic<Recorder> recorder{nullptr};
static_assert(std::atomic<Recorder>::is_always_lock_free);

void record(std::size_t instrumentation) noexcept {
    if (const auto installed = recorder.load(std::memory_order_acquire); installed != nullptr) {
        installed(instrumentation);
    }
}

struct InterposeSubstitution {
    const void* replacement;
    const void* original;
};

#define TGCLI_INTERPOSE(replacement, original)                                                     \
    __attribute__((used)) static const InterposeSubstitution substitution_##original[]             \
        __attribute__((section("__DATA,__interpose"))) = {                                         \
            {reinterpret_cast<const void*>(replacement), reinterpret_cast<const void*>(original)}}

constexpr std::size_t kCAllocation = 1;
constexpr std::size_t kSynchronization = 2;
constexpr std::size_t kIo = 3;

} // namespace

extern "C" void tgcli_stream_callback_safety_install_apple_recorder(Recorder value) noexcept {
    recorder.store(value, std::memory_order_release);
}

extern "C" void* tgcli_interpose_malloc(std::size_t size) {
    record(kCAllocation);
    return std::malloc(size);
}
TGCLI_INTERPOSE(tgcli_interpose_malloc, malloc);

extern "C" void* tgcli_interpose_calloc(std::size_t count, std::size_t size) {
    record(kCAllocation);
    return std::calloc(count, size);
}
TGCLI_INTERPOSE(tgcli_interpose_calloc, calloc);

extern "C" void* tgcli_interpose_realloc(void* value, std::size_t size) {
    record(kCAllocation);
    return std::realloc(value, size);
}
TGCLI_INTERPOSE(tgcli_interpose_realloc, realloc);

extern "C" void tgcli_interpose_free(void* value) {
    record(kCAllocation);
    std::free(value);
}
TGCLI_INTERPOSE(tgcli_interpose_free, free);

extern "C" void* tgcli_interpose_aligned_alloc(std::size_t alignment, std::size_t size) {
    record(kCAllocation);
    return std::aligned_alloc(alignment, size);
}
TGCLI_INTERPOSE(tgcli_interpose_aligned_alloc, aligned_alloc);

extern "C" int tgcli_interpose_posix_memalign(void** value, std::size_t alignment,
                                              std::size_t size) {
    record(kCAllocation);
    return posix_memalign(value, alignment, size);
}
TGCLI_INTERPOSE(tgcli_interpose_posix_memalign, posix_memalign);

#define TGCLI_INTERPOSE_SYNC(name, parameters, arguments)                                          \
    extern "C" int tgcli_interpose_##name parameters {                                             \
        record(kSynchronization);                                                                  \
        return name arguments;                                                                     \
    }                                                                                              \
    TGCLI_INTERPOSE(tgcli_interpose_##name, name)

TGCLI_INTERPOSE_SYNC(pthread_mutex_lock, (pthread_mutex_t * mutex), (mutex));
TGCLI_INTERPOSE_SYNC(pthread_mutex_trylock, (pthread_mutex_t * mutex), (mutex));
TGCLI_INTERPOSE_SYNC(pthread_mutex_unlock, (pthread_mutex_t * mutex), (mutex));
TGCLI_INTERPOSE_SYNC(pthread_cond_wait, (pthread_cond_t * condition, pthread_mutex_t* mutex),
                     (condition, mutex));
TGCLI_INTERPOSE_SYNC(pthread_cond_timedwait,
                     (pthread_cond_t * condition, pthread_mutex_t* mutex, const timespec* timeout),
                     (condition, mutex, timeout));
TGCLI_INTERPOSE_SYNC(pthread_cond_signal, (pthread_cond_t * condition), (condition));
TGCLI_INTERPOSE_SYNC(pthread_cond_broadcast, (pthread_cond_t * condition), (condition));

extern "C" int tgcli_interpose_open(const char* filename, int flags, ...) {
    record(kIo);
    if ((flags & O_CREAT) == 0) {
        return open(filename, flags);
    }
    va_list arguments{};
    va_start(arguments, flags);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    const auto mode = static_cast<mode_t>(va_arg(arguments, int));
    va_end(arguments);
    return open(filename, flags, mode);
}
TGCLI_INTERPOSE(tgcli_interpose_open, open);

extern "C" int tgcli_interpose_openat(int directory, const char* filename, int flags, ...) {
    record(kIo);
    if ((flags & O_CREAT) == 0) {
        return openat(directory, filename, flags);
    }
    va_list arguments{};
    va_start(arguments, flags);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    const auto mode = static_cast<mode_t>(va_arg(arguments, int));
    va_end(arguments);
    return openat(directory, filename, flags, mode);
}
TGCLI_INTERPOSE(tgcli_interpose_openat, openat);

#define TGCLI_INTERPOSE_IO(result, name, parameters, arguments)                                    \
    extern "C" result tgcli_interpose_##name parameters {                                          \
        record(kIo);                                                                               \
        return name arguments;                                                                     \
    }                                                                                              \
    TGCLI_INTERPOSE(tgcli_interpose_##name, name)

TGCLI_INTERPOSE_IO(ssize_t, read, (int descriptor, void* buffer, std::size_t count),
                   (descriptor, buffer, count));
TGCLI_INTERPOSE_IO(ssize_t, pread, (int descriptor, void* buffer, std::size_t count, off_t offset),
                   (descriptor, buffer, count, offset));
TGCLI_INTERPOSE_IO(ssize_t, write, (int descriptor, const void* buffer, std::size_t count),
                   (descriptor, buffer, count));
TGCLI_INTERPOSE_IO(ssize_t, pwrite,
                   (int descriptor, const void* buffer, std::size_t count, off_t offset),
                   (descriptor, buffer, count, offset));
TGCLI_INTERPOSE_IO(int, close, (int descriptor), (descriptor));
TGCLI_INTERPOSE_IO(int, fsync, (int descriptor), (descriptor));
TGCLI_INTERPOSE_IO(int, poll, (pollfd * descriptors, nfds_t count, int timeout),
                   (descriptors, count, timeout));
TGCLI_INTERPOSE_IO(int, select,
                   (int descriptor_count, fd_set* read_set, fd_set* write_set, fd_set* error_set,
                    timeval* timeout),
                   (descriptor_count, read_set, write_set, error_set, timeout));
TGCLI_INTERPOSE_IO(ssize_t, send, (int socket, const void* buffer, std::size_t length, int flags),
                   (socket, buffer, length, flags));
TGCLI_INTERPOSE_IO(ssize_t, recv, (int socket, void* buffer, std::size_t length, int flags),
                   (socket, buffer, length, flags));

#undef TGCLI_INTERPOSE_IO
#undef TGCLI_INTERPOSE_SYNC
#undef TGCLI_INTERPOSE
