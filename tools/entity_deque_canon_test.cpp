// native/src/entity_deque_canon.h against synthetic deques in the MSVC and
// libstdc++ layouts (docs/re/TERMINAL_WAIT_ORDER.md, container B).
//   cl /nologo /EHsc /O2 tools\entity_deque_canon_test.cpp   (or g++ -O2)
#include "../native/src/entity_deque_canon.h"
#include <cstdio>
#include <cstring>
#include <random>

static int failures = 0;
static void check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

struct Msvc {   // 0x28 bytes: +8 map, +0x10 mapsize, +0x18 off, +0x20 size
    uint8_t raw[0x28];
    std::vector<std::vector<int32_t>> blocks;
    std::vector<int32_t*> map;
    Msvc(size_t mapsize, size_t off, const std::vector<int32_t>& ids) : blocks(mapsize, std::vector<int32_t>(4, -7)), map(mapsize)
    {
        for (size_t i = 0; i < mapsize; i++) map[i] = blocks[i].data();
        for (size_t i = 0; i < ids.size(); i++) map[((off + i) >> 2) & (mapsize - 1)][(off + i) & 3] = ids[i];
        memset(raw, 0, sizeof raw);
        int32_t** m = map.data();
        size_t size = ids.size();
        memcpy(raw + 8, &m, 8); memcpy(raw + 0x10, &mapsize, 8); memcpy(raw + 0x18, &off, 8); memcpy(raw + 0x20, &size, 8);
    }
    std::vector<int32_t> read(size_t off, size_t n)
    {
        std::vector<int32_t> v;
        for (size_t i = 0; i < n; i++) v.push_back(map[((off + i) >> 2) & (map.size() - 1)][(off + i) & 3]);
        return v;
    }
};

struct Gnu {    // map, mapsize, start{cur,first,last,node}, finish{cur,first,last,node}
    uint8_t raw[0x50];
    std::vector<std::vector<int32_t>> blocks;
    std::vector<int32_t*> map;
    size_t startNode, startIdx, n;
    Gnu(size_t mapsize, size_t startNode_, size_t startIdx_, const std::vector<int32_t>& ids)
        : blocks(mapsize, std::vector<int32_t>(128, -7)), map(mapsize), startNode(startNode_), startIdx(startIdx_), n(ids.size())
    {
        for (size_t i = 0; i < mapsize; i++) map[i] = blocks[i].data();
        size_t node = startNode, idx = startIdx;
        for (int32_t id : ids) { map[node][idx] = id; if (++idx == 128) { idx = 0; node++; } }
        memset(raw, 0, sizeof raw);
        int32_t** m = map.data();
        int32_t* scur = map[startNode] + startIdx; int32_t* sfirst = map[startNode]; int32_t* slast = sfirst + 128; int32_t** snode = m + startNode;
        int32_t* fcur = map[node] + idx; int32_t* ffirst = map[node]; int32_t* flast = ffirst + 128; int32_t** fnode = m + node;
        memcpy(raw + 0x00, &m, 8); memcpy(raw + 0x08, &mapsize, 8);
        memcpy(raw + 0x10, &scur, 8); memcpy(raw + 0x18, &sfirst, 8); memcpy(raw + 0x20, &slast, 8); memcpy(raw + 0x28, &snode, 8);
        memcpy(raw + 0x30, &fcur, 8); memcpy(raw + 0x38, &ffirst, 8); memcpy(raw + 0x40, &flast, 8); memcpy(raw + 0x48, &fnode, 8);
    }
    std::vector<int32_t> read()
    {
        std::vector<int32_t> v;
        size_t node = startNode, idx = startIdx;
        for (size_t i = 0; i < n; i++) { v.push_back(map[node][idx]); if (++idx == 128) { idx = 0; node++; } }
        return v;
    }
};

int main()
{
    std::vector<int32_t*> at;
    std::vector<int32_t> ids;
    std::mt19937 rng(7);

    // MSVC: a wrapped deque (offset near the end of the map), shuffled ids
    {
        std::vector<int32_t> v;
        for (int i = 0; i < 23; i++) v.push_back(1000 + 37 * i);
        std::vector<int32_t> want = v;
        std::shuffle(v.begin(), v.end(), rng);
        Msvc d(8, 29, v);
        check(DequeCanonMsvc(d.raw, at, ids) == DQ_REORDERED, "msvc: shuffled wrapped deque reordered");
        check(d.read(29, 23) == want, "msvc: now ascending, same ids");
        size_t untouched = 0;
        for (size_t b = 0; b < 8; b++) for (int k = 0; k < 4; k++) untouched += d.blocks[b][k] == -7;
        check(untouched == 32 - 23, "msvc: cells outside the deque untouched");
        check(DequeCanonMsvc(d.raw, at, ids) == DQ_SORTED, "msvc: second pass finds it sorted");
    }
    {
        Msvc d(4, 0, { 5 });
        check(DequeCanonMsvc(d.raw, at, ids) == DQ_SORTED, "msvc: one id is sorted");
        Msvc bad(6, 0, { 3, 2, 1 });   // map size not a power of two
        check(DequeCanonMsvc(bad.raw, at, ids) == DQ_REFUSED && bad.read(0, 3) == std::vector<int32_t>({ 3, 2, 1 }),
              "msvc: a non-power-of-two map is refused, untouched");
        Msvc big(2, 0, { 1, 2, 3 });
        size_t tooMany = 9; memcpy(big.raw + 0x20, &tooMany, 8);
        check(DequeCanonMsvc(big.raw, at, ids) == DQ_REFUSED, "msvc: more ids than the map holds is refused");
    }

    // libstdc++: spans three blocks, starting mid-block
    {
        std::vector<int32_t> v;
        for (int i = 0; i < 300; i++) v.push_back(50000 - 11 * i);
        std::vector<int32_t> want = v;
        std::sort(want.begin(), want.end());
        std::shuffle(v.begin(), v.end(), rng);
        Gnu d(6, 1, 100, v);
        check(DequeCanonGnu(d.raw, at, ids) == DQ_REORDERED, "gnu: shuffled 3-block deque reordered");
        check(d.read() == want, "gnu: now ascending, same ids");
        check(d.blocks[1][99] == -7 && d.blocks[0][0] == -7, "gnu: cells before the start untouched");
        check(DequeCanonGnu(d.raw, at, ids) == DQ_SORTED, "gnu: second pass finds it sorted");
    }
    {
        Gnu e(2, 0, 5, {});
        check(DequeCanonGnu(e.raw, at, ids) == DQ_SORTED, "gnu: empty deque");
        Gnu full(3, 0, 120, { 9, 8, 7, 6, 5, 4, 3, 2 });   // ends exactly... crosses into block 1
        check(DequeCanonGnu(full.raw, at, ids) == DQ_REORDERED && full.read() == std::vector<int32_t>({ 2, 3, 4, 5, 6, 7, 8, 9 }),
              "gnu: a deque crossing a block boundary");
        Gnu bad(3, 0, 0, { 3, 2, 1 });
        int32_t** swapped = full.map.data() + 2; memcpy(bad.raw + 0x28, &swapped, 8);   // start node after finish node
        check(DequeCanonGnu(bad.raw, at, ids) == DQ_REFUSED && bad.read() == std::vector<int32_t>({ 3, 2, 1 }),
              "gnu: an inconsistent header is refused, untouched");
    }
    printf(failures ? "FAILED: %d\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
