// Steam 35924 integration for material-index cell paging. The evidence for
// every site below is in docs/material-grid-lifetime.md:
//   - the ONLY allocation of a cell payload is InternCreate (3303b0) calling the
//     vector<uint8>::resize helper 1d5830 at 3303e1 (return 3303e6) with 67,601;
//   - the ONLY free is the IBaseGrid<vector<uint8>> dtor 32a6f0, always preceded
//     by InternDestroy (330350) on every cell, which is replaced here for owned
//     cells (accounting, release, null triple) so the free sees null vectors;
//   - 32a6f0 is also hooked as a safety net, and the assign helper 1b3d90 is
//     guarded so an unexpected growth migrates to the heap instead of freeing
//     an arena pointer.
// The 1,157-byte ambient cells (same grid type) are excluded by size.
#pragma once
#include "material_pager.h"
#include "terrain_compression.h"
static int g_materialCompress=0, g_materialHotMB=256, g_materialWarmMB=1024;
static int g_materialMaxMB=0;   // material_cache_max_mb: hard cap on the resident target, 0 = auto (a quarter of the terrain cap), -1 = none
static volatile LONG g_materialCompressActive=0;
struct MaterialOwnedVector {uint8_t *first,*last,*end;};
using MaterialResizeFn=void(__fastcall*)(MaterialOwnedVector*,size_t);
using MaterialAssignFn=void(__fastcall*)(MaterialOwnedVector*,const uint8_t*,const uint8_t*);
using MaterialInternDestroyFn=void(__fastcall*)(void*,MaterialOwnedVector*);
using MaterialGridDtorFn=void*(__fastcall*)(void*,void*,void*,void*);
static MaterialResizeFn g_originalMaterialResize;
static MaterialAssignFn g_originalMaterialAssign;
static MaterialInternDestroyFn g_originalMaterialInternDestroy;
static MaterialGridDtorFn g_originalMaterialGridDtor;
static uintptr_t g_materialCompressionBase;
// Runtime checks from the lifetime report: every one of these except
// releases is expected to stay at zero.
static volatile LONG64 g_materialReleases=0, g_materialMigrations=0, g_materialLateReleases=0;

