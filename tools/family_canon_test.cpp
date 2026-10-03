// Offline test for the per-step ECS node-list canonicaliser (native/src/family_canon.h,
// installed by slice/hotjoin_order.inl, site "step"). No game needed.
//
//   cl /nologo /std:c++17 /EHsc /MT /W3 /O2 tools\family_canon_test.cpp /Fe:family_canon_test.exe
//   family_canon_test.exe
//
// tools/hotjoin_order_bytes_test.py checks the hook site against the exe. This
// one builds NodeList<N> images (node vector + a flat index with ctrl bytes and
// {entity, position} slots, the engine's layout) and checks that:
//   - the result is a pure function of the entity SET: two lists that took
//     different add / swap-remove histories to the same set come out identical,
//     nodes and component indices together;
//   - every index slot names its node's new position; ctrl bytes and keys are
//     untouched;
//   - a list that is already sorted is not written at all;
//   - an invalid tail index is refused and nothing moves.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <vector>

static bool Readable(const void* p, size_t n) { return p != nullptr || n == 0; }
#include "../native/src/family_canon.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// A NodeList<N> image driven by the engine's own Add / Remove rules.
struct FakeList {
    int N;
    size_t stride;
    std::vector<uint8_t> nodes;     // n * stride
    std::vector<int8_t> ctrl;       // cap
    std::vector<int32_t> slots;     // 2 * cap
    uint8_t image[0x40] = {};
    std::mt19937 hashRng;

    FakeList(int n, size_t cap, uint32_t seed) : N(n), stride(4 + 4 * (size_t)n), ctrl(cap, (int8_t)-128), slots(2 * cap, 0), hashRng(seed) {}
    size_t count() const { return nodes.size() / stride; }
    int32_t key(size_t i) const { int32_t k; memcpy(&k, &nodes[i * stride], 4); return k; }
    // "hash" = any free slot (a seeded probe), which is all the canonicaliser may assume
    long find(int32_t e) const { for (size_t j = 0; j < ctrl.size(); j++) if (ctrl[j] >= 0 && slots[2 * j] == e) return (long)j; return -1; }
    void add(int32_t e) {
        std::vector<uint8_t> node(stride);
        memcpy(&node[0], &e, 4);
        for (int c = 0; c < N; c++) { int32_t v = e * 10 + c; memcpy(&node[4 + 4 * c], &v, 4); }
        const size_t pos = count();
        nodes.insert(nodes.end(), node.begin(), node.end());
        size_t j;
        do { j = hashRng() % ctrl.size(); } while (ctrl[j] >= 0);
        ctrl[j] = (int8_t)(e & 0x7f);
        slots[2 * j] = e;
        slots[2 * j + 1] = (int32_t)pos;
    }
    void remove(int32_t e) {
        const long s = find(e);
        if (s < 0) abort();
        const size_t hole = (size_t)slots[2 * s + 1];
        ctrl[(size_t)s] = (int8_t)-2;   // deleted
        const size_t last = count() - 1;
        if (hole != last) {
            memcpy(&nodes[hole * stride], &nodes[last * stride], stride);
            slots[2 * find(key(hole)) + 1] = (int32_t)hole;
        }
        nodes.resize(last * stride);
    }
    uint8_t* nl() {
        uint8_t* b = nodes.empty() ? nullptr : nodes.data();
        uint8_t* e = b ? b + nodes.size() : nullptr;
        memcpy(image + 0x08, &b, 8);
        memcpy(image + 0x10, &e, 8);
        memcpy(image + 0x18, &e, 8);
        int8_t* c = ctrl.data(); int32_t* s = slots.data();
        size_t sz = count(), cap = ctrl.size();
        memcpy(image + 0x20, &c, 8);
        memcpy(image + 0x28, &s, 8);
        memcpy(image + 0x30, &sz, 8);
        memcpy(image + 0x38, &cap, 8);
        return image;
    }
    bool indexConsistent() const {
        size_t full = 0;
        for (size_t j = 0; j < ctrl.size(); j++) {
            if (ctrl[j] < 0) continue;
            full++;
            const int32_t p = slots[2 * j + 1];
            if (p < 0 || (size_t)p >= count() || key((size_t)p) != slots[2 * j]) return false;
        }
        return full == count();
    }
    bool sortedWithComponents() const {
        for (size_t i = 0; i < count(); i++) {
            if (i && key(i) <= key(i - 1)) return false;
            for (int c = 0; c < N; c++) { int32_t v; memcpy(&v, &nodes[i * stride + 4 + 4 * c], 4); if (v != key(i) * 10 + c) return false; }
        }
        return true;
    }
};

