// tsan_canary_mixed.cpp — the MIXED canary of the F-amended gate battery:
// an instrumented thread's plain writes race the uninstrumented helper
// DSO's intercepted memmove. This is the spike's P2a shape — the race the
// rejected `ignore_noninstrumented_modules=1` flag swallowed 0/20. The
// verdict-layer classifier must see this report's instrumented frames and
// rule it OURS -> FAIL, forever. A zero-report run means a dead detector
// and fails the job itself.
//
// Expected: TSan reports a data race (write vs memmove) — the process
// exits 0 under exitcode=0 and the CLASSIFIER owns the verdict.
#include <atomic>
#include <cstdio>
#include <thread>

extern "C" void uninst_memmove_writer(void *buf, size_t len, long iters);

static char g_buf[4096];

int main() {
    std::atomic<bool> go{false};
    std::thread tU([&] {
        while (!go.load(std::memory_order_relaxed)) {
        }
        uninst_memmove_writer(g_buf, sizeof(g_buf), 200000);
    });
    std::thread tI([&] {
        while (!go.load(std::memory_order_relaxed)) {
        }
        for (long j = 0; j < 800000; ++j)
            g_buf[j & 4095] = static_cast<char>(j); // instrumented writes
    });
    go.store(true);
    tI.join();
    tU.join();
    return 0;
}
