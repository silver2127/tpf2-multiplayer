// Offline test for the train reservation-order patch (native/src/trainorder.h,
// installed by slice_hook.cpp "TRAIN RESERVATION ORDER"). No game needed.
//
//   cl /nologo /std:c++17 /EHsc /MT /W3 tools\trainorder_test.cpp /Fe:trainorder_test.exe
//   trainorder_test.exe
//
// tools/trainorder_bytes_test.py is the other half: it checks the hook site and
// the RVAs against the shipped exe. This one checks the rule itself:
//
//   rank r  = sorted by name (ASCII-case-insensitive, byte-wise) then entity id
//   jitter j = minstd_rand(game-time seed), drawn in rank order, mod max(2,n/3)
//   reserve in order of (r + j), ties by entity id
//
// The properties that matter are that the result depends on nothing but the
// keys and the seed (so two peers compute it identically from the same world,
// whatever order their engines registered the trains in), and that the jitter
// is real enough to stop a late-alphabet train being starved at a junction.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdarg>
#include <algorithm>
#include <ctime>
#include <random>
#include <string>
#include <vector>

static bool Readable(const void* p, size_t n) { return p != nullptr || n == 0; }
static void Log(const char*, ...) {}
#include "../native/src/trainorder.h"

struct Train { std::string name; int32_t id; };

// Run the real ordering over a node list in the given registration order and
// report the entity ids in the order they would reserve track.
static std::vector<int32_t> Order(const std::vector<Train>& reg, uint32_t seed,
                                  TrainOrderOutcome* outcome = nullptr,
                                  std::vector<int32_t>* scores = nullptr)
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
    const TrainOrderOutcome o = TrainOrderArrange(idx.data(), n, keys.data(), seed);
    if (outcome) *outcome = o;
    std::vector<int32_t> out((size_t)n);
    for (int64_t i = 0; i < n; i++) out[(size_t)i] = keys[(size_t)idx[(size_t)i]].id;
    if (scores) {
        scores->resize((size_t)n);
        for (int64_t i = 0; i < n; i++) (*scores)[(size_t)i] = keys[(size_t)idx[(size_t)i]].score;
    }
    return out;
}

