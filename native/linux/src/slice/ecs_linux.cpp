// See ecs_linux.h. Nothing here calls the engine's asserting accessor, and no
// read leaves the pool's own bounds.
#include "ecs_linux.h"
#include "ecs_checks_linux.h"
#include "slice_core.h"
#include <cstring>
#include <mutex>
#include <vector>

namespace {

using TypeFindFn = uintptr_t (*)(void* mgr, const uintptr_t* ti);

uintptr_t ecsBase;

} // namespace

namespace { bool ecsAnchored; }

void SliceEcsSetBase(uintptr_t base) { ecsBase = base; }

// Nothing here works until this has passed: an image whose component read does
// not match build 35924 may lay its pools out differently.
bool SliceEcsAnchored(uintptr_t base)
{
    ecsAnchored = false;
    for (const auto& c : kEcsChecks) {
        std::vector<char> actual(c.size);
        if (!SliceRead(base + c.rva, actual.data(), actual.size()) ||
            memcmp(actual.data(), c.bytes, c.size)) {
            SliceLog("[ecs] byte guard failed at %lx\n", (unsigned long)c.rva);
            return false;
        }
    }
    return ecsAnchored = true;
}

int SliceEcsTypeIndex(uintptr_t engine, uintptr_t typeinfoRva)
{
    if (!ecsBase || !ecsAnchored || !engine) return -1;
    // Type indices belong to a world; drop the cache when the engine changes.
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    struct Entry { uintptr_t ti; int index; };
    static uintptr_t cachedEngine;
    static Entry cache[8];
    static size_t used;
    if (cachedEngine != engine) { cachedEngine = engine; used = 0; }
    for (size_t i = 0; i < used; ++i)
        if (cache[i].ti == typeinfoRva) return cache[i].index;
    const uintptr_t ti = ecsBase + typeinfoRva;
    const uintptr_t node = reinterpret_cast<TypeFindFn>(ecsBase + SLICE_RVA_TYPE_FIND)(
        reinterpret_cast<void*>(engine + 0x48), &ti);
    int index = -1;
    uintptr_t stored = 0;
    if (node && SliceReadT(node + 0x10, &stored) && stored > 0 && stored <= 4097)
        index = int(stored) - 1;
    if (used < sizeof(cache) / sizeof(cache[0])) cache[used++] = {typeinfoRva, index};
    return index;
}

int32_t SliceEcsComponentSlot(uintptr_t engine, int32_t entity, int type)
{
    if (!engine || entity < 0 || type < 0) return -1;
    uintptr_t entities = 0;
    if (!SliceReadT(engine + 0x98, &entities) || !entities) return -1;
    SliceVec slots{};
    if (!SliceReadStdVector(entities + size_t(entity) * 24, 8, 4096, &slots)) return -1;
    for (size_t i = 0; i < slots.count; ++i) {
        int32_t pair[2];
        if (!SliceRead(slots.begin + i * 8, pair, sizeof(pair))) return -1;
        if (pair[0] == type) return pair[1];
    }
    return -1;
}

uintptr_t SliceEcsComponentAt(uintptr_t engine, int type, int32_t slot, size_t stride)
{
    if (!engine || type < 0 || slot < 0 || !stride) return 0;
    uintptr_t pools = 0, pool = 0;
    if (!SliceReadT(engine + 0x80, &pools) || !pools ||
        !SliceReadT(pools + size_t(type) * 8, &pool) || !pool) return 0;
    if (slot < 0x40000000) {
        uintptr_t begin = 0, end = 0;
        if (!SliceReadT(pool + 0xb8, &begin) || !begin ||
            !SliceReadT(pool + 0xc0, &end) || end < begin) return 0;
        const uintptr_t at = begin + size_t(slot) * stride;
        return (at >= begin && at + stride <= end) ? at : 0;
    }
    const uint32_t entry = uint32_t(slot) - 0x40000000u;
    uintptr_t pages = 0, page = 0;
    if (!SliceReadT(pool + 0xd0, &pages) || !pages ||
        !SliceReadT(pages + size_t(entry / 32) * 16, &page) || !page) return 0;
    return page + size_t(entry % 32) * stride;
}

namespace {
// -2 = the entity has no PlayerOwned at all; -1 = it has one that did not read.
int OwnerAt(uintptr_t engine, int playerOwned, int32_t entity)
{
    const int32_t slot = SliceEcsComponentSlot(engine, entity, playerOwned);
    if (slot < 0) return -2;
    const uintptr_t at = SliceEcsComponentAt(engine, playerOwned, slot, SLICE_STRIDE_PLAYEROWNED);
    int32_t owner = -1;
    return (at && SliceRead(at, &owner, sizeof(owner))) ? owner : -1;
}
} // namespace

int SliceEcsOwner(uintptr_t engine, int32_t entity)
{
    const int playerOwned = SliceEcsTypeIndex(engine, SLICE_TI_PLAYEROWNED);
    if (playerOwned < 0) return -1;
    const int direct = OwnerAt(engine, playerOwned, entity);
    if (direct != -2) return direct < 0 ? -1 : direct;
    // The Windows shape: a station group that is not owned itself.
    const int stationGroup = SliceEcsTypeIndex(engine, SLICE_TI_STATIONGROUP);
    if (stationGroup < 0) return -1;
    const int32_t slot = SliceEcsComponentSlot(engine, entity, stationGroup);
    if (slot < 0) return -1;
    const uintptr_t group = SliceEcsComponentAt(engine, stationGroup, slot, SLICE_STRIDE_STATIONGROUP);
    if (!group) return -1;
    SliceVec stations{};
    if (!SliceReadStdVector(group, 4, 1 << 16, &stations) || !stations.count) return -1;
    int32_t first = -1;
    if (!SliceRead(stations.begin, &first, sizeof(first))) return -1;
    const int owner = OwnerAt(engine, playerOwned, first);
    return owner < 0 ? -1 : owner;
}

bool SliceEcsIsCompany(uintptr_t engine, int32_t entity)
{
    const int player = SliceEcsTypeIndex(engine, SLICE_TI_PLAYER);
    return player >= 0 && SliceEcsComponentSlot(engine, entity, player) >= 0;
}
