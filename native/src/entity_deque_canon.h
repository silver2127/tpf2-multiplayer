// Sort a std::deque<ecs::Entity> (int32 ids) ascending in place, for the
// hot-join order canon (docs/re/TERMINAL_WAIT_ORDER.md, container B: the people
// and cargo riding a vehicle, unloaded from the front). Only positions move;
// the deque's own bookkeeping is never written. Portable: MSVC layout for the
// Windows slice, libstdc++ layout for the native Linux boot library.
//
// Returns DQ_SORTED (already ascending, or fewer than two ids), DQ_REORDERED,
// or DQ_REFUSED (the header does not describe a deque of that shape: untouched).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

enum { DQ_SORTED = 0, DQ_REORDERED = 1, DQ_REFUSED = -1 };
static const size_t DQ_MAX_IDS = (size_t)1 << 22;

// Gathers the element addresses, sorts the values, writes them back in order.
static inline int DequeCanonApply(std::vector<int32_t*>& at, std::vector<int32_t>& ids)
{
    ids.clear();
    for (int32_t* p : at) ids.push_back(*p);
    if (std::is_sorted(ids.begin(), ids.end())) return DQ_SORTED;
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < at.size(); i++) *at[i] = ids[i];
    return DQ_REORDERED;
}

// MSVC std::deque<int32_t> (0x28 bytes): +0x08 map (int32_t**), +0x10 map size
// (a power of two), +0x18 offset, +0x20 size; 4 ids per block; element i is
// map[((off + i) >> 2) & (mapsize - 1)][(off + i) & 3].
static inline int DequeCanonMsvc(uint8_t* d, std::vector<int32_t*>& at, std::vector<int32_t>& ids)
{
    int32_t** map = *(int32_t***)(d + 0x08);
    const size_t mapsize = *(size_t*)(d + 0x10), off = *(size_t*)(d + 0x18), size = *(size_t*)(d + 0x20);
    if (size < 2) return DQ_SORTED;
    if (!map || !mapsize || (mapsize & (mapsize - 1)) || size > DQ_MAX_IDS || size > mapsize * 4) return DQ_REFUSED;
    at.clear();
    for (size_t i = 0; i < size; i++) {
        int32_t* block = map[((off + i) >> 2) & (mapsize - 1)];
        if (!block) return DQ_REFUSED;
        at.push_back(block + ((off + i) & 3));
    }
    return DequeCanonApply(at, ids);
}

// libstdc++ std::deque<int32_t>: +0x00 map, +0x08 map size, start iterator
// {cur +0x10, first +0x18, last +0x20, node +0x28}, finish {cur +0x30, first
// +0x38, last +0x40, node +0x48}; 128 ids per 0x200-byte block.
static inline int DequeCanonGnu(uint8_t* d, std::vector<int32_t*>& at, std::vector<int32_t>& ids)
{
    int32_t* cur = *(int32_t**)(d + 0x10);
    int32_t* first = *(int32_t**)(d + 0x18);
    int32_t* last = *(int32_t**)(d + 0x20);
    int32_t** node = *(int32_t***)(d + 0x28);
    int32_t* finish = *(int32_t**)(d + 0x30);
    int32_t** finishNode = *(int32_t***)(d + 0x48);
    int32_t** map = *(int32_t***)(d + 0x00);
    const size_t mapsize = *(size_t*)(d + 0x08);
    if (!cur || !node || !finishNode || !map || node < map || finishNode < node || finishNode >= map + mapsize ||
        last - first != 128 || cur < first || cur >= last || *node != first)
        return DQ_REFUSED;
    at.clear();
    while (cur != finish) {
        if (at.size() >= DQ_MAX_IDS) return DQ_REFUSED;
        at.push_back(cur);
        if (++cur == last) {
            if (node == finishNode) return DQ_REFUSED;   // walked past the finish block
            cur = *++node;
            if (!cur) return DQ_REFUSED;
            last = cur + 128;
        }
    }
    if (node != finishNode) return DQ_REFUSED;
    if (at.size() < 2) return DQ_SORTED;
    return DequeCanonApply(at, ids);
}
