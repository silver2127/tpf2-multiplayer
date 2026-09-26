// Steam 35924: trim push_back slack from the 64 m tree/scenery cell lists
// before AddComponent<ModelInstanceList> moves them into the ECS for the game.
//
// publish_model_instances (0x3bc760) groups every instance into one hash-list
// node per 64 m cell (0x3bdad0, one push_back per instance, 1.5x growth) and
// passes node+0x18 = {thin vector<0x18>, fat vector<0xc0>, bool dynamic=0}
// to AddComponent (0x1691c0) at 0x3bc8d1 with r9b=0 and a zero stack byte.
// That path ends in the move-assign 0x1eef60 (chunked push 0x2344d0 or a
// reused slot), which steals the three pointers, so the growth slack stays for
// the whole game. The replica-log copy (0x2612b0 -> 0x1e07d0 / 0x1dfdc0)
// allocates exactly size() and is unaffected.
//
// Only the engine's own helpers touch game memory: new buffers come from
// 0x15f4c0 (thin) / 0x15f5c0 (fat), the allocators its vector growth and copy
// constructors call; the old buffer is released by running the engine vector
// destructor 0xabf20 / 0x25ad60 on a copy with size zero, which frees by
// capacity through the [-8] large-block header and operator delete 0x2bf3abc
// (-> free) and destroys no record. Fat records are relocated with memcpy only
// when both embedded vectors (+0x90, +0xa8) are null, as the asset lambda
// 0x1535f0 builds them; any other cell keeps its fat vector untouched.
#pragma once
#include <cstddef>

static bool g_instanceShrink = false;

struct ShrinkVector { uint8_t* first; uint8_t* last; uint8_t* end; };
struct ShrinkList { ShrinkVector thin, fat; uint8_t dynamic; };
static_assert(sizeof(ShrinkVector) == 24 && offsetof(ShrinkList, fat) == 0x18 &&
              offsetof(ShrinkList, dynamic) == 0x30, "game ModelInstanceList ABI");

using ShrinkAllocateFn = uint8_t* (__fastcall*)(ShrinkVector* vector, uint64_t count);
using ShrinkReleaseFn = void (__fastcall*)(ShrinkVector* vector);
// AddComponent<ModelInstanceList>(engine, entity, list&&, bool, bool): the two
// flags are forwarded as full 64-bit register/stack slots, bit for bit.
using ShrinkAddFn = void (__fastcall*)(void*, void*, ShrinkList*, uint64_t, uint64_t);
using ShrinkPublishFn = void (__fastcall*)(void*, void*, void*, float);

struct ShrinkEngine {
    ShrinkAllocateFn allocateThin, allocateFat;
    ShrinkReleaseFn releaseThin, releaseFat;
    ShrinkAddFn addComponent;
};
struct ShrinkTotals { uint64_t bytes, cells, lists, keptFat; };

static ShrinkEngine g_shrinkEngine{};
static ShrinkPublishFn g_shrinkPublish = nullptr;
static uintptr_t g_shrinkBase = 0;
static bool g_shrinkLive = false;
// Written only on the world-entry thread (the matched call site).
static ShrinkTotals g_shrinkTotals{};

static uint64_t ShrinkVectorToSize(ShrinkVector* v, size_t record,
                                   ShrinkAllocateFn allocate, ShrinkReleaseFn release)
{
    if (!v->first || v->last < v->first || v->end < v->last) return 0;
    const size_t used = size_t(v->last - v->first);
    const size_t capacity = size_t(v->end - v->first);
    if (used == capacity || used % record || capacity % record) return 0;
    ShrinkVector old = *v;
    old.last = old.first;   // capacity kept, no records: free the block only
    if (used) {
        const auto fresh = allocate(v, used / record);
        if (!fresh) return 0;
        memcpy(fresh, v->first, used);
        v->first = fresh; v->last = fresh + used; v->end = fresh + used;
    } else {
        v->first = v->last = v->end = nullptr;
    }
    release(&old);
    return capacity - used;
}

static bool ShrinkFatRelocatable(const ShrinkVector& v)
{
    for (auto p = v.first; size_t(v.last - p) >= 0xc0; p += 0xc0) {
        uint64_t embedded[6];
        memcpy(embedded, p + 0x90, sizeof embedded);
        for (auto q : embedded) if (q) return false;
    }
    return true;
}

