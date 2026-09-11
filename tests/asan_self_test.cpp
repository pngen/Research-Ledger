// Deliberate AddressSanitizer self-test.
//
// The suite must be able to prove that the sanitizer it claims to run under is
// actually instrumenting this build. This program performs one known invalid
// heap access and is expected to be stopped by AddressSanitizer with a
// heap-buffer-overflow report. The build system registers it as a test whose
// pass criterion is that report, so a configuration in which the sanitizer is
// not active fails rather than passing silently.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__SANITIZE_ADDRESS__)
#define RESEARCH_LEDGER_ASAN_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RESEARCH_LEDGER_ASAN_ACTIVE 1
#endif
#endif

int main() {
#ifndef RESEARCH_LEDGER_ASAN_ACTIVE
    // Reached only if the target were built without instrumentation. Reporting
    // it explicitly is better than claiming sanitizer coverage that does not
    // exist.
    std::fprintf(stderr, "AddressSanitizer instrumentation is not active in this build\n");
    return 2;
#else
    constexpr std::size_t kBytes = 16;
    char* buffer = static_cast<char*>(std::malloc(kBytes));
    if (buffer == nullptr) {
        std::fprintf(stderr, "allocation failed\n");
        return 3;
    }
    std::memset(buffer, 0, kBytes);
    // One byte past the end of the allocation: AddressSanitizer must stop this.
    volatile char* out_of_bounds = buffer + kBytes;
    *out_of_bounds = 1;
    std::free(buffer);
    std::fprintf(stderr, "the deliberate out-of-bounds write was not detected\n");
    return 0;
#endif
}