static int Canon(FakeList& l, FamilyCanonScratch& s, size_t* moved = nullptr)
{
    size_t m = 0; const char* why = nullptr;
    const int r = FamilyCanonList(l.nl(), l.stride, s, &m, &why);
    if (moved) *moved = m;
    return r;
}

// Two histories reaching the same set: registration order (a load) vs add/remove churn (a running world).
static void TestHistoriesAgree(int N, uint32_t seed, int ops, bool canonEveryStep)
{
    std::mt19937 rng(seed);
    FakeList run(N, 4096, seed), load(N, 4096, seed ^ 0x9e3779b9u);
    FamilyCanonScratch s;
    std::vector<int32_t> alive;
    int32_t next = 1000;
    for (int i = 0; i < ops; i++) {
        if (alive.size() < 5 || rng() % 3) { alive.push_back(next); run.add(next); next += 1 + (int32_t)(rng() % 3); }
        else { size_t k = rng() % alive.size(); run.remove(alive[k]); alive.erase(alive.begin() + (long)k); }
        if (canonEveryStep && rng() % 4 == 0) { CHECK(Canon(run, s) != FC_REFUSED); CHECK(run.indexConsistent()); CHECK(run.sortedWithComponents()); }
    }
    std::vector<int32_t> reg(alive.rbegin(), alive.rend());   // reverse id order, like Engine::Load
    std::sort(reg.begin(), reg.end(), std::greater<int32_t>());
    for (int32_t e : reg) load.add(e);
    CHECK(Canon(run, s) != FC_REFUSED);
    CHECK(Canon(load, s) != FC_REFUSED);
    CHECK(run.indexConsistent());
    CHECK(load.indexConsistent());
    CHECK(run.sortedWithComponents());
    CHECK(run.nodes == load.nodes);
    // keep going on both with the SAME ops: they stay identical (canonicalised each step)
    for (int i = 0; i < ops / 2; i++) {
        if (alive.size() < 5 || rng() % 2) { alive.push_back(next); run.add(next); load.add(next); next++; }
        else { size_t k = rng() % alive.size(); run.remove(alive[k]); load.remove(alive[k]); alive.erase(alive.begin() + (long)k); }
        CHECK(Canon(run, s) != FC_REFUSED);
        CHECK(Canon(load, s) != FC_REFUSED);
        CHECK(run.nodes == load.nodes);
    }
    CHECK(run.indexConsistent() && load.indexConsistent());
}

static void TestSortedUntouched()
{
    FakeList l(2, 64, 7);
    for (int32_t e = 1; e <= 20; e++) l.add(e);
    std::vector<int32_t> before = l.slots;
    FamilyCanonScratch s;
    size_t m = 99;
    CHECK(Canon(l, s, &m) == FC_SORTED);
    CHECK(m == 0);
    CHECK(l.slots == before);
    FakeList empty(1, 16, 1);
    CHECK(Canon(empty, s) == FC_SORTED);
}

static void TestOneRemoval()
{
    FakeList l(1, 256, 3);
    for (int32_t e = 1; e <= 100; e++) l.add(e);
    l.remove(40);   // 100 moves into position 39
    FamilyCanonScratch s;
    size_t m = 0;
    CHECK(Canon(l, s, &m) == FC_REORDERED);
    CHECK(m == 60);   // 41..99 shift down one (59), 100 goes back to the end
    CHECK(l.sortedWithComponents());
    CHECK(l.indexConsistent());
}

static void TestRefusals()
{
    FamilyCanonScratch s;
    {   // an index slot pointing at the wrong node
        FakeList l(1, 64, 5);
        for (int32_t e = 10; e > 0; e--) l.add(e);
        std::vector<uint8_t> before = l.nodes;
        l.slots[2 * l.find(5) + 1] = 0;
        CHECK(Canon(l, s) == FC_REFUSED);
        CHECK(l.nodes == before);
    }
    {   // size mismatch
        FakeList l(1, 64, 5);
        for (int32_t e = 10; e > 0; e--) l.add(e);
        uint8_t* nl = l.nl();
        size_t bad = 3; memcpy(nl + 0x30, &bad, 8);
        size_t m; const char* why;
        CHECK(FamilyCanonList(nl, l.stride, s, &m, &why) == FC_REFUSED);
    }
    {   // a vector that is not a whole number of nodes
        FakeList l(2, 64, 5);
        for (int32_t e = 10; e > 0; e--) l.add(e);
        uint8_t* nl = l.nl();
        size_t m; const char* why;
        uint8_t* e = *(uint8_t**)(nl + 0x10) - 4; memcpy(nl + 0x10, &e, 8);
        CHECK(FamilyCanonList(nl, l.stride, s, &m, &why) == FC_REFUSED);
    }
}


