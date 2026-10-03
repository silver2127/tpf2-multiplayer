// wine_heap.h -- under Wine/Proton, switch the process heaps' mid-size bins to
// Wine's low-fragmentation front end before the game allocates.
//
// MEASURED 2026-09-21 on the dedicated server (Proton 9, a 119 MB world, one
// joiner): the simulation thread spent 79% of its time on ONE loop in Wine's
// ntdll, find_free_block (dlls/ntdll/heap.c), walking a free list to satisfy
// requests of 1-2 KB (3,820 of 3,824 samples), and the session was capped at 1x.
// A quarter of the whole process's CPU was that loop.
//
// Why: Wine's heap has a lock-free front end (LFH) with bins up to 32 KB, but a
// bin is only switched on by bin_try_enable():
//     bins under 1 KB:   after 0x800 allocations, or 16 live blocks
//     bins 1 KB..32 KB:  only while MORE THAN 4 MiB of that bin's blocks are live
// The game churns short-lived 1-32 KB vectors: many allocations, few alive at
// once, so those bins never switch on, and every such allocation takes the heap
// lock and searches the one size-ordered free list, skipping every free block of
// the bucket that is a little too small. After a big load that list is long.
// HeapSetInformation(HeapCompatibilityInformation = 2) does not help: it sets the
// heap's mode, not a bin's `enabled` flag. The rule is the same in Proton 9,
// Proton 10 (bleeding-edge) and upstream Wine, so a newer Proton is no fix.
// Windows is not affected: the installer's Segment Heap switch covers it there.
//
// The fix uses the rule as written: for each of the 80 bins from 1 KB to 32 KB,
// hold a little over 4 MiB of blocks of that size alive at once, which flips the
// bin on, then free them. `enabled` is never cleared again, so from then on the
// game's allocations of those sizes are served by the front end without the lock
// and without the search. Cost: about 90,000 allocations and a 4 MiB transient
// per heap, once, before the game's entry point; measured in milliseconds.
//
// Wine only (ntdll exports wine_get_version). TPF2MP_WINE_HEAP=0 turns it off,
// =1 forces it on real Windows (harmless there; for testing the code path).
#pragma once
#include <windows.h>

struct Tpf2mpWineHeapResult { bool wine; bool ran; int heaps; int bins; long allocations; DWORD ms; };

inline bool Tpf2mpUnderWine()
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version");
}

// One heap: every bin from 1 KB to 32 KB. Returns the allocations made, 0 if the
// heap refused the first block (not growable, or out of memory: leave it alone).
inline long Tpf2mpWarmHeap(HANDLE heap, int* binsDone)
{
    // Wine's bin table above 1 KB: five ranges of 16 bins each
    // (BIN_SIZE_MIN_3..7 / BIN_SIZE_STEP_3..7 in heap.c).
    static const struct { SIZE_T lo, step; } ranges[] = {
        { 0x400, 0x40 }, { 0x800, 0x80 }, { 0x1000, 0x100 }, { 0x2000, 0x200 }, { 0x4000, 0x400 } };
    const SIZE_T kLive = 0x400000;   // bin_try_enable's threshold: 4 MiB of live blocks
    long total = 0;
    void** held = static_cast<void**>(HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * (kLive / 0x400 + 64)));
    if (!held) return 0;
    for (const auto& r : ranges) {
        for (int b = 0; b < 16; ++b) {
            // mid-bin user size: the block header and the 16-byte rounding stay inside the bin
            const SIZE_T size = r.lo + SIZE_T(b) * r.step + r.step / 2;
            const SIZE_T want = kLive / size + 32;
            SIZE_T n = 0;
            for (; n < want; ++n) { held[n] = HeapAlloc(heap, 0, size); if (!held[n]) break; }
            total += long(n);
            const bool full = n == want;
            while (n) HeapFree(heap, 0, held[--n]);
            if (!full) { HeapFree(GetProcessHeap(), 0, held); return total; }
            if (binsDone) ++*binsDone;
        }
    }
    HeapFree(GetProcessHeap(), 0, held);
    return total;
}

inline Tpf2mpWineHeapResult Tpf2mpWineHeapWarmup()
{
    Tpf2mpWineHeapResult res = {};
    res.wine = Tpf2mpUnderWine();
    wchar_t env[8] = L"";
    const DWORD got = GetEnvironmentVariableW(L"TPF2MP_WINE_HEAP", env, 8);
    if (got && env[0] == L'0') return res;
    if (!res.wine && !(got && env[0] == L'1')) return res;
    const DWORD t0 = GetTickCount();
    // Only heaps known to be serialized: the process heap, and the heap the game's
    // malloc uses (ucrtbase's _get_heap_handle; the same heap under Wine today).
    // Never the others GetProcessHeaps lists: a HEAP_NO_SERIALIZE heap belongs to
    // one thread, and allocating from it here would race its owner.
    HANDLE heaps[2] = { GetProcessHeap(), nullptr };
    if (HMODULE crt = GetModuleHandleW(L"ucrtbase.dll")) {
        using GetHeap = intptr_t (__cdecl*)();
        if (auto fn = reinterpret_cast<GetHeap>(GetProcAddress(crt, "_get_heap_handle"))) {
            HANDLE h = reinterpret_cast<HANDLE>(fn());
            if (h && h != heaps[0]) heaps[1] = h;
        }
    }
    for (HANDLE heap : heaps) {
        if (!heap) continue;
        int bins = 0;
        const long a = Tpf2mpWarmHeap(heap, &bins);
        if (a) { ++res.heaps; res.bins += bins; res.allocations += a; }
    }
    res.ran = true;
    res.ms = GetTickCount() - t0;
    return res;
}
