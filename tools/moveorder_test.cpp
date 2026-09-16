// Offline test for the ship/aircraft claim-order observer (native/src/moveorder.h,
// installed by slice_hook.cpp "SHIP AND AIRCRAFT CLAIM ORDER"). No game needed.
//
//   cl /nologo /std:c++17 /EHsc /MT /W3 /O2 tools\moveorder_test.cpp /Fe:moveorder_test.exe
//   moveorder_test.exe
//
// tools/moveorder_bytes_test.py is the other half: it checks the two hook sites
// and the relay against the shipped exe. This one checks the pure part -- the
// three numbers the observer logs, and what two peers can conclude from them:
//
//   nameHash  FNV over the NAMES in the engine's node order   (the claim order)
//   rankHash  the same names in name order                    (the fleet as a set)
//   idHash    FNV over the entity ids in node order           (this peer's shape)
//
// so: same n, same rankHash, different nameHash on two peers = the same fleet
// claiming locks in different orders. rankHash must therefore not depend on the
// registration order at all, and nameHash must.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdarg>
#include <algorithm>
#include <random>
#include <string>
#include <vector>

static bool Readable(const void* p, size_t n) { return p != nullptr || n == 0; }
static void Log(const char*, ...) {}
#include "../native/src/trainorder.h"
#include "../native/src/moveorder.h"

struct Vehicle { std::string name; int32_t id; };

