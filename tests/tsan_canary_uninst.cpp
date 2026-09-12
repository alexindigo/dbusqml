// tsan_canary_uninst.cpp — the UNINSTRUMENTED helper DSO of the mixed
// canary (F-amended gate battery). Built WITHOUT -fsanitize=thread
// (-fno-sanitize=all in CMake) so its frames play the uninstrumented-Qt
// role: its racing access happens via the intercepted libc call
// (memmove) from an uninstrumented caller PC — the exact shape that killed
// option A in the tsan-blanket-spike (P2a swallowed 0/20 under
// ignore_noninstrumented_modules=1).
#include <cstdlib>
#include <cstring>

extern "C" {

// Tight loop of intercepted memmove into the shared buffer; source is a
// private static block (no overlap with the destination).
void uninst_memmove_writer(void *buf, size_t len, long iters) {
    static char *src = nullptr;
    if (!src) {
        src = static_cast<char *>(malloc(len));
        memset(src, 0x5a, len);
    }
    for (long i = 0; i < iters; ++i)
        memmove(buf, src, len);
}

} // extern "C"
