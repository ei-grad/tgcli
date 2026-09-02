#include "common/cancellation.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

// Global allocation replacement needs one process-wide failpoint.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> fail_allocations = false;

void* allocate(std::size_t size) {
    if (fail_allocations.load(std::memory_order_acquire)) {
        throw std::bad_alloc();
    }
    // Global new cannot use an allocating owner abstraction without recursion.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    if (auto* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

} // namespace

void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

void operator delete[](void* memory) noexcept {
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

void operator delete(void* memory, std::size_t unused_size) noexcept {
    static_cast<void>(unused_size);
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

void operator delete[](void* memory, std::size_t unused_size) noexcept {
    static_cast<void>(unused_size);
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

int main() {
    const tgcli::cancellation::Source source;
    int calls = 0;
    const tgcli::cancellation::Callback callback(source.get_token(), [&] { ++calls; });
    fail_allocations.store(true, std::memory_order_release);
    const bool stopped = source.request_stop();
    fail_allocations.store(false, std::memory_order_release);
    return stopped && calls == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
}