// The hash folds ASCII case the way the compare does, so "the same name" here
// has to as well.
static std::string Fold(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

struct Measured {
    uint32_t nameHash, rankHash, idHash;
    MoveOrderOutcome o;
    std::vector<int32_t> rankedIds;
};

// What MoveOrderMeasure computes, over a node list in the given order.
static Measured Measure(const std::vector<Vehicle>& reg)
{
    const int64_t n = (int64_t)reg.size();
    std::vector<TrainOrderKey> keys((size_t)n);
    std::vector<int32_t> idx((size_t)n);
    for (int64_t i = 0; i < n; i++) {
        keys[(size_t)i].name = reg[(size_t)i].name.empty() ? nullptr : reg[(size_t)i].name.c_str();
        keys[(size_t)i].len = (uint32_t)reg[(size_t)i].name.size();
        keys[(size_t)i].id = reg[(size_t)i].id;
        keys[(size_t)i].score = 0;
        idx[(size_t)i] = (int32_t)i;
    }
    Measured m;
    m.nameHash = MoveOrderNameHash(idx.data(), n, keys.data());
    m.idHash = MoveOrderIdHash(idx.data(), n, keys.data());
    m.o = MoveOrderRank(idx.data(), n, keys.data());
    m.rankHash = MoveOrderNameHash(idx.data(), n, keys.data());
    for (int64_t i = 0; i < n; i++) m.rankedIds.push_back(keys[(size_t)idx[(size_t)i]].id);
    return m;
}

int main()
{
    std::mt19937 rng(20260916);
    const char* pool[] = { "Ferry", "Cargo Ship", "Tanker", "Clipper", "Barge", "Liner",
                           "ferry", "CARGO SHIP", "Jumbo", "Dash 8", "Boeing 747", "" };

    // 1. registration-order independence of what two peers compare: rankHash
    //    and the ranked id sequence never move; nameHash and idHash do whenever
    //    the order actually differs by name / by id.
    {
        int worlds = 0, nameMoved = 0, idMoved = 0;
        for (int w = 0; w < 300; w++) {
            const int n = 2 + (int)(rng() % 40);
            std::vector<Vehicle> a((size_t)n);
            for (int i = 0; i < n; i++) a[(size_t)i] = { pool[rng() % 12], 1000 + (int32_t)(rng() % 5000) };
            std::vector<Vehicle> b = a;
            std::shuffle(b.begin(), b.end(), rng);
            const Measured ma = Measure(a), mb = Measure(b);
            assert(!ma.o.refused && !mb.o.refused);
            assert(ma.rankHash == mb.rankHash);
            assert(ma.rankedIds == mb.rankedIds || ma.o.duplicates);
            assert(ma.o.named == mb.o.named);
            bool sameNameSeq = true, sameIdSeq = true;
            for (int i = 0; i < n; i++) {
                if (Fold(a[(size_t)i].name) != Fold(b[(size_t)i].name)) sameNameSeq = false;
                if (a[(size_t)i].id != b[(size_t)i].id) sameIdSeq = false;
            }
            if (!sameNameSeq) { assert(ma.nameHash != mb.nameHash); nameMoved++; }
            else assert(ma.nameHash == mb.nameHash);
            if (!sameIdSeq) { assert(ma.idHash != mb.idHash); idMoved++; }
            else assert(ma.idHash == mb.idHash);
            worlds++;
        }
        printf("PASS independence: %d fleets measured from two registration orders -- rankHash and "
               "the ranked ids never moved, nameHash moved in %d (every one whose name sequence "
               "differed), idHash in %d\n", worlds, nameMoved, idMoved);
    }

    // 2. the rank rule: name (ASCII case-insensitive), then id; blanks first;
    //    `changed` says whether the engine's order was already that.
    {
        std::vector<Vehicle> v = { {"Tanker", 5}, {"ferry", 9}, {"", 3}, {"Ferry", 4}, {"barge", 7} };
        Measured m = Measure(v);
        assert(!m.o.refused && m.o.changed && !m.o.duplicates && m.o.named == 4);
        assert((m.rankedIds == std::vector<int32_t>{ 3, 7, 4, 9, 5 }));
        std::vector<Vehicle> sorted = { {"", 3}, {"barge", 7}, {"Ferry", 4}, {"ferry", 9}, {"Tanker", 5} };
        Measured s = Measure(sorted);
        assert(!s.o.changed && s.rankHash == m.rankHash && s.nameHash == s.rankHash);
        printf("PASS rule: blank first, then name case-folded, ties by id; an engine already in "
               "name order reports reordered=0 and nameHash == rankHash\n");
    }

    // 3. the hash: length-prefixed and case-folded exactly as the compare folds.
    {
        std::vector<Vehicle> ab = { {"ab", 1}, {"c", 2} };
        std::vector<Vehicle> a_bc = { {"a", 1}, {"bc", 2} };
        assert(Measure(ab).nameHash != Measure(a_bc).nameHash);
        std::vector<Vehicle> lower = { {"ferry one", 1}, {"barge", 2} };
        std::vector<Vehicle> upper = { {"FERRY ONE", 1}, {"Barge", 2} };
        assert(Measure(lower).nameHash == Measure(upper).nameHash);
        assert(Measure(lower).rankHash == Measure(upper).rankHash);
        std::vector<Vehicle> umlaut = { {"F\xc3\xa4hre", 1} };
        std::vector<Vehicle> umlaut2 = { {"F\xc3\x84hre", 1} };            // not folded: only A-Z is
        assert(Measure(umlaut).nameHash != Measure(umlaut2).nameHash);
        printf("PASS hash: \"ab\",\"c\" != \"a\",\"bc\"; ASCII case is folded, non-ASCII bytes are not\n");
    }

    // 4. refusals and edge cases leave nothing half-done.
    {
        std::vector<TrainOrderKey> k(3);
        int32_t idx[3] = { 0, 2, 1 };
        assert(!strcmp(MoveOrderRank(idx, 3, k.data()).refused, "not iota"));
        int32_t iota[3] = { 0, 1, 2 };
        assert(!strcmp(MoveOrderRank(iota, 3, nullptr).refused, "no keys"));
        assert(!strcmp(MoveOrderRank(iota, -1, k.data()).refused, "bad count"));
        assert(!strcmp(MoveOrderRank(nullptr, 3, k.data()).refused, "bad count"));
        k[1].len = 3; k[1].name = nullptr;
        assert(!strcmp(MoveOrderRank(iota, 3, k.data()).refused, "key without text"));
        assert(iota[0] == 0 && iota[1] == 1 && iota[2] == 2);
        int32_t one[1] = { 0 };
        const MoveOrderOutcome o1 = MoveOrderRank(one, 1, k.data());
        assert(!o1.refused && !o1.changed);
        std::vector<Vehicle> dup = { {"Ferry", 7}, {"ferry", 7} };
        assert(Measure(dup).o.duplicates);
        printf("PASS refusals: a non-iota array, no keys, a bad count and a length with no text "
               "refuse and leave the array untouched; a single node is a no-op; an "
               "indistinguishable pair is flagged\n");
    }

    // 5. scale: the observer runs once per sim step, so it should be cheap.
    {
        std::vector<Vehicle> big(2000);
        for (auto& v : big) v = { pool[rng() % 12], (int32_t)(rng() % 100000) };
        std::vector<Vehicle> other = big;
        std::shuffle(other.begin(), other.end(), rng);
        assert(Measure(big).rankHash == Measure(other).rankHash);
        printf("PASS scale: 2000 vehicles measured identically from two registration orders\n");
    }

    printf("ALL PASS moveorder\n");
    return 0;
}
