// Steam 35924 world-entry phase instrumentation. All game arguments and
// ownership are preserved; hooks below do not skip or parallelize ECS writes.
#pragma once
#include <psapi.h>
static bool g_worldEntryTimings = false;
static bool g_worldEntryTrackBusy = false;
static volatile LONG g_worldEntryActive=0;
static thread_local unsigned g_worldEntryNesting = 0;

struct WorldEntryTimer {
    const char* phase;
    LARGE_INTEGER start{}, frequency{};
    bool active;
    explicit WorldEntryTimer(const char* name) : phase(name), active(g_worldEntryTimings && g_worldEntryNesting != 0) {
        if (!active) return;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        memory("begin", 0.0);
    }
    void memory(const char* event, double seconds) {
        PROCESS_MEMORY_COUNTERS_EX m{};
        m.cb = sizeof m;
        MEMORYSTATUSEX system{};
        system.dwLength = sizeof system;
        const bool processOk = K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof m) != FALSE;
        const bool systemOk = GlobalMemoryStatusEx(&system) != FALSE;
        H->log("world entry: %s %s %.3f s; private %.3f GiB, resident %.3f GiB, "
               "available %.3f GiB; memory_valid=%d/%d", event, phase, seconds,
               m.PrivateUsage / 1073741824.0, m.WorkingSetSize / 1073741824.0,
               system.ullAvailPhys / 1073741824.0, processOk, systemOk);
    }
    ~WorldEntryTimer() {
        if (!active) return;
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        memory("end", frequency.QuadPart ? double(end.QuadPart-start.QuadPart)/frequency.QuadPart : 0);
    }
};

using EntryFn = void (__fastcall*)(void*,void*,void*,uint32_t,uint32_t,void*,void*,void*);
using AllocateWorldFn = void (__fastcall*)(void*,void*);
using PopulateFn = void (__fastcall*)(void*,void*,void*,void*,uint32_t,void*);
using TerrainBuildFn = void (__fastcall*)(void*,void*,void*,void*,void*,void*,void*,void*,void*,void*);
using TerrainPublishFn = void (__fastcall*)(void*,void*,void*);
using RoadsFn = void (__fastcall*)(void*,void*,void*,uint32_t,uint32_t,void*,void*,void*,void*,uint8_t);
static RoadsFn g_originalRoads=nullptr;
static EntryFn g_originalEntry = nullptr;
static AllocateWorldFn g_originalAllocate = nullptr;
static PopulateFn g_originalTrees = nullptr, g_originalAssets = nullptr;
static TerrainBuildFn g_originalTerrainBuild = nullptr;
static TerrainPublishFn g_originalTerrainPublish = nullptr;

static void __fastcall EntryDetour(void* a,void* b,void* c,uint32_t seed,
                                   uint32_t date,void* map,void* name,void* progress) {
    struct Scope {
        Scope(){++g_worldEntryNesting;InterlockedIncrement(&g_worldEntryActive);}
        ~Scope(){--g_worldEntryNesting;InterlockedDecrement(&g_worldEntryActive);}
    } scope;
    WorldEntryTimer timer("InitNewGame total");
    g_originalEntry(a,b,c,seed,date,map,name,progress);
}
static void __fastcall AllocateWorldDetour(void* a,void* b) {
    WorldEntryTimer timer("world allocation"); g_originalAllocate(a,b);
}
static void __fastcall TreesDetour(void* a,void* b,void* c,void* d,uint32_t seed,void* f) {
    WorldEntryTimer timer("trees"); g_originalTrees(a,b,c,d,seed,f);
}
static void __fastcall AssetsDetour(void* a,void* b,void* c,void* d,uint32_t seed,void* f) {
    WorldEntryTimer timer("scenery assets"); g_originalAssets(a,b,c,d,seed,f);
}
static void __fastcall TerrainBuildDetour(void* a,void* b,void* c,void* d,void* e,
                                         void* f,void* g,void* h,void* i,void* j) {
    WorldEntryTimer timer("terrain preparation"); g_originalTerrainBuild(a,b,c,d,e,f,g,h,i,j);
}
static void __fastcall TerrainPublishDetour(void* a,void* b,void* c) {
    WorldEntryTimer timer("terrain publication"); g_originalTerrainPublish(a,b,c);
}
static void __fastcall RoadsDetour(void* a,void* b,void* c,uint32_t d,uint32_t e,
                                  void* f,void* g,void* h,void* i,uint8_t j) {
    WorldEntryTimer timer("town/industry road connections");
    g_originalRoads(a,b,c,d,e,f,g,h,i,j);
}

