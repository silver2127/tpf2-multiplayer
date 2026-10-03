// wine_heap_bench.cpp -- does the Wine heap warm-up (native/src/wine_heap.h) cure
// the free-list search? A standalone reproduction, no game needed.
//
//   wine_heap_bench.exe [warm]        (run it under the same Wine/Proton as the game)
//
// 1. Fragment the process heap the way a big world load does: FRAG blocks of
//    1,300 bytes, each followed by a small pin that stays alive, then free the
//    1,300s. They cannot coalesce, so the heap's 1,280..1,535-byte free-list
//    bucket holds FRAG blocks that are all too small for the next step.
// 2. Churn: CHURN times allocate 1,500 bytes and free it, few alive at once --
//    what the simulation's short-lived vectors do. Without the front end, every
//    allocation searches past the whole bucket.
// With "warm", Tpf2mpWineHeapWarmup() runs first (forced on Windows too).
// Prints the churn's wall time and allocations per second. On Windows both runs
// are fast (the NT heap's front end is on by default); under Wine the cold run
// is the dedicated server's 1x cap.
//
// Build (from a VS developer prompt):  cl /O2 /EHsc /MT tools\wine_heap_bench.cpp /Fe:wine_heap_bench.exe
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../native/src/wine_heap.h"

int main(int argc, char** argv)
{
    const bool warm = argc > 1 && !strcmp(argv[1], "warm");
    const int FRAG = argc > 2 ? atoi(argv[2]) : 200000;
    const int CHURN = argc > 3 ? atoi(argv[3]) : 300000;
    HANDLE heap = GetProcessHeap();
    printf("wine: %s, mode: %s, fragments: %d, churn: %d\n", Tpf2mpUnderWine() ? "yes" : "no", warm ? "warm" : "cold", FRAG, CHURN);
    if (warm) {
        SetEnvironmentVariableW(L"TPF2MP_WINE_HEAP", L"1");
        const Tpf2mpWineHeapResult r = Tpf2mpWineHeapWarmup();
        printf("warm-up: %d heap(s), %d bin(s), %ld allocations, %lu ms\n", r.heaps, r.bins, r.allocations, r.ms);
    }
    std::vector<void*> frag(FRAG), pins(FRAG);
    for (int i = 0; i < FRAG; ++i) { frag[i] = HeapAlloc(heap, 0, 1300); pins[i] = HeapAlloc(heap, 0, 40); }
    for (int i = 0; i < FRAG; ++i) HeapFree(heap, 0, frag[i]);

    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    void* ring[8] = {};
    for (int i = 0; i < CHURN; ++i) {
        void*& slot = ring[i & 7];
        if (slot) HeapFree(heap, 0, slot);
        slot = HeapAlloc(heap, 0, 1500);
        if (slot) memset(slot, 1, 64);
    }
    QueryPerformanceCounter(&t1);
    const double s = double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
    printf("churn: %.3f s, %.0f allocations/s\n", s, CHURN / s);
    return 0;
}