// Move an owned payload to a stock heap vector of the same size, preserving
// bytes, before a generic vector operation could free or reallocate it.
static void AutoMaterialBudgets(uint64_t totalBytes,int* hot,int* warm) {
    int h=AutoBudgetMB(totalBytes,180,96,1024);
    *hot=h;*warm=AutoBudgetMB(totalBytes,48,h,4096);
}
static void MigrateMaterialOwned(MaterialOwnedVector* v) {
    size_t size=size_t(v->last-v->first);
    MaterialOwnedVector replacement{};
    g_originalMaterialResize(&replacement,size?size:1);
    replacement.last=replacement.first+size;
    if(size)memcpy(replacement.first,v->first,size);
    MaterialPager::Release(v->first);*v=replacement;
    InterlockedIncrement64(&g_materialMigrations);
}
static void ResizeMaterialOwned(MaterialOwnedVector* v,size_t n,bool eligible) {
    using namespace MaterialPager;
    if(Contains(v->first)) {
        size_t oldSize=size_t(v->last-v->first);
        if(n<=Bytes){v->last=v->first+n; if(n>oldSize)memset(v->first+oldSize,0,n-oldSize);return;}
        // Growth past the slot: one stock allocation of the new (zero-filled)
        // size, keep the old bytes, and never let generic code free our slot.
        MaterialOwnedVector replacement{};
        g_originalMaterialResize(&replacement,n);
        memcpy(replacement.first,v->first,oldSize);
        Release(v->first);*v=replacement;
        InterlockedIncrement64(&g_materialMigrations);return;
    }
    if(eligible && !v->first && !v->last && !v->end && n==Bytes) {
        // Fresh sections are zero-filled, matching the stock grow path's memset.
        if(auto p=Allocate()){*v={p,p+n,p+n};return;}
    }
    g_originalMaterialResize(v,n);
}
static void __fastcall MaterialCompressedResize(MaterialOwnedVector* v,size_t n) {
    bool eligible=InterlockedCompareExchange(&g_materialCompressActive,0,0) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress())==g_materialCompressionBase+0x3303e6;
    ResizeMaterialOwned(v,n,eligible);
}
static void AssignMaterialOwned(MaterialOwnedVector* dst,const uint8_t* first,const uint8_t* last) {
    // The stock helper frees and reallocates when capacity is too small. An
    // owned cell's capacity is exactly Bytes; anything larger migrates first.
    if(MaterialPager::Contains(dst->first) && size_t(last-first)>size_t(dst->end-dst->first))
        MigrateMaterialOwned(dst);
    g_originalMaterialAssign(dst,first,last);
}
static void __fastcall MaterialCompressedAssign(MaterialOwnedVector* dst,const uint8_t* first,const uint8_t* last) {
    AssignMaterialOwned(dst,first,last);
}
static void InternDestroyMaterial(void* grid,MaterialOwnedVector* v) {
    if(!MaterialPager::Contains(v->first)) {g_originalMaterialInternDestroy(grid,v);return;}
    // Stock: [grid+0x60] -= [grid+0x10] * [grid+0xc] (int32), canary compare,
    // resize(0). The canary is not read here: that would decompress every cold
    // cell at teardown only to discard it (the codec's content hash already
    // guards the bytes). Releasing and nulling makes 32a6f0 skip the free.
    auto counter=reinterpret_cast<int32_t*>(static_cast<uint8_t*>(grid)+0x60);
    auto dimX=*reinterpret_cast<int32_t*>(static_cast<uint8_t*>(grid)+0x0c);
    auto dimY=*reinterpret_cast<int32_t*>(static_cast<uint8_t*>(grid)+0x10);
    *counter=int32_t(uint32_t(*counter)-uint32_t(dimX)*uint32_t(dimY));
    MaterialPager::Release(v->first);*v={};
    InterlockedIncrement64(&g_materialReleases);
}
static void __fastcall MaterialCompressedInternDestroy(void* grid,MaterialOwnedVector* v) {InternDestroyMaterial(grid,v);}
static void ReleaseMaterialGridCells(void* grid) {
    auto base=static_cast<uint8_t*>(grid);
    auto first=*reinterpret_cast<uint8_t**>(base+0x78),last=*reinterpret_cast<uint8_t**>(base+0x80);
    if(!first || last<first || (last-first)%0x48)return;
    for(auto cell=first;cell<last;cell+=0x48) {
        auto v=reinterpret_cast<MaterialOwnedVector*>(cell);
        if(MaterialPager::Contains(v->first)) {
            MaterialPager::Release(v->first);*v={};
            InterlockedIncrement64(&g_materialLateReleases);
        }
    }
}
static void* __fastcall MaterialCompressedGridDtor(void* grid,void* a2,void* a3,void* a4) {
    ReleaseMaterialGridCells(grid);
    return g_originalMaterialGridDtor(grid,a2,a3,a4);
}
static DWORD WINAPI MaterialEvictionHelper(void*) {
    for(;;){Sleep(25);if(InterlockedCompareExchange(&g_materialCompressActive,0,0))MaterialPager::Tick();}
}
static DWORD WINAPI MaterialCompressionWorker(void*) {
    uint64_t lastLog=GetTickCount64(),lastPolicy=0;int effectiveMB=g_materialHotMB;
    uint64_t lastDecodes=0,lastEvictMicros=0,lastEvictOps=0,lastRateLog=0;EvictRateState rate{};
    for(;;) {
        Sleep(25);
        if(!InterlockedCompareExchange(&g_materialCompressActive,0,0))continue;
        auto now=GetTickCount64();
        if(now-lastPolicy>=1000) {
            lastPolicy=now;auto s=MaterialPager::Snapshot();MEMORYSTATUSEX m{};m.dwLength=sizeof m;
            // Pager sections are page-file-backed: they count against the system
            // commit limit (RAM + page file) exactly like private memory. Use the
            // smaller of free RAM and free commit, and back off hard when commit is
            // nearly exhausted (a 512x512 desert preview hit std::bad_alloc at the
            // 114.6 GB commit limit while tiles were held uncompressed).
            bool haveStatus=PagerMemoryStatus(&m)!=0;
            uint64_t available=haveStatus?(m.ullAvailPhys<m.ullAvailPageFile?m.ullAvailPhys:m.ullAvailPageFile):0;
            static const uint64_t physical=InstalledPhysicalBytes();
            static CommitTightState tightState;    // sticky: see CommitTightSticky
            bool commitTight=haveStatus && CommitTightSticky(tightState,m.ullAvailPageFile<CommitTightBytes(physical),m.ullAvailPageFile,
                                                            CommitTightBytes(physical),(uint64_t(g_materialHotMB>256?g_materialHotMB-256:0)<<20)+(1ull<<30),now);
            bool pressure=haveStatus && m.ullAvailPhys<PagerHeadroom(physical);
            bool busy=InterlockedCompareExchange(&g_worldEntryActive,0,0)!=0;
            // Initial generation, edit boxes and a full repaint allocate or
            // restore cells in bursts; keep the warm allowance for 15 s after.
            bool bulk=s.lastBulkAllocation && now-s.lastBulkAllocation<15000;
            // Growth with the live size while loading, capped by the smaller of free
            // RAM and free commit (above). Measured on the same 256x256 save: with a
            // fixed 4 GiB allowance the loading thread spent 69-71% of two profile
            // windows restoring tiles (85 s); with growth 41-53% (73 s). The earlier
            // RAM-only cap hit std::bad_alloc at the commit limit on a 512x512 preview.
            int next=TerrainBudgetMB(g_materialHotMB,g_materialWarmMB,busy,bulk,available,(s.live*MaterialPager::SlotBytes)>>20,physical);
            // Same steady-state rules as the terrain pager: ramp down instead of
            // snapping (MEASURED 2026-09-17: the snap evicted 93,000 cells in 30 s
            // and the game stuttered), hold or grow while cells fault back in.
            uint64_t decodes=s.faults-s.softRescues,decodesPerSec=decodes-lastDecodes;lastDecodes=decodes;
            MaterialPager::SetUrgent(commitTight||pressure);
            // the throttle is for load bursts only (terrain_compression.h, same date)
            MaterialPager::SetThrottle(commitTight && (busy||bulk));
            if(commitTight && next>256)next=256;
            else if(!(busy||bulk)){ static int wsFloor=0; next=TerrainBudgetSteady(effectiveMB,next,g_materialHotMB,decodesPerSec,available,physical,2,&wsFloor); }
            { int cap=PagerCapMB(g_materialMaxMB,g_materialHotMB,physical,1); if(cap && next>cap)next=cap; }
            MaterialPager::SetBudget(size_t(next)*1024*1024);
            if(next!=effectiveMB && (next==g_materialHotMB||effectiveMB==g_materialHotMB||next-effectiveMB>=1024||effectiveMB-next>=1024))H->log("material compression: resident target %d -> %d MiB (generation=%d bulk_allocation=%d commit_tight=%d pressure=%d free=%llu MiB)",effectiveMB,next,int(busy),int(bulk),int(commitTight),int(pressure),(unsigned long long)(m.ullAvailPhys>>20));
            effectiveMB=next;
            // Adaptive eviction rate: this pager's own cost, the terrain worker's
            // frame-stall count (one meter for the process).
            uint64_t ops=s.evictOps-lastEvictOps,micros=s.evictMicros-lastEvictMicros;lastEvictOps=s.evictOps;lastEvictMicros=s.evictMicros;
            unsigned stalls=unsigned(InterlockedCompareExchange(&g_uiStallsLastSec,0,0));
            bool uiSignal=GetModuleHandleW(L"tpf2_menu.dll")!=nullptr;
            unsigned before=rate.rate;
            unsigned perSec=EvictRateStep(&rate,ops?micros/ops:0,stalls,uiSignal,g_materialEvictPerSec<0?0:unsigned(g_materialEvictPerSec));
            MaterialPager::SetEvictRate(perSec);
            if(perSec && perSec<before && stalls && now-lastRateLog>=10000) {
                lastRateLog=now;
                H->log("material compression: %u frame stall(s) >= 60 ms, eviction rate %u -> %u/s (last second: %llu evictions, %llu us each)",stalls,before,perSec,ops,ops?micros/ops:0ull);
            }
        }
        MaterialPager::Tick();
        if(now-lastLog>=30000) {
            lastLog=now;auto s=MaterialPager::Snapshot();
            if(s.live||g_materialReleases)H->log("material compression: live=%llu resident=%llu backing=%.1f MiB compressed=%.1f MiB encoded_commit=%.1f MiB faults=%llu evictions=%llu failures=%llu encodes=%llu reused=%llu writes=%llu slot_overflows=%llu soft_blocked=%llu soft_rescues=%llu cancelled=%llu releases=%lld migrations=%lld late_releases=%lld",
                s.live,s.resident,double(s.resident*MaterialPager::SlotBytes)/(1024*1024),
                double(s.compressedBytes)/(1024*1024),double(s.compressedCommit)/(1024*1024),s.faults,s.evictions,s.failures,
                s.encodes,s.reusedEvictions,s.writeFaults,s.overflows,s.softBlocked,s.softRescues,s.cancelledEvictions,
                (long long)g_materialReleases,(long long)g_materialMigrations,(long long)g_materialLateReleases);
        }
    }
}
static const uint8_t kMaterialResizeBytes[]={0x48,0x89,0x4c,0x24,0x08,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30};
static const uint8_t kMaterialResizeCall[]={0x48,0x63,0xd0,0xe8,0x4a,0x54,0xea,0xff};
static const uint8_t kMaterialInternDestroyBytes[]={0x48,0x83,0xec,0x28,0x4c,0x8b,0xca,0x8b,0x51,0x10,0x0f,0xaf,0x51,0x0c};
static const uint8_t kMaterialGridDtorBytes[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
static const uint8_t kMaterialAssignBytes[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20};
static bool InstallMaterialCompression() {
    if(!g_materialCompress)return false;
    MaterialPager::SetEvictRate(g_materialEvictPerSec<0?0:unsigned(g_materialEvictPerSec));
    if(!g_materialHotMB || g_materialWarmMB==-1) {   // 0 / -1 mean auto; other negatives stay invalid
        uint64_t total=InstalledPhysicalBytes();int hot=0,warm=0;
        AutoMaterialBudgets(total,&hot,&warm);
        if(!g_materialHotMB)g_materialHotMB=hot;
        if(g_materialWarmMB==-1)g_materialWarmMB=warm;
        H->log("material compression: auto budgets for %llu MiB RAM -> hot %d MiB, warm %d MiB",
               (unsigned long long)(total>>20),g_materialHotMB,g_materialWarmMB);
    }
    if(g_gog || g_materialCompress!=1 || g_materialHotMB<64 || g_materialHotMB>8192 || g_materialWarmMB<0 || g_materialWarmMB>8192) {
        H->log("material compression: requires Steam, compress=1, hot_mb=64..8192, warm_mb=0..8192; OFF");return false;
    }
    if(!H->verifyBytes(0x1d5830,kMaterialResizeBytes,sizeof kMaterialResizeBytes) ||
       !H->verifyBytes(0x3303de,kMaterialResizeCall,sizeof kMaterialResizeCall) ||
       !H->verifyBytes(0x330350,kMaterialInternDestroyBytes,sizeof kMaterialInternDestroyBytes) ||
       !H->verifyBytes(0x32a6f0,kMaterialGridDtorBytes,sizeof kMaterialGridDtorBytes) ||
       !H->verifyBytes(0x1b3d90,kMaterialAssignBytes,sizeof kMaterialAssignBytes)) {
        H->log("material compression: Steam byte mismatch; OFF");return false;
    }
    if(!MaterialPager::Init(size_t(g_materialHotMB)*1024*1024)) {
        H->log("material compression: placeholder/handler initialization failed; OFF");return false;
    }
    g_materialCompressionBase=H->moduleBase();
    // Every release/migration path first; allocation stays disabled until
    // EVERY hook and the worker succeed, so a partial install owns nothing.
    if(!H->installHook(H->moduleBase()+0x330350,reinterpret_cast<void*>(MaterialCompressedInternDestroy),sizeof kMaterialInternDestroyBytes,reinterpret_cast<void**>(&g_originalMaterialInternDestroy)) ||
       !H->installHook(H->moduleBase()+0x32a6f0,reinterpret_cast<void*>(MaterialCompressedGridDtor),sizeof kMaterialGridDtorBytes,reinterpret_cast<void**>(&g_originalMaterialGridDtor)) ||
       !H->installHook(H->moduleBase()+0x1b3d90,reinterpret_cast<void*>(MaterialCompressedAssign),sizeof kMaterialAssignBytes,reinterpret_cast<void**>(&g_originalMaterialAssign)) ||
       !H->installHook(H->moduleBase()+0x1d5830,reinterpret_cast<void*>(MaterialCompressedResize),sizeof kMaterialResizeBytes,reinterpret_cast<void**>(&g_originalMaterialResize))) {
        H->log("material compression: hook failed; allocation remains OFF");return false;
    }
    HANDLE worker=CreateThread(nullptr,0,MaterialCompressionWorker,nullptr,0,nullptr);
    if(!worker){H->log("material compression: worker failed; allocation remains OFF");return false;}
    SetThreadPriority(worker,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(worker);
    for(unsigned n=PagerHelperThreads();n--;)
        if(HANDLE helperThread=CreateThread(nullptr,0,MaterialEvictionHelper,nullptr,0,nullptr)){SetThreadPriority(helperThread,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(helperThread);}
    InterlockedExchange(&g_materialCompressActive,1);
    H->log("material compression: lossless material-index cells enabled, %d MiB resident target (%d MiB warm); restart to disable",g_materialHotMB,g_materialWarmMB);
    return true;
}

extern "C" __declspec(dllexport) void BigmapTestAutoMaterialBudgets(uint64_t totalBytes,int* hot,int* warm){AutoMaterialBudgets(totalBytes,hot,warm);}
extern "C" __declspec(dllexport) int BigmapTestMaterialInit(){return MaterialPager::Init(1024*1024);}
extern "C" __declspec(dllexport) void BigmapTestMaterialResize(MaterialOwnedVector* v,size_t n,int eligible,MaterialResizeFn fn){g_originalMaterialResize=fn;ResizeMaterialOwned(v,n,eligible!=0);}
extern "C" __declspec(dllexport) void BigmapTestMaterialAssign(MaterialOwnedVector* dst,const uint8_t* first,const uint8_t* last,MaterialAssignFn assign,MaterialResizeFn resize){g_originalMaterialAssign=assign;g_originalMaterialResize=resize;AssignMaterialOwned(dst,first,last);}
extern "C" __declspec(dllexport) void BigmapTestMaterialInternDestroy(void* grid,MaterialOwnedVector* v,MaterialInternDestroyFn fn){g_originalMaterialInternDestroy=fn;InternDestroyMaterial(grid,v);}
extern "C" __declspec(dllexport) void BigmapTestMaterialGridCells(void* grid){ReleaseMaterialGridCells(grid);}
extern "C" __declspec(dllexport) int BigmapTestMaterialEvict(void* p){return MaterialPager::Contains(p)&&MaterialPager::Evict(MaterialPager::Index(p),true);}
extern "C" __declspec(dllexport) int64_t BigmapTestMaterialCounter(int which){return which==0?g_materialReleases:which==1?g_materialMigrations:g_materialLateReleases;}
extern "C" __declspec(dllexport) int BigmapTestInstallMaterial(const Tpf2mpHost* host,int gog,int enabled,int hotMB,int warmMB) {
    H=host;g_gog=gog!=0;g_materialCompress=enabled;g_materialHotMB=hotMB;g_materialWarmMB=warmMB;
    return InstallMaterialCompression();
}