struct WorldEntrySite {
    uintptr_t rva;
    const char* name;
    uint8_t bytes[24];
    int size;
    void* detour;
    void** original;
};
static WorldEntrySite g_entrySites[] = {
    {0x230440,"world allocation", {0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x70,0x20,0x55,0x57,0x41,0x56},15,
        (void*)&AllocateWorldDetour,(void**)&g_originalAllocate},
    {0x3bcb90,"terrain preparation", {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xf8},18,
        (void*)&TerrainBuildDetour,(void**)&g_originalTerrainBuild},
    {0x3bc9e0,"terrain publication", {0x48,0x8b,0xc4,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x50,0x48,0xc7,0x40,0xc8,0xfe,0xff,0xff,0xff},20,
        (void*)&TerrainPublishDetour,(void**)&g_originalTerrainPublish},
    {0x3b8bd0,"trees", {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x18,0xec,0xff,0xff},21,
        (void*)&TreesDetour,(void**)&g_originalTrees},
    {0x3b8660,"scenery assets", {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x18,0xec,0xff,0xff},21,
        (void*)&AssetsDetour,(void**)&g_originalAssets},
    {0x157390,"InitNewGame total", {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xe8,0xdd,0xff,0xff},21,
        (void*)&EntryDetour,(void**)&g_originalEntry},
    {0x937590,"town/industry road connections", {0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x88,0xfd,0xff,0xff},22,
        (void*)&RoadsDetour,(void**)&g_originalRoads}
};
static bool InstallWorldEntryTimings() {
    if ((!g_worldEntryTimings && !g_worldEntryTrackBusy) || g_gog) return false;
    if(!g_worldEntryTimings) {
        const auto& s=g_entrySites[5];
        if(!H->verifyBytes(s.rva,s.bytes,s.size) ||
           !H->installHook(H->moduleBase()+s.rva,s.detour,s.size,s.original))return false;
        H->log("world entry: generation activity tracking enabled for cache warmup");return true;
    }
    for (const auto& s : g_entrySites) {
        if (!H->verifyBytes(s.rva,s.bytes,s.size)) {
            H->log("world entry timings: %s byte mismatch; OFF",s.name); return false;
        }
    }
    // Activate the outer timer last. Partial installation leaves earlier
    // wrappers as pass-throughs outside the instrumented entry scope.
    for (unsigned index : {0u,1u,2u,3u,4u,6u,5u}) {
        const auto& s=g_entrySites[index];
        if (!H->installHook(H->moduleBase()+s.rva,s.detour,s.size,s.original)) {
            H->log("world entry timings: %s hook failed; OFF",s.name); return false;
        }
    }
    H->log("world entry timings: enabled (total, allocation, terrain, trees, scenery, road connections)");
    return true;
}
extern "C" __declspec(dllexport)
int BigmapTestWorldBusy(){return int(InterlockedCompareExchange(&g_worldEntryActive,0,0));}
extern "C" __declspec(dllexport)
int BigmapTestInstallWorldActivity(const Tpf2mpHost* host,int gog) {
    const auto oldHost=H;bool oldGog=g_gog,oldEnabled=g_worldEntryTimings,oldTrack=g_worldEntryTrackBusy;
    H=host;g_gog=gog!=0;g_worldEntryTimings=false;g_worldEntryTrackBusy=true;
    bool ok=InstallWorldEntryTimings();H=oldHost;g_gog=oldGog;g_worldEntryTimings=oldEnabled;g_worldEntryTrackBusy=oldTrack;return ok;
}
extern "C" __declspec(dllexport)
int BigmapTestInstallWorldEntry(const Tpf2mpHost* host,int gog,int enabled) {
    const auto oldHost=H; const bool oldGog=g_gog, oldEnabled=g_worldEntryTimings;
    H=host; g_gog=gog!=0; g_worldEntryTimings=enabled!=0;
    const bool result=InstallWorldEntryTimings();
    H=oldHost; g_gog=oldGog; g_worldEntryTimings=oldEnabled;
    return result;
}
extern "C" __declspec(dllexport)
void BigmapTestInvokeWorldEntry(const Tpf2mpHost* host,int index,uintptr_t* a) {
    const auto oldHost=H; const auto oldEnabled=g_worldEntryTimings;H=host;g_worldEntryTimings=true;
    switch(index) {
    case 0: AllocateWorldDetour((void*)a[0],(void*)a[1]); break;
    case 1: TerrainBuildDetour((void*)a[0],(void*)a[1],(void*)a[2],(void*)a[3],
        (void*)a[4],(void*)a[5],(void*)a[6],(void*)a[7],(void*)a[8],(void*)a[9]); break;
    case 2: TerrainPublishDetour((void*)a[0],(void*)a[1],(void*)a[2]); break;
    case 3: TreesDetour((void*)a[0],(void*)a[1],(void*)a[2],(void*)a[3],uint32_t(a[4]),(void*)a[5]); break;
    case 4: AssetsDetour((void*)a[0],(void*)a[1],(void*)a[2],(void*)a[3],uint32_t(a[4]),(void*)a[5]); break;
    case 5: EntryDetour((void*)a[0],(void*)a[1],(void*)a[2],uint32_t(a[3]),
        uint32_t(a[4]),(void*)a[5],(void*)a[6],(void*)a[7]); break;
    case 6: RoadsDetour((void*)a[0],(void*)a[1],(void*)a[2],uint32_t(a[3]),
        uint32_t(a[4]),(void*)a[5],(void*)a[6],(void*)a[7],(void*)a[8],uint8_t(a[9])); break;
    }
    H=oldHost;g_worldEntryTimings=oldEnabled;
}