static void ShrinkListToSize(ShrinkList* list, const ShrinkEngine& engine)
{
    uint64_t trimmed = ShrinkVectorToSize(&list->thin, 0x18,
        engine.allocateThin, engine.releaseThin);
    if (list->fat.end != list->fat.last) {
        if (ShrinkFatRelocatable(list->fat))
            trimmed += ShrinkVectorToSize(&list->fat, 0xc0,
                engine.allocateFat, engine.releaseFat);
        else
            ++g_shrinkTotals.keptFat;
    }
    ++g_shrinkTotals.lists;
    if (trimmed) { ++g_shrinkTotals.cells; g_shrinkTotals.bytes += trimmed; }
}

static void ShrinkAddForward(bool fromPublish, void* engine, void* entity,
                             ShrinkList* list, uint64_t flag, uint64_t dense)
{
    if (fromPublish && list) ShrinkListToSize(list, g_shrinkEngine);
    g_shrinkEngine.addComponent(engine, entity, list, flag, dense);
}

static void __fastcall ShrinkAddDetour(void* engine, void* entity, ShrinkList* list,
                                       uint64_t flag, uint64_t dense)
{
    ShrinkAddForward(uintptr_t(_ReturnAddress()) == g_shrinkBase + 0x3bc8d6,
                     engine, entity, list, flag, dense);
}

static void __fastcall ShrinkPublishDetour(void* self, void* engine, void* instances, float cell)
{
    const uintptr_t from = uintptr_t(_ReturnAddress());
    const ShrinkTotals before = g_shrinkTotals;
    g_shrinkPublish(self, engine, instances, cell);
    if (!g_shrinkLive) return;
    const ShrinkTotals& t = g_shrinkTotals;
    H->log("instance shrink: %s trimmed %.1f MiB in %llu of %llu cells "
           "(%llu fat lists kept); running total %.1f MiB",
           from == g_shrinkBase + 0x3b8f56 ? "trees" :
           from == g_shrinkBase + 0x3b89e6 ? "scenery assets" : "model instances",
           (t.bytes - before.bytes) / 1048576.0,
           (unsigned long long)(t.cells - before.cells),
           (unsigned long long)(t.lists - before.lists),
           (unsigned long long)(t.keptFat - before.keptFat), t.bytes / 1048576.0);
}

struct ShrinkSite { uintptr_t rva; const uint8_t* bytes; uint32_t size; };
static const uint8_t kShrinkCallSite[] = {   // 3bc8bb: flags 0/0, r8=&cell->list, call 1691c0
    0xc6,0x44,0x24,0x20,0x00,0x45,0x33,0xc9,0x4c,0x8d,0x43,0x18,0x48,0x8d,
    0x54,0x24,0x38,0x48,0x8b,0x4c,0x24,0x30,0xe8,0xea,0xc8,0xda,0xff};
static const uint8_t kShrinkAddPrologue[] = {
    0x44,0x88,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x53,0x55,0x56,0x57};