// Tail optimization: include prefix nodes reached by a newly appended low key,
// retain components at all widths, and reuse scratch across shrinking/growing tails.
static void TestTails(int N)
{
    FamilyCanonScratch s;
    const std::vector<std::vector<int32_t>> cases = {
        {1, 2, 3, 4, 6, 5},                 // two-node tail
        {1, 3, 5, 7, 9, 4},                 // merge back into the prefix
        {-9, -7, -5, -3, -1, -6},           // signed entity ordering
        {1, 2, 30, 4, 5, 6, 7, 8, 9, 10, 11, 12}, // sparse extraction
        {1, 2, 9, 8, 7, 6, 5, 4, 3},        // whole-region sort
        {1, 2, 3, 5, 4}
    };
    for (const auto& ids : cases) {
        FakeList l(N, 64, 23);
        for (auto id : ids) l.add(id);
        const auto before = l.nodes;
        const auto slots = l.slots;
        const auto ctrl = l.ctrl;
        auto sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        size_t expectedMoved = 0, prefix = 0;
        while (prefix < ids.size() && ids[prefix] == sorted[prefix]) ++prefix;
        for (size_t j = 0; j < ids.size(); ++j) expectedMoved += ids[j] != sorted[j];
        size_t moved = 0;
        CHECK(Canon(l, s, &moved) == FC_REORDERED);
        CHECK(moved == expectedMoved);
        CHECK(l.sortedWithComponents() && l.indexConsistent());
        CHECK(std::equal(before.begin(), before.begin() + prefix * l.stride, l.nodes.begin()));
        CHECK(l.ctrl == ctrl);
        for (size_t j = 0; j < ctrl.size(); ++j) {
            CHECK(l.slots[2*j] == slots[2*j]);
            if (ctrl[j] < 0 || (size_t)slots[2*j+1] < prefix)
                CHECK(l.slots[2*j+1] == slots[2*j+1]);
        }
        const auto canonical = l.nodes;
        const auto canonicalSlots = l.slots;
        CHECK(Canon(l, s, &moved) == FC_SORTED && moved == 0);
        CHECK(l.nodes == canonical && l.slots == canonicalSlots);
    }
    // Duplicate in the region, or equal to a key in the earlier prefix.
    for (const auto& ids : std::vector<std::vector<int32_t>>{{1,2,3,4,4}, {1,2,3,4,2}}) {
        FakeList l(N, 64, 23);
        for (auto id : ids) l.add(id);
        const auto before = l.nodes;
        const auto slots = l.slots;
        CHECK(Canon(l, s) == FC_REFUSED);
        CHECK(l.nodes == before && l.slots == slots);
    }
    // Even untouched prefix slots must have positions in range; a bad tail
    // key must also fail before either nodes or index positions are written.
    for (int mode = 0; mode < 3; ++mode) {
        FakeList l(N, 64, 23);
        for (auto id : {1,2,3,4,6,5}) l.add(id);
        if (mode < 2) l.slots[2*l.find(1)+1] = mode ? 6 : -1;
        else l.slots[2*l.find(6)] = 99;
        const auto before = l.nodes;
        const auto slots = l.slots;
        CHECK(Canon(l, s) == FC_REFUSED);
        CHECK(l.nodes == before && l.slots == slots);
    }
}

int main()
{
    TestSortedUntouched();
    TestOneRemoval();
    TestRefusals();
    for (int N = 1; N <= 5; ++N) TestTails(N);
    for (int N = 1; N <= 5; N++)
        for (uint32_t seed = 1; seed <= 40; seed++) {
            TestHistoriesAgree(N, seed * 7919u + (uint32_t)N, 200 + (int)(seed * 23 % 900), seed & 1);
        }
    // a big, badly shuffled list goes through the whole-sort path
    {
        FakeList l(1, 1 << 15, 11);
        std::vector<int32_t> ids(12000);
        for (int i = 0; i < 12000; i++) ids[(size_t)i] = i * 2 + 1;
        std::shuffle(ids.begin(), ids.end(), std::mt19937(5));
        for (int32_t e : ids) l.add(e);
        FamilyCanonScratch s;
        CHECK(Canon(l, s) == FC_REORDERED);
        CHECK(l.sortedWithComponents() && l.indexConsistent());
    }
    if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
    printf("family_canon_test: all passed\n");
    return 0;
}
