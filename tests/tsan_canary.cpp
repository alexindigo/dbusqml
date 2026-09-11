// tsan_canary.cpp — deliberately racy binary: the TSan CI gate's canary.
//
// The gate's pass condition is "grep finds no WARNING: ThreadSanitizer in
// the captured output" — a gate that can't fail is not a gate. CI runs this
// binary FIRST and requires the grep to FIND the warning, proving the
// detection pipeline (instrumentation, output capture, grep) is alive
// before the real selection's silence is trusted.
//
// Non-default target (EXCLUDE_FROM_ALL): built on demand by the sanitizer
// CI job only. Not a test; never installed.
#include <thread>

int main() {
    // Two threads hammering one unsynchronized int: under -fsanitize=thread
    // this MUST produce a data-race report.
    int shared = 0;
    std::thread a([&] {
        for (int i = 0; i < 200000; ++i)
            ++shared;
    });
    std::thread b([&] {
        for (int i = 0; i < 200000; ++i)
            ++shared;
    });
    a.join();
    b.join();
    // Read the racy value so the loops are not optimized away; the value
    // itself is meaningless.
    return shared < 0 ? 1 : 0;
}