static const uint8_t kShrinkPublishPrologue[] = {
    0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
static const uint8_t kShrinkTreesCall[] = {0xe8,0x0a,0x38,0x00,0x00};
static const uint8_t kShrinkAssetsCall[] = {0xe8,0x7a,0x3d,0x00,0x00};
static const uint8_t kShrinkAllocThin[] = {
    0x48,0x83,0xec,0x28,0x48,0x8d,0x04,0x52,0x48,0xb9,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,
    0xaa,0x0a,0x48,0xc1,0xe0,0x03,0x49,0xc7,0xc0,0xff,0xff,0xff,0xff,0x48,0x3b,0xd1,
    0x76,0x05,0x49,0x8b,0xc0,0xeb,0x08,0x48,0x3d,0x00,0x10,0x00,0x00,0x72,0x30,0x48,
    0x8d,0x48,0x27,0x48,0x3b,0xc8,0x49,0x0f,0x46,0xc8,0xe8,0x81,0x45,0xa9,0x02,0x48,
    0x8b,0xc8,0x48,0x85,0xc0,0x74,0x11,0x48,0x83,0xc0,0x27,0x48,0x83,0xe0,0xe0,0x48,
    0x89,0x48,0xf8,0x48,0x83,0xc4,0x28,0xc3,0xff,0x15,0xb2,0xc2,0xda,0x02};
static const uint8_t kShrinkAllocFat[] = {
    0x48,0x83,0xec,0x28,0x48,0x8d,0x04,0x52,0x48,0xb9,0x55,0x55,0x55,0x55,0x55,0x55,
    0x55,0x01,0x48,0xc1,0xe0,0x06,0x49,0xc7,0xc0,0xff,0xff,0xff,0xff,0x48,0x3b,0xd1,
    0x76,0x05,0x49,0x8b,0xc0,0xeb,0x08,0x48,0x3d,0x00,0x10,0x00,0x00,0x72,0x30,0x48,
    0x8d,0x48,0x27,0x48,0x3b,0xc8,0x49,0x0f,0x46,0xc8,0xe8,0x81,0x44,0xa9,0x02,0x48,
    0x8b,0xc8,0x48,0x85,0xc0,0x74,0x11,0x48,0x83,0xc0,0x27,0x48,0x83,0xe0,0xe0,0x48,
    0x89,0x48,0xf8,0x48,0x83,0xc4,0x28,0xc3,0xff,0x15,0xb2,0xc1,0xda,0x02};
static const uint8_t kShrinkFreeThin[] = {
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x09,0x48,0x85,0xc9,0x74,
    0x5d,0x48,0x8b,0x53,0x10,0x48,0xb8,0xab,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0x2a,0x48,
    0x2b,0xd1,0x48,0xf7,0xea,0x48,0xc1,0xfa,0x02,0x48,0x8b,0xc2,0x48,0xc1,0xe8,0x3f,
    0x48,0x03,0xd0,0x48,0x8d,0x14,0x52,0x48,0xc1,0xe2,0x03,0x48,0x81,0xfa,0x00,0x10,
    0x00,0x00,0x72,0x18,0x4c,0x8b,0x41,0xf8,0x48,0x83,0xc2,0x27,0x49,0x2b,0xc8,0x48,
    0x8d,0x41,0xf8,0x48,0x83,0xf8,0x1f,0x77,0x1b,0x49,0x8b,0xc8,0xe8,0x3b,0x7b,0xb4,
    0x02,0x33,0xc0,0x48,0x89,0x03,0x48,0x89,0x43,0x08,0x48,0x89,0x43,0x10,0x48,0x83,
    0xc4,0x20,0x5b,0xc3,0xff,0x15,0x36,0xf8,0xe5,0x02};
static const uint8_t kShrinkFreeFat[] = {
    0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x19,0x48,0x8b,0xf9,
    0x48,0x85,0xdb,0x0f,0x84,0x8a,0x00,0x00,0x00,0x48,0x89,0x74,0x24,0x30,0x48,0x8b,
    0x71,0x08,0x48,0x3b,0xde,0x74,0x17,0x48,0x8b,0xcb,0xe8,0xe1,0x6a,0xef,0xff,0x48,
    0x81,0xc3,0xc0,0x00,0x00,0x00,0x48,0x3b,0xde,0x75,0xec,0x48,0x8b,0x1f,0x48,0x8b,
    0x4f,0x10,0x48,0xb8,0xab,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0x2a,0x48,0x8b,0x74,0x24,
    0x30,0x48,0x2b,0xcb,0x48,0xf7,0xe9,0x48,0xc1,0xfa,0x05,0x48,0x8b,0xc2,0x48,0xc1,
    0xe8,0x3f,0x48,0x03,0xd0,0x48,0x8d,0x14,0x52,0x48,0xc1,0xe2,0x06,0x48,0x81,0xfa,
    0x00,0x10,0x00,0x00,0x72,0x18,0x48,0x8b,0x43,0xf8,0x48,0x83,0xc2,0x27,0x48,0x2b,
    0xd8,0x48,0x83,0xc3,0xf8,0x48,0x83,0xfb,0x1f,0x77,0x23,0x48,0x8b,0xd8,0x48,0x8b,
    0xcb,0xe8,0xc6,0x8c,0x99,0x02,0x33,0xc0,0x48,0x89,0x07,0x48,0x89,0x47,0x08,0x48,
    0x89,0x47,0x10,0x48,0x8b,0x5c,0x24,0x38,0x48,0x83,0xc4,0x20,0x5f,0xc3,0xff,0x15,
    0xbc,0x09,0xcb,0x02};
static const uint8_t kShrinkDelete[] = {0xe9,0xff,0x06,0x00,0x00};     // 2bf3abc -> 2bf41c0
static const uint8_t kShrinkDeleteJump[] = {0xe9,0xf2,0x25,0x00,0x00}; // 2bf41c0 -> 2bf67b7
static const uint8_t kShrinkFreeThunk[] = {0xff,0x25,0xfb,0x4d,0x31,0x00}; // jmp [free IAT 2f0b5b8]
static const ShrinkSite kShrinkSites[] = {
    {0x3bc8bb, kShrinkCallSite, sizeof kShrinkCallSite},
    {0x1691c0, kShrinkAddPrologue, sizeof kShrinkAddPrologue},
    {0x3bc760, kShrinkPublishPrologue, sizeof kShrinkPublishPrologue},
    {0x3b8f51, kShrinkTreesCall, sizeof kShrinkTreesCall},
    {0x3b89e1, kShrinkAssetsCall, sizeof kShrinkAssetsCall},
    {0x15f4c0, kShrinkAllocThin, sizeof kShrinkAllocThin},
    {0x15f5c0, kShrinkAllocFat, sizeof kShrinkAllocFat},
    {0x0abf20, kShrinkFreeThin, sizeof kShrinkFreeThin},
    {0x25ad60, kShrinkFreeFat, sizeof kShrinkFreeFat},
    {0x2bf3abc, kShrinkDelete, sizeof kShrinkDelete},
    {0x2bf41c0, kShrinkDeleteJump, sizeof kShrinkDeleteJump},
    {0x2bf67b7, kShrinkFreeThunk, sizeof kShrinkFreeThunk},
};

static bool InstallInstanceShrink()
{
    if (!g_instanceShrink) return false;
    if (g_gog) {
        H->log("instance shrink: Steam 35924 only; OFF on the GOG build");
        return false;
    }
    for (const auto& s : kShrinkSites) {
        if (!H->verifyBytes(s.rva, s.bytes, s.size)) {
            H->log("instance shrink: byte mismatch at RVA 0x%llx; OFF", (unsigned long long)s.rva);
            return false;
        }
    }
    const uintptr_t base = H->moduleBase();
    g_shrinkBase = base;
    g_shrinkEngine.allocateThin = reinterpret_cast<ShrinkAllocateFn>(base + 0x15f4c0);
    g_shrinkEngine.allocateFat = reinterpret_cast<ShrinkAllocateFn>(base + 0x15f5c0);
    g_shrinkEngine.releaseThin = reinterpret_cast<ShrinkReleaseFn>(base + 0x0abf20);
    g_shrinkEngine.releaseFat = reinterpret_cast<ShrinkReleaseFn>(base + 0x25ad60);
    void* publish = nullptr;
    if (!H->installHook(base + 0x3bc760, (void*)&ShrinkPublishDetour,
                        sizeof kShrinkPublishPrologue, &publish)) {
        H->log("instance shrink: publish hook failed; OFF");
        return false;
    }
    g_shrinkPublish = reinterpret_cast<ShrinkPublishFn>(publish);
    void* add = nullptr;
    if (!H->installHook(base + 0x1691c0, (void*)&ShrinkAddDetour,
                        sizeof kShrinkAddPrologue, &add)) {
        H->log("instance shrink: AddComponent hook failed; OFF (publish passes through)");
        return false;
    }
    g_shrinkEngine.addComponent = reinterpret_cast<ShrinkAddFn>(add);
    g_shrinkLive = true;
    H->log("instance shrink: 64 m tree/scenery cell lists trimmed to size at call 0x3bc8d1 "
           "(engine allocator and destructors; contents, order and arguments unchanged)");
    return true;
}

extern "C" __declspec(dllexport)
int BigmapTestInstallInstanceShrink(const Tpf2mpHost* host, int gog, int enabled)
{
    const auto oldHost = H; const bool oldGog = g_gog, oldEnabled = g_instanceShrink;
    H = host; g_gog = gog != 0; g_instanceShrink = enabled != 0;
    const bool result = InstallInstanceShrink();
    H = oldHost; g_gog = oldGog; g_instanceShrink = oldEnabled;
    return result;
}

// The publish wrapper with a caller-supplied host (the installer restores H).
extern "C" __declspec(dllexport)
void BigmapTestInvokeInstanceShrinkPublish(const Tpf2mpHost* host, void* self, void* engine,
    void* instances, float cell)
{
    const auto oldHost = H;
    H = host;
    ShrinkPublishDetour(self, engine, instances, cell);
    H = oldHost;
}

// The detour body for the matched call site, with fixture engine helpers.
// totals receives {bytes, cells, lists, keptFat} for this call.
extern "C" __declspec(dllexport)
void BigmapTestInstanceShrinkAdd(const ShrinkEngine* engine, void* a, void* b,
    ShrinkList* list, uint64_t flag, uint64_t dense, uint64_t* totals)
{
    const auto oldEngine = g_shrinkEngine; const auto oldTotals = g_shrinkTotals;
    g_shrinkEngine = *engine; g_shrinkTotals = {};
    ShrinkAddForward(true, a, b, list, flag, dense);
    totals[0] = g_shrinkTotals.bytes; totals[1] = g_shrinkTotals.cells;
    totals[2] = g_shrinkTotals.lists; totals[3] = g_shrinkTotals.keptFat;
    g_shrinkEngine = oldEngine; g_shrinkTotals = oldTotals;
}
