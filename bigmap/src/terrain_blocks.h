// Steam 35924: the terrain alignment pass's per-tile and per-region vectors
// go through the plugin's pagers instead of the heap.
//
// MEASURED 2026-09-17 with an ETW VirtualAllocation trace (LONGMAPSAVE) and
// in-process counters (LONGBOI), both 207,360-tile loads:
//   - the pass computes a work block per region through terrain_util::GetBlock
//     (0x3c4a20), whose vector<uint16_t>(n) constructor call at 0x3c4ba2
//     (return 0x3c4ba7) was hit ~10 million times on LONGBOI, ~99,000 times
//     with whole-tile blocks on LONGMAPSAVE;
//   - and resizes an alignment result vector per region at 0xaac4d9 inside
//     ecs::TerrainAlignmentSystem::UpdateSubterrains: 3.3 million times.
// All of them stay alive until the publication pass has copied each into its
// tile: the game's own 34 GiB private peak, written once and read once.
//
// Routing: both allocation sites go to SmallPager (any size up to the codec's
// maximum): reserved pages, committed on first touch, compressed and
// decommitted when the pool is over budget, restored on the next access.
// Their release comes through the CRT: the aligned delete reads the raw
// pointer at [-8] (the pagers store the span base there, as MSVC does) and
// calls free. free is imported (api-ms-win-crt-heap-l1-1-0!free), so its IAT
// slot (0x2f0b5b8) is pointed at a detour that returns arena pointers to the
// owning pager and passes everything else on unchanged.
#pragma once
#include "small_pager.h"
struct BlockVector { uint16_t *first, *last, *end; };
using BlockCtorFn = void(__fastcall*)(BlockVector*, size_t);
using FreeFn = void(__cdecl*)(void*);
static BlockCtorFn g_originalBlockCtor = nullptr;
static FreeFn g_originalFree = nullptr;
static uintptr_t g_blockBase = 0;
static const uintptr_t kBlockCtorRva = 0x310230, kBlockCtorReturn = 0x3c4ba7, kFreeIatRva = 0x2f0b5b8;
static const uint8_t kBlockCtorBytes[19] = {
    0x48, 0x89, 0x4c, 0x24, 0x08,                    // mov [rsp+8], rcx
    0x57,                                            // push rdi
    0x48, 0x83, 0xec, 0x30,                          // sub rsp, 0x30
    0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff   // mov qword [rsp+0x20], -2
};
static void __fastcall BlockCtorDetour(BlockVector* v, size_t n) {
    auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    InterlockedIncrement64(&g_blockCalls);
    if (ret == g_blockBase + kBlockCtorReturn && n && n <= SmallPager::MaxSamples &&
        InterlockedCompareExchange(&g_smallPagerActive, 0, 0)) {
        InterlockedIncrement64(&g_blockSized);
        if (auto p = SmallPager::Allocate(n)) {
            *v = {p, p + n, p + n};
            if (InterlockedIncrement64(&g_blockAllocations) == 1 && H) H->log("terrain blocks: first alignment work block routed to the small pager (%llu samples)", (unsigned long long)n);
            return;
        }
    }
    g_originalBlockCtor(v, n);
}
static void __cdecl FreeDetour(void* p) {
    if (p) {
        if (TerrainPager::Contains(p)) {
            if (TerrainPager::ReleaseAny(p)) { InterlockedIncrement64(&g_blockReleases); return; }
            InterlockedIncrement64(&g_blockStray); return;   // an arena address that is not a live slot: never hand it to the CRT
        }
        if (SmallPager::Contains(p)) {
            if (SmallPager::Release(p)) { InterlockedIncrement64(&g_blockReleases); return; }
            InterlockedIncrement64(&g_blockStray); return;
        }
    }
    g_originalFree(p);
}
static bool InstallTerrainBlocks() {
    if (!g_terrainBlocks || g_gog) return false;
    if (!InterlockedCompareExchange(&g_terrainCompressActive, 0, 0)) {
        H->log("terrain blocks: needs terrain_cache_compress=1; OFF"); return false;
    }
    if (!H->verifyBytes(kBlockCtorRva, kBlockCtorBytes, sizeof kBlockCtorBytes)) {
        H->log("terrain blocks: Steam byte mismatch at the block vector constructor; OFF"); return false;
    }
    if (!SmallPager::Init(size_t(g_smallHotMB) * 1024 * 1024)) {
        H->log("terrain blocks: small pager initialization failed; OFF"); return false;
    }
    g_blockBase = H->moduleBase();
    // The IAT slot must hold the CRT's free before it is replaced.
    auto slot = reinterpret_cast<void**>(g_blockBase + kFreeIatRva);
    auto ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto crtFree = ucrt ? reinterpret_cast<void*>(GetProcAddress(ucrt, "free")) : nullptr;
    if (!crtFree || *slot != crtFree) {
        H->log("terrain blocks: free import slot does not hold ucrtbase!free (%p vs %p); OFF", *slot, crtFree); return false;
    }
    g_originalFree = reinterpret_cast<FreeFn>(crtFree);
    void* detour = reinterpret_cast<void*>(FreeDetour);
    uint8_t bytes[8]; memcpy(bytes, &detour, 8);
    // free first: once the constructor hands out arena blocks, every release
    // must already be routed to the pager.
    if (!H->patchBytes(kFreeIatRva, bytes, 8)) { H->log("terrain blocks: free import patch failed; OFF"); return false; }
    if (!H->installHook(g_blockBase + kBlockCtorRva, reinterpret_cast<void*>(BlockCtorDetour), sizeof kBlockCtorBytes,
                        reinterpret_cast<void**>(&g_originalBlockCtor))) {
        uint8_t back[8]; memcpy(back, &crtFree, 8); H->patchBytes(kFreeIatRva, back, 8);
        H->log("terrain blocks: constructor hook failed; free import restored; OFF"); return false;
    }
    InterlockedExchange(&g_smallPagerActive, 1);
    H->log("terrain blocks: alignment work blocks (terrain_util::GetBlock) and result vectors (UpdateSubterrains) go through the small pager, %d MiB resident target; free import routed", g_smallHotMB);
    return true;
}
extern "C" __declspec(dllexport) void BigmapTestBlockCtor(BlockVector* v, size_t n, BlockCtorFn original) { g_originalBlockCtor = original; BlockCtorDetour(v, n); }
extern "C" __declspec(dllexport) void BigmapTestFree(void* p, FreeFn original) { g_originalFree = original; FreeDetour(p); }
extern "C" __declspec(dllexport) void BigmapTestBlockStats(uint64_t* out) { out[0] = g_blockAllocations; out[1] = g_blockReleases; out[2] = g_blockStray; }
extern "C" __declspec(dllexport) int BigmapTestSmallInit() { if (!SmallPager::Init(1 << 20)) return 0; InterlockedExchange(&g_smallPagerActive, 1); return 1; }
extern "C" __declspec(dllexport) void* BigmapTestSmallAllocate(size_t n) { return SmallPager::Allocate(n); }