int main()
{
    std::mt19937 rng(20260916);

    // 1. the generator: our reading of the engine's minstd_rand (seeded at
    //    0xabe03d, advanced at 0xabe0f3) is MSVC's std::minstd_rand, which is
    //    what the game -- an MSVC build -- compiled that code from.
    int draws = 0;
    for (uint32_t seed : {0u, 1u, 2u, 7u, 42u, 1234567u, 2147483646u, 2147483647u,
                          2147483648u, 4000000000u, 0xffffffffu}) {
        std::minstd_rand g(seed);
        uint32_t s = TrainOrderSeedFix(seed);
        for (int i = 0; i < 500; i++) { assert(TrainOrderNext(&s) == (uint32_t)g()); draws++; }
    }
    printf("PASS generator: %d draws match MSVC std::minstd_rand across 11 seeds\n", draws);

    // 2. the property: the order depends on the keys and the seed, not on the
    //    order the engine happened to register the trains in.
    int checked = 0, controlDiffs = 0;
    for (int t = 0; t < 300; t++) {
        const int n = 2 + (int)(rng() % 64);
        std::vector<Train> reg;
        for (int i = 0; i < n; i++) {
            char nm[32];
            // names as a player leaves them: duplicates, mixed case, some blank
            snprintf(nm, sizeof(nm), "%s %d", (rng() % 2) ? "Coal" : "ore", (int)(rng() % 8));
            reg.push_back({ (rng() % 7 == 0) ? std::string() : std::string(nm),
                            (int32_t)(1000 + rng() % 100000) });
        }
        std::sort(reg.begin(), reg.end(), [](const Train& a, const Train& b) { return a.id < b.id; });
        reg.erase(std::unique(reg.begin(), reg.end(),
                              [](const Train& a, const Train& b) { return a.id == b.id; }), reg.end());
        const uint32_t seed = rng();
        std::vector<Train> other = reg;
        std::shuffle(other.begin(), other.end(), rng);        // the other peer's node list
        assert(Order(reg, seed) == Order(other, seed));
        checked++;
        // ...and the seed really is an input: a different seed usually reorders.
        if (Order(reg, seed) != Order(reg, seed + 1)) controlDiffs++;
    }
    printf("PASS independence: %d worlds ordered identically from two registration orders "
           "(and %d/%d change when the seed does)\n", checked, controlDiffs, checked);
    assert(controlDiffs > checked / 2);

    // 3. alphabetical is in charge: with a jitter of at most J-1, a train can
    //    only ever be overtaken by one within J places of it.
    {
        std::vector<Train> reg;
        for (int i = 0; i < 60; i++) {
            char nm[16]; snprintf(nm, sizeof(nm), "train %02d", i);
            reg.push_back({ nm, (int32_t)(500 + i) });
        }
        const int64_t J = 60 / TRAIN_ORDER_JITTER_DIV;     // 20
        int maxMove = 0;
        for (uint32_t seed = 1; seed <= 200; seed++) {
            std::vector<Train> shuffled = reg;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            std::vector<int32_t> got = Order(shuffled, seed);
            for (size_t pos = 0; pos < got.size(); pos++) {
                const int rank = got[pos] - 500;           // names sort in id order here
                maxMove = (std::max)(maxMove, std::abs((int)pos - rank));
            }
        }
        printf("PASS bounded: over 200 seeds no train moved more than %d places from its "
               "alphabetical rank (jitter window J=%lld)\n", maxMove, (long long)J);
        assert(maxMove > 0);                                // the jitter does something
        assert(maxMove < (int)J * 2);                       // and not too much
    }

    // 4. the anti-starvation property the jitter exists for: the last-ranked
    //    train is not condemned to go last for ever, and the first-ranked one
    //    does not always win.
    {
        std::vector<Train> reg;
        for (int i = 0; i < 9; i++) {
            char nm[16]; snprintf(nm, sizeof(nm), "t%d", i);
            reg.push_back({ nm, (int32_t)(10 + i) });
        }
        int lastMovedUp = 0, firstLostIt = 0;
        for (uint32_t seed = 1; seed <= 400; seed++) {
            std::vector<int32_t> got = Order(reg, seed);
            if (got.back() != 18) lastMovedUp++;            // "t8" not last
            if (got.front() != 10) firstLostIt++;           // "t0" not first
        }
        printf("PASS fairness: over 400 seeds the last-ranked train avoided last place %d "
               "times and the first-ranked one lost the front %d times\n", lastMovedUp, firstLostIt);
        assert(lastMovedUp > 40 && firstLostIt > 40);
    }

    // 5. the score bound the rule promises: rank 0 can never outscore
    //    rank n-1 + J - 1, and every score is within [rank, rank + J - 1].
    {
        std::vector<Train> reg;
        for (int i = 0; i < 30; i++) {
            char nm[16]; snprintf(nm, sizeof(nm), "n%02d", i);
            reg.push_back({ nm, (int32_t)(200 + i) });
        }
        const int64_t J = 30 / TRAIN_ORDER_JITTER_DIV;
        for (uint32_t seed = 1; seed <= 200; seed++) {
            std::vector<int32_t> scores;
            std::vector<int32_t> got = Order(reg, seed, nullptr, &scores);
            int minScore = 1 << 30, maxScore = -1;
            for (size_t i = 0; i < got.size(); i++) {
                const int rank = got[i] - 200;
                assert(scores[i] >= rank && scores[i] <= rank + (int)J - 1);
                minScore = (std::min)(minScore, scores[i]);
                maxScore = (std::max)(maxScore, scores[i]);
            }
            assert(minScore <= (int)J - 1);                  // rank 0's ceiling
            assert(maxScore <= 29 + (int)J - 1);             // rank n-1's ceiling
            for (size_t i = 1; i < scores.size(); i++) assert(scores[i - 1] <= scores[i]);
        }
        printf("PASS scores: every train scored within [rank, rank+J-1] and the result is "
               "sorted by score, over 200 seeds\n");
    }

    // 6. all names equal (or all missing) reduces to a seeded permutation of the
    //    ids: still deterministic, still registration-order independent, and not
    //    simply id order.
    for (int blank = 0; blank < 2; blank++) {
        std::vector<Train> reg;
        for (int i = 0; i < 40; i++)
            reg.push_back({ blank ? std::string() : std::string("Express"), (int32_t)(1 + i * 3) });
        std::vector<int32_t> ids;
        for (const Train& t : reg) ids.push_back(t.id);
        int permuted = 0;
        for (uint32_t seed = 1; seed <= 100; seed++) {
            std::vector<Train> other = reg;
            std::shuffle(other.begin(), other.end(), rng);
            std::vector<int32_t> a = Order(reg, seed), b = Order(other, seed);
            assert(a == b);
            std::vector<int32_t> sorted = a;
            std::sort(sorted.begin(), sorted.end());
            assert(sorted == ids);                           // a permutation, nothing lost
            if (a != ids) permuted++;
        }
        printf("PASS equal names (%s): 100 seeds give a stable permutation of the ids, %d of "
               "them not id order\n", blank ? "all blank" : "all \"Express\"", permuted);
        assert(permuted > 50);
    }

    // 7. the comparison itself: case-insensitive, empty first, ties by id.
    {
        std::vector<Train> reg = {
            { "beta", 5 }, { "ALPHA", 9 }, { "", 7 }, { "alpha", 3 }, { "Beta", 1 },
        };
        // With J >= 2 the jitter can reorder, so check the RANK directly by
        // giving every train the same jitter draw: seed 0 -> 1, and any seed
        // gives the same rank before jitter. Compare the name rule alone.
        TrainOrderKey k[5];
        const char* names[5] = { "beta", "ALPHA", "", "alpha", "Beta" };
        const int32_t ids[5] = { 5, 9, 7, 3, 1 };
        for (int i = 0; i < 5; i++) {
            k[i].name = names[i][0] ? names[i] : nullptr;
            k[i].len = (uint32_t)strlen(names[i]);
            k[i].id = ids[i]; k[i].score = 0;
        }
        // "" < alpha(3) < ALPHA(9) < Beta(1) < beta(5)
        assert(TrainOrderNameCmp(k[2], k[0]) < 0);           // empty sorts first
        assert(TrainOrderNameCmp(k[1], k[3]) == 0);          // ALPHA == alpha
        assert(TrainOrderNameCmp(k[3], k[0]) < 0);           // alpha < beta
        assert(TrainOrderNameCmp(k[4], k[0]) == 0);          // Beta == beta
        assert(TrainOrderNameCmp(k[0], k[1]) > 0);
        // a prefix sorts before the longer name
        TrainOrderKey shortK = { "coal", 4, 1, 0 }, longK = { "coal 2", 6, 2, 0 };
        assert(TrainOrderNameCmp(shortK, longK) < 0);
        printf("PASS compare: case folded on ASCII only, empty first, prefix before the "
               "longer name\n");
    }

    // 8. refusals leave the array exactly as the engine built it.
    {
        TrainOrderKey k[3] = { { "a", 1, 5, 0 }, { "b", 1, 4, 0 }, { "c", 1, 3, 0 } };
        int32_t idx[3] = { 0, 1, 2 };
        TrainOrderOutcome o = TrainOrderArrange(idx, -1, k, 1);
        assert(o.refused && idx[0] == 0 && idx[1] == 1 && idx[2] == 2);
        o = TrainOrderArrange(idx, TRAINORDER_MAX_N + 1, k, 1);
        assert(o.refused);
        o = TrainOrderArrange(idx, 3, nullptr, 1);
        assert(o.refused && idx[0] == 0 && idx[1] == 1 && idx[2] == 2);
        int32_t notIota[3] = { 2, 0, 1 };
        o = TrainOrderArrange(notIota, 3, k, 1);
        assert(o.refused && notIota[0] == 2 && notIota[1] == 0 && notIota[2] == 1);
        TrainOrderKey bad[2] = { { nullptr, 4, 1, 0 }, { "x", 1, 2, 0 } };
        int32_t idx2[2] = { 0, 1 };
        o = TrainOrderArrange(idx2, 2, bad, 1);
        assert(o.refused && idx2[0] == 0 && idx2[1] == 1);
        o = TrainOrderArrange(idx, 0, k, 1);
        assert(!o.refused && !o.changed);
        o = TrainOrderArrange(idx, 1, k, 1);
        assert(!o.refused && !o.changed);
        printf("PASS refusals: a bad count, no keys, a non-iota array and a length with no "
               "text all leave the order untouched\n");
    }

    // 9. two records the rule cannot separate -- same name AND same id -- are
    //    reported, because which of them goes first is then down to the sort.
    //    The same id under different names is decided by the name and is fine.
    {
        TrainOrderKey same[3] = { { "a", 1, 7, 0 }, { "b", 1, 3, 0 }, { "a", 1, 7, 0 } };
        int32_t idx[3] = { 0, 1, 2 };
        TrainOrderOutcome o = TrainOrderArrange(idx, 3, same, 12345);
        assert(!o.refused && o.duplicates && o.named == 3);
        TrainOrderKey split[3] = { { "a", 1, 7, 0 }, { "b", 1, 3, 0 }, { "c", 1, 7, 0 } };
        int32_t idx2[3] = { 0, 1, 2 };
        o = TrainOrderArrange(idx2, 3, split, 12345);
        assert(!o.refused && !o.duplicates);
        printf("PASS duplicates: an indistinguishable pair is flagged, a shared id under "
               "different names is not\n");
    }

    // 10. scale: 4000 trains, still registration-order independent.
    {
        std::vector<Train> reg;
        for (int i = 0; i < 4000; i++) {
            char nm[24]; snprintf(nm, sizeof(nm), "Line %d train %d", i % 37, i);
            reg.push_back({ nm, (int32_t)(500000 - i * 3) });
        }
        std::vector<Train> other = reg;
        std::shuffle(other.begin(), other.end(), rng);
        TrainOrderOutcome o{};
        assert(Order(reg, 123456789u, &o) == Order(other, 123456789u));
        assert(!o.refused && o.named == 4000 && !o.duplicates);
        printf("PASS scale: 4000 named trains ordered identically from two registration orders\n");
    }

    // 11. what it costs. This runs on the sim thread inside the engine's own
    //     update, once a step, so the number matters: slice_hook.cpp logs a
    //     SLOW line past 1 ms. Measured here for the ordering alone -- the
    //     name LOOKUPS (one component-list scan per train) are on top, and
    //     the dll times the two together for real.
    for (int64_t n : {(int64_t)100, (int64_t)500, (int64_t)2000}) {
        std::vector<Train> reg;
        for (int64_t i = 0; i < n; i++) {
            char nm[24]; snprintf(nm, sizeof(nm), "Line %d train %d", (int)(i % 37), (int)i);
            reg.push_back({ nm, (int32_t)(100000 - (int)i * 3) });
        }
        std::vector<TrainOrderKey> keys((size_t)n);
        std::vector<int32_t> idx((size_t)n);
        const int reps = 200;
        const std::clock_t t0 = std::clock();
        for (int r = 0; r < reps; r++) {
            for (int64_t i = 0; i < n; i++) {
                keys[(size_t)i].name = reg[(size_t)i].name.c_str();
                keys[(size_t)i].len = (uint32_t)reg[(size_t)i].name.size();
                keys[(size_t)i].id = reg[(size_t)i].id;
                keys[(size_t)i].score = 0;
                idx[(size_t)i] = (int32_t)i;
            }
            TrainOrderArrange(idx.data(), n, keys.data(), (uint32_t)(r + 1));
        }
        const double us = (double)(std::clock() - t0) / CLOCKS_PER_SEC * 1e6 / reps;
        printf("COST %4lld trains: %.0f us per sim step (ordering only)\n", (long long)n, us);
        assert(us < 100000.0);          // a loose sanity bound, not a budget
    }

    puts("ALL PASS trainorder");
    return 0;
}
