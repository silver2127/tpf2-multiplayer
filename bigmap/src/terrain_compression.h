// Steam 35924 integration. Only CTerrain's shared 257x257 uint16 vectors are
// eligible. Existing vectors, source heightmaps and rendering buffers stay on
// their stock allocators. Enable before loading; never retrofit live pointers.
#pragma once
#include "memory_status.h"
#include "terrain_pager.h"
#include "small_pager.h"
#include "terrain_warmup.h"
static int g_terrainCompress=0, g_terrainHotMB=1024;
static int g_terrainWarmMB=4096;
static int g_terrainMaxMB=0;   // terrain_cache_max_mb: hard cap on the resident target, 0 = auto (PagerCapMB), -1 = none
// Measurement stage of docs/terrain-cow-sharing.md: share the section between
// the two CTerrain versions instead of copying, privatizing on first write.
static int g_terrainCowShare=0;
// Is the COW copy hook even reached during a load? The first measured run shared
// nothing, and `1dedd0` only runs when the detach at `33dd20` finds refs > 1.
static volatile LONG64 g_cowCopyCalls=0, g_cowCopyUnmanaged=0;
// Content-dedup probe (measurement only): hash every live tile and log how many
// are byte-identical to another. Every 10 s while loading, every 2 min otherwise.
static int g_terrainDedupProbe=0;
// Content dedup: an eviction whose bytes match a stored blob shares it instead
// of encoding (see pager_impl.inl). Measured need: the two CTerrain versions of
// a save load are byte-identical tile for tile.
static int g_terrainDedup=0;
// Fresh tiles start without a section (see TerrainPager::Allocate).
static int g_terrainLazyZero=0;
// The alignment pass's per-tile vectors go through the pager too (terrain_blocks.h).
static int g_terrainBlocks=0;
static volatile LONG64 g_blockAllocations=0, g_blockReleases=0, g_blockStray=0;
// Diagnostics for the block detour: every call, calls routed by size and
// caller, and resize calls returning to UpdateSubterrains.
static volatile LONG64 g_blockCalls=0, g_blockSized=0, g_resultResizes=0;
// The small pager (small_pager.h) behind terrain_blocks: its resident budget
// in MiB and the switch the hooks read.
static int g_smallHotMB=1024;
static volatile LONG g_smallPagerActive=0;
// Ceiling on evictions per second outside loading and memory pressure
// (0 = unlimited). The rate actually used adapts below it, see EvictRateStep.
static int g_terrainEvictPerSec=4000, g_materialEvictPerSec=4000;
// Adaptive eviction rate, stepped once per second by each pager's worker.
// Two signals, so a slow machine evicts less and a fast one more, and neither
// notices it:
// - Cost: `avgMicros` is the mean wall time of one eviction (protect, encode,
//   unmap) measured in the last second. Evictions get a quarter of one core:
//   the cap is 250 ms of that work per second, never below 100 evictions/s.
// - Stutter: `stalls` is the number of gameplay-frame gaps of 60 ms or more
//   seen in the last second (UiStallMeter, from the menu DLL's last-frame
//   stamp; absent without the multiplayer DLL). A stall halves the rate; after
//   five stall-free seconds it grows by a quarter per second back to the cap.
// `cost` is a running average of the measured cost (three parts old, one
// new), kept across seconds with no evictions: MEASURED 2026-09-17, without
// it every quiet second let the rate climb to the ceiling and the next busy
// second started with a burst at 4,000/s against a measured 1,250/s cap.
struct EvictRateState {unsigned rate,quiet;uint64_t cost;};
static unsigned EvictRateStep(EvictRateState* st,uint64_t avgMicros,unsigned stalls,bool uiSignal,unsigned ceiling) {
    if(!ceiling)return 0;
    if(!st->rate)st->rate=ceiling<1000?ceiling:1000;
    if(avgMicros)st->cost=st->cost?(st->cost*3+avgMicros)/4:avgMicros;
    unsigned costCap=st->cost?unsigned(250000ull/st->cost):ceiling;
    if(costCap<100)costCap=100;
    unsigned cap=costCap<ceiling?costCap:ceiling;
    if(uiSignal && stalls){st->rate=st->rate/2>100?st->rate/2:100;st->quiet=0;}
    else if(++st->quiet>5){unsigned grow=st->rate/4>50?st->rate/4:50;st->rate+=grow;}
    if(st->rate>cap)st->rate=cap;
    if(st->rate<100)st->rate=100;
    return st->rate;
}
// Samples the menu DLL's last-gameplay-frame stamp (GetTickCount64 at each
// CGameUI update, 0 outside a world) every 25 ms. Two consecutive observed
// stamps 60 ms or more apart mean a frame took at least that long.
struct UiStallMeter {
    uint64_t last=0;unsigned stalls=0;
    void Sample(uint64_t stamp) {
        if(stamp && last && stamp!=last && stamp-last>=60)++stalls;
        last=stamp;
    }
    unsigned Take(){unsigned n=stalls;stalls=0;return n;}
};
// Stalls seen by the terrain worker in its last second, for the material worker.
static volatile LONG g_uiStallsLastSec=0;
static void ProbeYield(){TerrainPager::Tick();}
static void LogDedupProbe() {
    TerrainPager::ProbeResult r{};
    if(!TerrainPager::Probe(&r,ProbeYield)){H->log("terrain dedup probe: table allocation failed");return;}
    H->log("terrain dedup probe: live=%llu hashed=%llu (resident=%llu cold=%llu) skipped=%llu distinct=%llu duplicates=%llu zero_tiles=%llu pairs=%llu largest_group=%llu low_half=%llu ms=%llu",
           r.live,r.hashedResident+r.hashedPacked,r.hashedResident,r.hashedPacked,r.skipped,r.distinct,r.duplicated,r.zero,r.pairs,r.largestGroup,r.lowHalf,r.ms);
}
static volatile LONG g_terrainCompressActive=0;
struct TerrainOwnedVector {uint16_t *first,*last,*end;};
using TerrainResizeFn=void(__fastcall*)(TerrainOwnedVector*,size_t);
using TerrainCopyFn=TerrainOwnedVector*(__fastcall*)(TerrainOwnedVector*,const TerrainOwnedVector*);
using TerrainDestroyFn=void(__fastcall*)(void*);
static TerrainResizeFn g_originalTerrainResize;
static TerrainCopyFn g_originalTerrainCopy;
static TerrainDestroyFn g_originalTerrainDestroy;
static uintptr_t g_terrainCompressionBase;
static void ResizeTerrainOwned(TerrainOwnedVector* v,size_t n,bool eligible,bool toSmall=false) {
    using namespace TerrainPager;
    if(SmallPager::Contains(v->first)) {
        // A small-pager span: grow within its page capacity, else migrate to
        // the stock heap before the engine's own code can see our memory.
        size_t oldSize=size_t(v->last-v->first);
        if(n<=SmallPager::Capacity(v->first)){v->last=v->first+n;if(n>oldSize)memset(v->first+oldSize,0,(n-oldSize)*2);return;}
        TerrainOwnedVector replacement{};
        g_originalTerrainResize(&replacement,n);
        memcpy(replacement.first,v->first,oldSize*2);
        SmallPager::Release(v->first);*v=replacement;return;
    }
    if(toSmall && !v->first && !v->last && !v->end && n && n<=SmallPager::MaxSamples) {
        if(auto p=SmallPager::Allocate(n)){*v={p,p+n,p+n};return;}
    }
    if(Contains(v->first)) {
        size_t oldSize=size_t(v->last-v->first);
        if(n<=Samples){v->last=v->first+n; if(n>oldSize)memset(v->first+oldSize,0,(n-oldSize)*2);return;}
        // A future engine path grows this vector: migrate to the stock heap
        // before its normal reallocation/free code can see our backing memory.
        TerrainOwnedVector replacement{};
        g_originalTerrainResize(&replacement,n);
        memcpy(replacement.first,v->first,oldSize*2);
        Release(v->first);*v=replacement;return;
    }
    if(eligible && !v->first && !v->last && !v->end && n==Samples) {
        if(auto p=Allocate()){*v={p,p+n,p+n};return;}
    }
    g_originalTerrainResize(v,n);
}
static void __fastcall TerrainCompressedResize(TerrainOwnedVector* v,size_t n) {
    // 0x33ccaa: CTerrain::AddTile, the height cache itself. 0xaac4d9: the
    // alignment result vector in ecs::TerrainAlignmentSystem::UpdateSubterrains
    // (MEASURED 2026-09-17: 37,354 of them, 6.7 GiB, all alive at the load's
    // peak); a plain vector, released through the CRT free import that
    // terrain_blocks.h routes to the pager, so only with terrain_blocks on.
    auto ret=reinterpret_cast<uintptr_t>(_ReturnAddress());
    bool result=ret==g_terrainCompressionBase+0xaac4d9;
    if(result)InterlockedIncrement64(&g_resultResizes);
    bool eligible=InterlockedCompareExchange(&g_terrainCompressActive,0,0) && ret==g_terrainCompressionBase+0x33ccaa;
    bool toSmall=result && InterlockedCompareExchange(&g_smallPagerActive,0,0);
    ResizeTerrainOwned(v,n,eligible,toSmall);
}
static TerrainOwnedVector* CopyTerrainOwned(TerrainOwnedVector* dst,const TerrainOwnedVector* src,bool eligible) {
    if(eligible && src->first && uintptr_t(src->last)-uintptr_t(src->first)==TerrainPager::Bytes) {
        if(g_terrainCowShare) {
            InterlockedIncrement64(&g_cowCopyCalls);
            if(!TerrainPager::Contains(src->first))InterlockedIncrement64(&g_cowCopyUnmanaged);
            // Map the source's section a second time rather than copying 132 KiB.
            // Both views become read-only; whichever version is written first
            // takes a private copy. Falls through to the eager paths on refusal.
            if(auto p=TerrainPager::Share(src->first)) {
                *dst={p,p+TerrainPager::Samples,p+TerrainPager::Samples};return dst;
            }
        }
        if(auto p=TerrainPager::Clone(src->first)) {
            // Immutable compressed bytes can be shared between independently
            // owned vectors. First write restores a private backing section.
            *dst={p,p+TerrainPager::Samples,p+TerrainPager::Samples};return dst;
        }
        if(auto p=TerrainPager::Allocate()) {
            // COW source may be compressed; the ordinary memcpy faults back in
            // without changing source bytes or shared ownership.
            memcpy(p,src->first,TerrainPager::Bytes);
            *dst={p,p+TerrainPager::Samples,p+TerrainPager::Samples};return dst;
        }
    }
    return g_originalTerrainCopy(dst,src);
}
static TerrainOwnedVector* __fastcall TerrainCompressedCopy(TerrainOwnedVector* dst,const TerrainOwnedVector* src) {
    bool eligible=InterlockedCompareExchange(&g_terrainCompressActive,0,0) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress())==g_terrainCompressionBase+0x33dd91;
    return CopyTerrainOwned(dst,src,eligible);
}
static void __fastcall TerrainCompressedDestroy(void* control) {
    auto v=reinterpret_cast<TerrainOwnedVector*>(static_cast<uint8_t*>(control)+0x10);
    if(TerrainPager::Release(v->first)){*v={};return;}
    g_originalTerrainDestroy(control);
}
// Budgets sized from installed RAM when the cfg value asks for it (hot 0,
// warm -1). The tuned 3072/4096 MiB pair suits a 94 GiB machine; the same
// fractions give ~1 GiB/2.6 GiB on 32 GiB and ~546/1365 MiB on 16 GiB, where
// fixed values would push the game into paging.
static int AutoBudgetMB(uint64_t totalBytes,int divisor,int lo,int hi) {
    uint64_t mb=(totalBytes>>20)/uint64_t(divisor);
    if(mb<uint64_t(lo))mb=uint64_t(lo);
    if(mb>uint64_t(hi))mb=uint64_t(hi);
    return int(mb);
}
static void AutoTerrainBudgets(uint64_t totalBytes,int* hot,int* warm) {
    int h=AutoBudgetMB(totalBytes,30,256,4096);
    *hot=h;*warm=AutoBudgetMB(totalBytes,12,h,8192);
}
static uint64_t InstalledPhysicalBytes() {
    MEMORYSTATUSEX m{};m.dwLength=sizeof m;
    return PagerMemoryStatus(&m)?m.ullTotalPhys:0;   // the simulated size when simulate_physical_mb is set
}
// Headroom the pagers keep free for the OS and the engine, sized to the
// machine: physical/7, clamped to 2..12 GiB. 12 GiB was MEASURED as the need
// on a 94 GiB box with no page file (the engine's load burst); a flat 12 GiB
// on a 32 GiB machine (4.6 GiB here) left the pagers no room at all once a
// big save was loaded (2026-09-17: 19.4 GB in the game, awful performance).
// An unknown size (0) keeps the measured 12 GiB.
static uint64_t PagerHeadroom(uint64_t physical) {
    constexpr uint64_t GiB=1024ull*1024*1024;
    if(!physical)return 12*GiB;
    uint64_t h=physical/7;
    if(h<2*GiB)h=2*GiB;
    if(h>12*GiB)h=12*GiB;
    return h;
}
// Free commit below which the pagers back off hard (256 MiB, urgent, throttled):
// physical/8, clamped to 2..10 GiB. 10 GiB was MEASURED on the 94 GiB box (6
// came too late, before the alignment pass was batched); a 32 GiB machine with
// a system-managed page file rarely has 10 GiB of free commit with a big save
// loaded and would sit throttled for the whole session. Unknown size: 10 GiB.
static uint64_t CommitTightBytes(uint64_t physical) {
    constexpr uint64_t GiB=1024ull*1024*1024;
    if(!physical)return 10*GiB;
    uint64_t t=physical/8;
    if(t<2*GiB)t=2*GiB;
    if(t>10*GiB)t=10*GiB;
    return t;
}
// The automatic resident cap, the same on every machine that can afford it:
// terrain physical/4 clamped to 4..8 GiB (16 GiB: 4, 32 GiB and up: 8),
// material a quarter of that. Without a cap the steady policy fills RAM
// down to the headroom, which is what keeps a big map smooth -- and what
// made the game 20 GB on a 32 GiB machine (2026-09-20). MEASURED the same
// day with the policy simulating 32 GiB on a freshly generated big map: a
// 4 GiB cap sat under the engine's working set -- 370-512 cold restores/s
// steady with spikes to 5,971, the log's own "working set exceeds the
// budget" -- while the game itself was 15 GiB private before the pager
// held a byte. So the map is most of the 20 GB; the pager can only be
// capped where its working set fits, and 8 GiB holds this one. The price
// of any cap is decodes (`cold restores/s`). hot stays the floor.
static int PagerCapMB(int configured,int hot,uint64_t physical,unsigned shareQuarters=4) {
    constexpr uint64_t GiB=1024ull*1024*1024;
    if(configured<0)return 0;                       // -1: no cap
    uint64_t cap;
    if(configured>0)cap=uint64_t(configured);
    else {
        uint64_t autoBytes=physical?physical/4:8*GiB;
        if(autoBytes>8*GiB)autoBytes=8*GiB;
        if(autoBytes<4*GiB)autoBytes=4*GiB;
        cap=((autoBytes>>20)*shareQuarters)/4;
    }
    return int(cap>uint64_t(hot)?cap:uint64_t(hot));
}
// The commit-tight throttle with hysteresis. Raw `tight` is free commit under
// CommitTightBytes; once tight, this stays tight until free commit clears the
// threshold by more than this pager itself gives back when throttled (its
// hot target minus the 256 MiB floor, plus 1 GiB), and for at least 30 s.
// MEASURED 2026-09-20 on the 94 GiB rig with 9 GiB of free commit under a
// 10 GiB threshold: the pager throttled to 256 MiB, its own 3 GiB release
// cleared the threshold, it re-expanded, and the flag set again -- 74 flips
// in one session, each one evicting and re-inflating the terrain in front of
// the camera: the zoomed-in stutter.
struct CommitTightState { bool tight=false; ULONGLONG since=0; };
static bool CommitTightSticky(CommitTightState& st,bool raw,uint64_t availPageFile,uint64_t threshold,uint64_t releaseBytes,ULONGLONG now) {
    if(raw){ if(!st.tight)st.since=now; st.tight=true; return true; }
    if(st.tight) {
        if(availPageFile<threshold+releaseBytes || now-st.since<30000)return true;
        st.tight=false;
    }
    return false;
}
static int TerrainBudgetMB(int hot,int warm,bool busy,bool bulk,uint64_t available,uint64_t liveMB=0,uint64_t physical=0) {
    // Memory pressure overrides the warmup allowance. Available RAM is sampled
    // outside the pager lock; this is a conservative policy, not an allocation.
    constexpr uint64_t GiB=1024ull*1024*1024;
    if(!((busy||bulk) && available>=4*GiB && warm>hot))return hot;
    // While loading, keep every live allocation resident when RAM allows. A
    // 256x256 save reload with a 4 GiB allowance decoded ~290k terrain tiles on
    // the loading threads (the loader re-reads what was just evicted). The
    // target never exceeds available RAM minus 12 GiB, so it shrinks by itself
    // as memory fills, and never drops below the configured warm allowance.
    // Keep half of what is free, at least 12 GiB, for everything else. A quarter
    // (2 GiB floor) was MEASURED insufficient on 2026-09-17: on a 94 GiB machine
    // with no page file (commit limit = RAM) and ~44 GiB committed elsewhere,
    // a 103,680-tile load held ~20 GiB of sections and the engine's own
    // allocations then failed with the game's "Out of memory" assert, twice.
    // Sections commit in full at creation and cannot be freed faster than
    // they encode, so the room has to be left before the engine's burst.
    // The floor is the machine's headroom (PagerHeadroom: 12 GiB here, 4.6 GiB
    // on 32 GiB): a flat 12 GiB left 16 and 32 GiB machines with no allowance.
    uint64_t headroom=PagerHeadroom(physical);
    uint64_t reserve=available/2>headroom?available/2:headroom;
    uint64_t capMB=available>reserve?(available-reserve)>>20:0;
    uint64_t want=liveMB>uint64_t(warm)?liveMB:uint64_t(warm);
    uint64_t floor=capMB>uint64_t(warm)?capMB:uint64_t(warm);
    uint64_t target=want<floor?want:floor;
    if(target>65536)target=65536;
    return int(target>uint64_t(hot)?target:uint64_t(hot));
}
// Steady state (not loading, commit not tight), once per second. `next` is the
// policy's own target for this second (the hot budget), `prev` the target in
// force. MEASURED 2026-09-17 on a 103,680-tile world: when the load ended the
// target snapped from 13.4 GiB to the 3.2 GiB hot budget, the pager evicted
// 67,000 tiles inside a minute, and the engine, which keeps ~36,000 tiles
// (4.6 GiB) in use on that map, faulted 5,900 evicted tiles per second back in
// through a decode each: visible freezes. Three rules replace the snap:
// - Ramp: the target drops by at most 1/16 of itself (>= 64 MiB) per second.
// - Stutter feedback: `decodesPerSec` is the number of cold restores in the
//   last second. At >= 300 the engine is re-reading what was just evicted:
//   grow by 1/8 (>= 128 MiB). At >= 100 hold. Below that, drift down.
// - Ceiling: the hot budget plus a share (terrain 3/4, material 1/2) of the
//   room above a reserve, where room = free RAM (`available` = min(free RAM,
//   free commit)) plus what this pager already holds, and the reserve is a
//   quarter of that room or the machine's headroom (PagerHeadroom), whichever
//   is larger; never above 65536 MiB.
// - Pressure: free RAM under the headroom shrinks by 1/8 per second.
// - Working-set floor (`wsFloor`, the caller's state): the target the stutter
//   feedback drove this pager to is remembered and the drift never goes below
//   it; it decays by 1/256 per quiet second (halves in about three minutes).
//   MEASURED 2026-09-20 with the policy simulating 32 GiB (hot 1092 MiB) on a
//   freshly generated map: without it the target sawtoothed between 1.4 and
//   1.6 GiB under 1,000-2,100 cold restores/s -- each quiet second drifted
//   1/16 toward hot, the next burst grew 1/8, and the engine faulted the same
//   tiles back in over and over. A real 32 GiB machine runs exactly that.
// The result never goes below `next`, so the configured budget stays a floor.
static int TerrainBudgetSteady(int prev,int next,int hot,uint64_t decodesPerSec,uint64_t available,uint64_t physical=0,unsigned shareQuarters=2,int* wsFloor=nullptr) {
    uint64_t headroom=PagerHeadroom(physical);
    // RAM this pager could own: what is free now PLUS what it holds itself.
    // Counting free RAM alone starved the pager on a full machine: its own
    // resident set made "free" small, the ceiling fell to the hot budget, the
    // pager evicted, and the engine faulted the evicted tiles back in through
    // a decode each (the 32 GiB case of 2026-09-17).
    uint64_t room=available+(uint64_t(prev)<<20);
    uint64_t reserve=room/4>headroom?room/4:headroom;
    // The terrain pager takes 3/4 of the spare, the material pager 1/2 (their
    // sum overshoots by a quarter at most; the pressure rule below takes it back).
    uint64_t spareMB=room>reserve?((room-reserve)>>20)*shareQuarters/4:0;
    uint64_t ceil=uint64_t(hot)+spareMB;if(ceil>65536)ceil=65536;
    int ceiling=int(ceil);
    int target=next;
    if(decodesPerSec>=300){int grow=prev/8>128?prev/8:128;target=prev+grow;}
    else if(decodesPerSec>=100){target=prev>next?prev:next;}
    else if(next<prev){int step=prev/16>64?prev/16:64;target=prev-step>next?prev-step:next;}
    // Memory pressure: free RAM under the headroom shrinks the target by 1/8
    // per second whatever the decode feedback says. Paging the engine out is
    // worse than any decode.
    if(available<headroom){int cut=prev/8>128?prev/8:128;int forced=prev-cut>next?prev-cut:next;if(target>forced)target=forced;}
    if(target>ceiling)target=ceiling;
    if(wsFloor) {
        if(decodesPerSec>=300 && target>*wsFloor)*wsFloor=target;
        else if(decodesPerSec<100 && *wsFloor>0)*wsFloor-=(*wsFloor/256>1?*wsFloor/256:1);
        if(target<*wsFloor && *wsFloor<=ceiling)target=*wsFloor;
    }
    if(target<next)target=next;
    return target;
}
// Extra eviction threads: encoding runs outside the pool lock, so several
// threads keep up with bulk allocation during loading. Policy and logging stay
// on the main worker.
static unsigned PagerHelperThreads() {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    return info.dwNumberOfProcessors>=16?3:info.dwNumberOfProcessors>=8?1:0;
}
// Defined in terrain_serve.h (included later): the sidecar counters for the 30 s line.
static int TerrainServeStatus(char* out, size_t cap);
static DWORD WINAPI TerrainEvictionHelper(void*) {
    for(;;) {
        Sleep(25);
        if(InterlockedCompareExchange(&g_terrainCompressActive,0,0))TerrainPager::Tick();
        if(InterlockedCompareExchange(&g_smallPagerActive,0,0))SmallPager::Tick();
    }
}
static DWORD WINAPI TerrainCompressionWorker(void*) {
    uint64_t lastLog=GetTickCount64(),lastPolicy=0;int effectiveMB=g_terrainHotMB;
    uint64_t lastProbe=0;bool probeFast=false;
    uint64_t lastDecodes=0,lastStutterLog=0,lastRateLog=0;int smallEffectiveMB=g_smallHotMB;
    uint64_t lastEvictMicros=0,lastEvictOps=0;EvictRateState rate{};UiStallMeter stallMeter;
    TerrainWarmup warmup;
    using UiTickFn=uint64_t(*)();UiTickFn uiTick=nullptr;
    for(;;) {
        Sleep(25);
        if(!InterlockedCompareExchange(&g_terrainCompressActive,0,0))continue;
        auto now=GetTickCount64();
        if(uiTick)stallMeter.Sample(uiTick());
        if(now-lastPolicy>=1000) {
            lastPolicy=now;auto s=TerrainPager::Snapshot();MEMORYSTATUSEX m{};m.dwLength=sizeof m;
            // Pager sections are page-file-backed: they count against the system
            // commit limit (RAM + page file) exactly like private memory. Use the
            // smaller of free RAM and free commit, and back off hard when commit is
            // nearly exhausted (a 512x512 desert preview hit std::bad_alloc at the
            // 114.6 GB commit limit while tiles were held uncompressed).
            bool haveStatus=PagerMemoryStatus(&m)!=0;
            uint64_t available=haveStatus?(m.ullAvailPhys<m.ullAvailPageFile?m.ullAvailPhys:m.ullAvailPageFile):0;
            static const uint64_t physical=InstalledPhysicalBytes();
            // Sized to the machine (CommitTightBytes): 10 GiB here, 4 GiB on 32 GiB;
            // sticky (CommitTightSticky), so this pager's own release cannot clear it.
            static CommitTightState tightState;
            bool commitTight=haveStatus && CommitTightSticky(tightState,m.ullAvailPageFile<CommitTightBytes(physical),m.ullAvailPageFile,
                                                            CommitTightBytes(physical),(uint64_t(g_terrainHotMB>256?g_terrainHotMB-256:0)<<20)+(1ull<<30),now);
            // Free RAM under the machine's headroom: shrink and evict urgently.
            bool pressure=haveStatus && m.ullAvailPhys<PagerHeadroom(physical);
            bool busy=InterlockedCompareExchange(&g_worldEntryActive,0,0)!=0;
            bool bulk=s.lastBulkAllocation && now-s.lastBulkAllocation<15000;
            if(!uiTick) {
                if(auto menu=GetModuleHandleW(L"tpf2_menu.dll"))
                    uiTick=reinterpret_cast<UiTickFn>(GetProcAddress(menu,"Tpf2mpLastGameUiTick"));
            }
            bool loading=warmup.Update(now,busy,s.lastBulkAllocation,uiTick?uiTick():0,uiTick!=nullptr);
            // Growth with the live size while loading, capped by the smaller of free
            // RAM and free commit (above). Measured on the same 256x256 save: with a
            // fixed 4 GiB allowance the loading thread spent 69-71% of two profile
            // windows restoring tiles (85 s); with growth 41-53% (73 s). The earlier
            // RAM-only cap hit std::bad_alloc at the commit limit on a 512x512 preview.
            int next=TerrainBudgetMB(g_terrainHotMB,g_terrainWarmMB,busy,bulk||loading,available,(s.live*TerrainPager::SlotBytes)>>20,physical);
            // Cold restores in the last second: faults minus the ones a
            // protection change alone satisfied.
            uint64_t decodes=s.faults-s.softRescues,decodesPerSec=decodes-lastDecodes;lastDecodes=decodes;
            TerrainPager::SetUrgent(commitTight||pressure);
            // THE THROTTLE IS FOR LOAD BURSTS (2026-09-22). It makes a fault that needs a
            // new section sleep, up to 2 s, while the pool is over budget -- the answer to
            // a load creating ~30,000 sections a second. On a running or paused world it
            // only froze the game: a PC at 80 of 94 GB committed (no page file, other
            // programs holding most of it) kept commit_tight on, and every tile the camera
            // needed waited ~2 s (throttle_waits=1957, throttle_ms=3,862,828 on a live-join
            // host that looked hung). Eviction stays urgent under a tight commit charge;
            // only the sleep is limited to loading.
            TerrainPager::SetThrottle(commitTight && (busy||bulk||loading));
            if(InterlockedCompareExchange(&g_smallPagerActive,0,0)) {
                // No loading allowance here: MEASURED 2026-09-17, with the tile
                // pager's allowance the small pager kept 2.28 million spans
                // (26.7 GiB) resident during the load, which is the peak it exists
                // to remove. The budget is the fixed floor; a tight commit charge
                // drains it to 64 MiB.
                int smallNext=commitTight?64:g_smallHotMB;
                SmallPager::SetBudget(size_t(smallNext)*1024*1024);
                SmallPager::SetThrottle(commitTight && (busy||bulk||loading));
                if(smallNext!=smallEffectiveMB)H->log("small pager: resident target %d -> %d MiB (commit_tight=%d)",smallEffectiveMB,smallNext,int(commitTight));
                smallEffectiveMB=smallNext;
            }
            if(commitTight && next>256)next=256;
            else if(!(busy||bulk||loading)) {
                static int wsFloor=0;
                int steady=TerrainBudgetSteady(effectiveMB,next,g_terrainHotMB,decodesPerSec,available,physical,3,&wsFloor);
                if(steady>effectiveMB && decodesPerSec>=300 && now-lastStutterLog>=10000) {
                    lastStutterLog=now;
                    H->log("terrain compression: %llu cold restores/s, resident target %d -> %d MiB (working set exceeds the budget)",decodesPerSec,effectiveMB,steady);
                }
                next=steady;
            }
            // The cap (configured, or the machine-independent automatic one) wins
            // over every allowance: RAM for decodes, the user's trade; hot stays
            // the floor.
            { int cap=PagerCapMB(g_terrainMaxMB,g_terrainHotMB,physical,4); if(cap && next>cap)next=cap; }
            TerrainPager::SetBudget(size_t(next)*1024*1024);
            if(next!=effectiveMB && (next==g_terrainHotMB||effectiveMB==g_terrainHotMB||next-effectiveMB>=1024||effectiveMB-next>=1024))H->log("terrain compression: resident target %d -> %d MiB (generation=%d bulk_allocation=%d loading_tail=%d ui_signal=%d commit_tight=%d pressure=%d free=%llu MiB)",effectiveMB,next,int(busy),int(bulk),int(loading),int(uiTick!=nullptr),int(commitTight),int(pressure),(unsigned long long)(m.ullAvailPhys>>20));
            effectiveMB=next;
            probeFast=busy||bulk||loading;
            // Adaptive eviction rate from last second's cost and frame stalls.
            uint64_t ops=s.evictOps-lastEvictOps,micros=s.evictMicros-lastEvictMicros;lastEvictOps=s.evictOps;lastEvictMicros=s.evictMicros;
            unsigned stalls=stallMeter.Take();InterlockedExchange(&g_uiStallsLastSec,LONG(stalls));
            unsigned before=rate.rate;
            unsigned perSec=EvictRateStep(&rate,ops?micros/ops:0,stalls,uiTick!=nullptr,g_terrainEvictPerSec<0?0:unsigned(g_terrainEvictPerSec));
            TerrainPager::SetEvictRate(perSec);
            if(perSec && perSec<before && stalls && now-lastRateLog>=10000) {
                lastRateLog=now;
                H->log("terrain compression: %u frame stall(s) >= 60 ms, eviction rate %u -> %u/s (last second: %llu evictions, %llu us each)",stalls,before,perSec,ops,ops?micros/ops:0ull);
            }
        }
        TerrainPager::Tick();
        if(InterlockedCompareExchange(&g_smallPagerActive,0,0))SmallPager::Tick();
        if(g_terrainDedupProbe && now-lastProbe>=(probeFast?10000ull:120000ull)) {
            lastProbe=now;
            if(TerrainPager::Snapshot().live)LogDedupProbe();
        }
        if(now-lastLog>=30000) {
            lastLog=now;auto s=TerrainPager::Snapshot();
            char serve[448];TerrainServeStatus(serve,sizeof serve);
            if(s.live)H->log("terrain compression: live=%llu resident=%llu backing=%.1f MiB compressed=%.1f MiB encoded_commit=%.1f MiB faults=%llu evictions=%llu failures=%llu encodes=%llu reused=%llu writes=%llu shared_clones=%llu slot_overflows=%llu soft_blocked=%llu soft_rescues=%llu cancelled=%llu cow_shared=%llu cow_slots=%llu cow_privatized=%llu cow_privatize_mb=%.1f dedup_hits=%llu dedup_rebuilds=%llu restore_retries=%llu restore_giveups=%llu rate_limited=%llu evict_rate=%u/s evict_us=%llu lazy=%llu throttle_waits=%llu throttle_ms=%llu blocks=%lld block_releases=%lld block_stray=%lld block_calls=%lld block_sized=%lld result_resizes=%lld%s",
                s.live,s.resident,double(s.resident*TerrainPager::SlotBytes)/(1024*1024),
                double(s.compressedBytes)/(1024*1024),double(s.compressedCommit)/(1024*1024),s.faults,s.evictions,s.failures,
                s.encodes,s.reusedEvictions,s.writeFaults,s.sharedClones,s.overflows,s.softBlocked,s.softRescues,s.cancelledEvictions,
                s.sharedViews,s.sharedSlots,s.privatizations,double(s.privatizeBytes)/(1024*1024),s.dedupHits,s.dedupRebuilds,s.restoreRetries,s.restoreGiveUps,s.rateLimited,rate.rate,s.evictOps?s.evictMicros/s.evictOps:0ull,s.lazyAllocations,s.throttleWaits,s.throttleMillis,
                InterlockedCompareExchange64(&g_blockAllocations,0,0),InterlockedCompareExchange64(&g_blockReleases,0,0),InterlockedCompareExchange64(&g_blockStray,0,0),
                InterlockedCompareExchange64(&g_blockCalls,0,0),InterlockedCompareExchange64(&g_blockSized,0,0),InterlockedCompareExchange64(&g_resultResizes,0,0),serve);
            if(InterlockedCompareExchange(&g_smallPagerActive,0,0)) {
                auto sp=SmallPager::Snapshot();
                if(sp.live||sp.releases)H->log("small pager: live=%llu lazy=%llu resident=%llu resident_mb=%.1f cold=%llu compressed_mb=%.1f faults=%llu commits=%llu restores=%llu evictions=%llu releases=%llu failures=%llu incompressible=%llu cancelled=%llu commit_retries=%llu throttle_waits=%llu throttle_ms=%llu overflows=%llu ring_drops=%llu",
                    sp.live,sp.lazy,sp.resident,double(sp.residentBytes)/(1024*1024),sp.cold,double(sp.compressedBytes)/(1024*1024),sp.faults,sp.commits,sp.restores,sp.evictions,sp.releases,sp.failures,sp.incompressible,sp.cancelled,sp.commitRetries,sp.throttleWaits,sp.throttleMillis,sp.overflows,sp.ringDrops);
            }
            if(g_terrainCowShare)H->log("terrain cow: copy_hook_calls=%lld unmanaged_src=%lld shared=%llu refused_not_slot=%llu refused_cold=%llu refused_packed=%llu refused_busy=%llu",
                g_cowCopyCalls,g_cowCopyUnmanaged,s.sharedViews,
                s.shareRefusedNotSlot,s.shareRefusedCold,s.shareRefusedPacked,s.shareRefusedBusy);
        }
    }
}
static const uint8_t kTerrainResizeBytes[]={0x48,0x89,0x4c,0x24,0x08,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30};
static const uint8_t kTerrainCopyBytes[]={0x48,0x89,0x4c,0x24,0x08,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30};
static const uint8_t kTerrainDestroyBytes[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x49,0x10,0x48,0x85,0xc9};
static const uint8_t kTerrainResizeCall[]={0xe8,0xa6,0x8f,0xe9,0xff};
static const uint8_t kTerrainCopyCall[]={0xe8,0x3f,0x10,0xea,0xff};
static bool InstallTerrainCompression() {
    if(!g_terrainCompress)return false;
    if(!g_terrainHotMB || g_terrainWarmMB==-1) {   // 0 / -1 mean auto; other negatives stay invalid
        uint64_t total=InstalledPhysicalBytes();int hot=0,warm=0;
        AutoTerrainBudgets(total,&hot,&warm);
        if(!g_terrainHotMB)g_terrainHotMB=hot;
        if(g_terrainWarmMB==-1)g_terrainWarmMB=warm;
        H->log("terrain compression: auto budgets for %llu MiB RAM -> hot %d MiB, warm %d MiB",
               (unsigned long long)(total>>20),g_terrainHotMB,g_terrainWarmMB);
    }
    if(g_gog || g_terrainCompress!=1 || g_terrainCacheSpacing!=1 || g_terrainHotMB<128 || g_terrainHotMB>8192 || g_terrainWarmMB<0 || g_terrainWarmMB>8192) {
        H->log("terrain compression: requires Steam, spacing=1, compress=1, hot_mb=128..8192, warm_mb=0..8192; OFF");return false;
    }
    if(!H->verifyBytes(0x1d5c50,kTerrainResizeBytes,sizeof kTerrainResizeBytes) ||
       !H->verifyBytes(0x1dedd0,kTerrainCopyBytes,sizeof kTerrainCopyBytes) ||
       !H->verifyBytes(0x33de30,kTerrainDestroyBytes,sizeof kTerrainDestroyBytes) ||
       !H->verifyBytes(0x33cca5,kTerrainResizeCall,sizeof kTerrainResizeCall) ||
       !H->verifyBytes(0x33dd8c,kTerrainCopyCall,sizeof kTerrainCopyCall)) {
        H->log("terrain compression: Steam byte mismatch; OFF");return false;
    }
    if(!TerrainPager::Init(size_t(g_terrainHotMB)*1024*1024)) {
        H->log("terrain compression: placeholder/handler initialization failed; OFF");return false;
    }
    TerrainPager::SetEvictRate(g_terrainEvictPerSec<0?0:unsigned(g_terrainEvictPerSec));
    if(g_terrainDedup && !TerrainPager::EnableDedup()){H->log("terrain compression: dedup index allocation failed; dedup OFF");g_terrainDedup=0;}
    if(g_terrainLazyZero && !TerrainPager::EnableLazyZero()){H->log("terrain compression: zero blob encode failed; lazy zero OFF");g_terrainLazyZero=0;}
    g_terrainCompressionBase=H->moduleBase();
    // Destruction first; allocation remains disabled until EVERY hook and the
    // worker succeed. Partial installation cannot create managed allocations.
    if(!H->installHook(H->moduleBase()+0x33de30,reinterpret_cast<void*>(TerrainCompressedDestroy),sizeof kTerrainDestroyBytes,reinterpret_cast<void**>(&g_originalTerrainDestroy)) ||
       !H->installHook(H->moduleBase()+0x1dedd0,reinterpret_cast<void*>(TerrainCompressedCopy),sizeof kTerrainCopyBytes,reinterpret_cast<void**>(&g_originalTerrainCopy)) ||
       !H->installHook(H->moduleBase()+0x1d5c50,reinterpret_cast<void*>(TerrainCompressedResize),sizeof kTerrainResizeBytes,reinterpret_cast<void**>(&g_originalTerrainResize))) {
        H->log("terrain compression: hook failed; allocation remains OFF");return false;
    }
    HANDLE worker=CreateThread(nullptr,0,TerrainCompressionWorker,nullptr,0,nullptr);
    if(!worker){H->log("terrain compression: worker failed; allocation remains OFF");return false;}
    SetThreadPriority(worker,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(worker);
    for(unsigned n=PagerHelperThreads();n--;)
        if(HANDLE helperThread=CreateThread(nullptr,0,TerrainEvictionHelper,nullptr,0,nullptr)){SetThreadPriority(helperThread,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(helperThread);}
    InterlockedExchange(&g_terrainCompressActive,1);
    H->log("terrain compression: lossless 1 m cache enabled, %d MiB resident target, cow_share=%d dedup=%d lazy_zero=%d dedup_probe=%d; restart to disable",g_terrainHotMB,g_terrainCowShare,g_terrainDedup,g_terrainLazyZero,g_terrainDedupProbe);
    return true;
}

extern "C" __declspec(dllexport) int BigmapTestCompressionInit(){return TerrainPager::Init(1024*1024);}
extern "C" __declspec(dllexport) void BigmapTestCompressionResize(TerrainOwnedVector* v,size_t n,int eligible,TerrainResizeFn fn){g_originalTerrainResize=fn;ResizeTerrainOwned(v,n,eligible!=0);}
extern "C" __declspec(dllexport) void* BigmapTestCompressionCopy(TerrainOwnedVector* dst,const TerrainOwnedVector* src,int eligible,TerrainCopyFn fn){g_originalTerrainCopy=fn;return CopyTerrainOwned(dst,src,eligible!=0);}
extern "C" __declspec(dllexport) void BigmapTestCompressionDestroy(void* control,TerrainDestroyFn fn){g_originalTerrainDestroy=fn;TerrainCompressedDestroy(control);}
extern "C" __declspec(dllexport) int BigmapTestCompressionEvict(void* p){return TerrainPager::Contains(p)&&TerrainPager::Evict(TerrainPager::Index(p),true);}
extern "C" __declspec(dllexport) void BigmapTestSetCowShare(int on){g_terrainCowShare=on;}
extern "C" __declspec(dllexport) void BigmapTestCompressionStats(uint64_t* out) {
    auto s=TerrainPager::Snapshot();
    out[0]=s.live;out[1]=s.resident;out[2]=s.sharedViews;out[3]=s.sharedSlots;
    out[4]=s.privatizations;out[5]=s.failures;
}
extern "C" __declspec(dllexport) int BigmapTestTerrainBudget(int hot,int warm,int busy,int bulk,uint64_t available){return TerrainBudgetMB(hot,warm,busy!=0,bulk!=0,available);}
extern "C" __declspec(dllexport) void BigmapTestAutoTerrainBudgets(uint64_t totalBytes,int* hot,int* warm){AutoTerrainBudgets(totalBytes,hot,warm);}
extern "C" __declspec(dllexport) unsigned BigmapTestEvictRateStep(unsigned* rate,unsigned* quiet,uint64_t* cost,uint64_t avgMicros,unsigned stalls,int uiSignal,unsigned ceiling) {
    EvictRateState st{*rate,*quiet,*cost};unsigned r=EvictRateStep(&st,avgMicros,stalls,uiSignal!=0,ceiling);*rate=st.rate;*quiet=st.quiet;*cost=st.cost;return r;
}
extern "C" __declspec(dllexport) unsigned BigmapTestUiStalls(const uint64_t* stamps,int n) {
    UiStallMeter m;for(int i=0;i<n;++i)m.Sample(stamps[i]);return m.Take();
}
extern "C" __declspec(dllexport) int BigmapTestTerrainBudgetSteady(int prev,int next,int hot,uint64_t decodesPerSec,uint64_t available,uint64_t physical,unsigned share){return TerrainBudgetSteady(prev,next,hot,decodesPerSec,available,physical,share);}
extern "C" __declspec(dllexport) uint64_t BigmapTestPagerHeadroom(uint64_t physical){return PagerHeadroom(physical);}
extern "C" __declspec(dllexport) uint64_t BigmapTestCommitTightBytes(uint64_t physical){return CommitTightBytes(physical);}
extern "C" __declspec(dllexport) int BigmapTestTerrainBudgetLive(int hot,int warm,int busy,int bulk,uint64_t available,uint64_t liveMB,uint64_t physical){return TerrainBudgetMB(hot,warm,busy!=0,bulk!=0,available,liveMB,physical);}
extern "C" __declspec(dllexport) int BigmapTestWarmUpdate(TerrainWarmup* state,uint64_t now,int busy,uint64_t bulk,uint64_t ui,int signal){return state->Update(now,busy!=0,bulk,ui,signal!=0);}
extern "C" __declspec(dllexport) int BigmapTestInstallCompression(const Tpf2mpHost* host,int gog,int spacing,int enabled,int hotMB) {
    H=host;g_gog=gog!=0;g_terrainCacheSpacing=spacing;g_terrainCompress=enabled;g_terrainHotMB=hotMB;
    return InstallTerrainCompression();
}
