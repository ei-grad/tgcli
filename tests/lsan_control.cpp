#include <cstddef>

namespace {

[[gnu::noinline]] char* tgcli_owned_leak_control() {
    // The parent CTest requires this tgcli-owned allocation to remain visible to LSan.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return new char[17];
}

} // namespace

int main() {
    char* volatile leaked = tgcli_owned_leak_control();
    leaked[0] = 'x';
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    return leaked[0] == 'x' ? 0 : 1;
}
