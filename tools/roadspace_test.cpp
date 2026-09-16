// Offline test for the road free-space patch (native/src/roadspace.h, installed
// by slice_hook.cpp "ROAD FREE SPACE"). No game needed.
//
//   cl /nologo /std:c++17 /EHsc /MT /W3 /O2 tools\roadspace_test.cpp /Fe:roadspace_test.exe
//   roadspace_test.exe
//
// tools/roadspace_bytes_test.py is the other half: it checks the two hook
// sites, the helpers and the loop against the shipped exe. This one checks the
// arithmetic:
//
//   - the same multiset of terms, in any order, gives the same float
//   - the engine's own single-precision sum does NOT (a real case is shown)
//   - the two clipping helpers match the engine's comiss/maxss sequences
//   - the cap: terms past ROADSPACE_MAX_TERMS are flagged and still summed
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

static bool Readable(const void* p, size_t n) { return p != nullptr || n == 0; }
static void Log(const char*, ...) {}
#include "../native/src/roadspace.h"

static float Ordered(const std::vector<float>& t, RoadSpaceAcc* acc = nullptr)
{
    RoadSpaceAcc local;
    RoadSpaceAcc* a = acc ? acc : &local;
    RoadSpaceBegin(a);
    for (float v : t) RoadSpaceAdd(a, v);
    return RoadSpaceResult(a);
}

static bool SameBits(float a, float b) { return memcmp(&a, &b, sizeof(float)) == 0; }

// A footprint the way the engine makes one: a vehicle somewhere on an edge of
// this length, clipped, in whichever direction it faces.
static float Footprint(std::mt19937& rng, float length)
{
    std::uniform_real_distribution<float> pos(-5.0f, length + 5.0f);
    std::uniform_real_distribution<float> half(2.0f, 12.0f);
    const float p = pos(rng), back = -half(rng), front = half(rng);
    if (rng() & 1) return RoadSpaceTermForward(p, p, back, front, length);
    return RoadSpaceTermBackward(p, p, back, front);
}

int main()
{
    std::mt19937 rng(20260916);

    // 1. order independence: one bag of terms, 1000 shuffles, one float.
    {
        int bags = 0, shuffles = 0;
        for (int b = 0; b < 40; b++) {
            const int n = 2 + (int)(rng() % 63);
            std::uniform_real_distribution<float> len(30.0f, 400.0f);
            const float length = len(rng);
            std::vector<float> t((size_t)n);
            for (float& v : t) v = Footprint(rng, length);
            const float ref = Ordered(t);
            for (int s = 0; s < 1000; s++) {
                std::shuffle(t.begin(), t.end(), rng);
                const float got = Ordered(t);
                if (!SameBits(ref, got)) {
                    printf("FAIL order: bag %d shuffle %d: %.9g != %.9g\n", b, s, (double)got, (double)ref);
                    return 1;
                }
                shuffles++;
            }
            bags++;
        }
        printf("PASS order: %d bags of 2..64 footprints, %d shuffles, one float each\n", bags, shuffles);
    }

    // 2. the engine's sum is order-dependent and ours is not, on the same data.
    //    First the textbook case, then a search over realistic footprints for
    //    a bag whose engine sums disagree between two orders.
    {
        std::vector<float> a = { 1e8f, 1.0f, -1e8f };
        std::vector<float> b = { 1e8f, -1e8f, 1.0f };
        const float ea = RoadSpaceEngineSum(a.data(), 3), eb = RoadSpaceEngineSum(b.data(), 3);
        assert(!SameBits(ea, eb));                       // 0 vs 1
        assert(SameBits(Ordered(a), Ordered(b)));
        assert(Ordered(a) == 1.0f);

        int found = 0, tried = 0;
        double maxDelta = 0.0;
        int exampleN = 0; float ex1 = 0, ex2 = 0, exo = 0;
        for (int b2 = 0; b2 < 20000 && found < 50; b2++) {
            const int n = 3 + (int)(rng() % 6);
            std::vector<float> t((size_t)n);
            for (float& v : t) v = Footprint(rng, 120.0f);
            std::vector<float> u = t;
            std::shuffle(u.begin(), u.end(), rng);
            tried++;
            const float e1 = RoadSpaceEngineSum(t.data(), n), e2 = RoadSpaceEngineSum(u.data(), n);
            if (SameBits(e1, e2)) continue;
            found++;
            const float o1 = Ordered(t), o2 = Ordered(u);
            if (!SameBits(o1, o2)) { printf("FAIL: ordered sums differ where the engine's do\n"); return 1; }
            const double d = fabs((double)e1 - (double)e2);
            if (d > maxDelta) { maxDelta = d; exampleN = n; ex1 = e1; ex2 = e2; exo = o1; }
        }
        assert(found >= 10);
        printf("PASS divergence: %d of %d bags of 3..8 real footprints summed to different floats "
               "in two orders the engine's way (largest gap %.3g: %.9g vs %.9g at n=%d); the "
               "ordered sum was %.9g both times, every time\n",
               found, tried, maxDelta, (double)ex1, (double)ex2, exampleN, (double)exo);
    }

    // 3. the clipping helpers against hand-computed values.
    {
        // forward: hi = min(pos + front, length), lo = pos + back
        assert(RoadSpaceTermForward(10.0f, 10.0f, -2.0f, 3.0f, 100.0f) == 5.0f);
        assert(RoadSpaceTermForward(99.0f, 99.0f, -2.0f, 3.0f, 100.0f) == 3.0f);   // clipped at 100
        assert(RoadSpaceTermForward(0.0f, 0.0f, -2.0f, 3.0f, 100.0f) == 5.0f);     // lo may go negative
        assert(RoadSpaceTermForward(-4.0f, -4.0f, -2.0f, 3.0f, 100.0f) == 5.0f);
        // the two GetPos results are separate arguments, as in the engine
        assert(RoadSpaceTermForward(10.0f, 11.0f, -2.0f, 3.0f, 100.0f) == 4.0f);
        // backward: hi = pos - back, lo = max(0, pos - front)
        assert(RoadSpaceTermBackward(10.0f, 10.0f, -2.0f, 3.0f) == 5.0f);
        assert(RoadSpaceTermBackward(1.0f, 1.0f, -2.0f, 3.0f) == 3.0f);            // clipped at 0
        assert(RoadSpaceTermBackward(-1.0f, -1.0f, -2.0f, 3.0f) == 1.0f);
        assert(RoadSpaceTermBackward(10.0f, 12.0f, -2.0f, 3.0f) == 7.0f);
        // NaN survives the way comiss/ja and maxss leave it
        const float nan = std::nanf("");
        assert(std::isnan(RoadSpaceTermForward(nan, 10.0f, -2.0f, 3.0f, 100.0f)));
        assert(std::isnan(RoadSpaceTermBackward(nan, 10.0f, -2.0f, 3.0f)));
        // +inf as a length clips nothing
        assert(RoadSpaceTermForward(1e5f, 1e5f, -2.0f, 3.0f, INFINITY) == 5.0f);
        printf("PASS clipping: forward and backward helpers match the hand-computed values, "
               "clip at the edge ends, and let a NaN through\n");
    }

    // 4. the accumulator: asEngine is bit-for-bit the engine's own sum, and n
    //    terms past the cap are flagged, spilled in arrival order, and still
    //    counted.
    {
        std::vector<float> t(600);
        for (float& v : t) v = Footprint(rng, 250.0f);
        RoadSpaceAcc acc;
        const float out = Ordered(t, &acc);
        assert(acc.total == 600 && acc.n == ROADSPACE_MAX_TERMS);
        assert(RoadSpaceOverflowed(&acc));
        assert(SameBits(acc.asEngine, RoadSpaceEngineSum(t.data(), 600)));
        double spill = 0.0;
        for (int i = ROADSPACE_MAX_TERMS; i < 600; i++) spill += (double)t[i];
        assert(acc.spill == spill);
        std::vector<float> head(t.begin(), t.begin() + ROADSPACE_MAX_TERMS);
        std::sort(head.begin(), head.end());
        double s = 0.0;
        for (float v : head) s += (double)v;
        assert(SameBits(out, (float)(s + spill)));
        for (int i = 1; i < acc.n; i++) assert(acc.terms[i - 1] <= acc.terms[i]);

        RoadSpaceAcc small;
        std::vector<float> few = { 3.0f, 1.0f, 2.0f };
        Ordered(few, &small);
        assert(!RoadSpaceOverflowed(&small) && small.total == 3 && small.n == 3 && small.spill == 0.0);
        assert(SameBits(small.asEngine, RoadSpaceEngineSum(few.data(), 3)));

        RoadSpaceAcc none;
        RoadSpaceBegin(&none);
        assert(RoadSpaceResult(&none) == 0.0f && !std::signbit(RoadSpaceResult(&none)));
        printf("PASS cap: 600 terms -> %d sorted + 88 spilled in arrival order, flagged; asEngine "
               "is the engine's own sum bit for bit; an empty edge is +0\n", ROADSPACE_MAX_TERMS);
    }

    // 5. a NaN term cannot make the sort walk off the array.
    {
        std::vector<float> t(64);
        for (float& v : t) v = Footprint(rng, 100.0f);
        for (int k = 0; k < 64; k += 7) t[(size_t)k] = std::nanf("");
        RoadSpaceAcc acc;
        const float out = Ordered(t, &acc);
        assert(std::isnan(out) && acc.n == 64);
        printf("PASS nan: 10 NaN terms among 64 sort within bounds and the result is NaN\n");
    }

    // 6. cost of the summation alone, /O2.
    for (int n : { 8, 64 }) {
        std::vector<float> t((size_t)n);
        for (float& v : t) v = Footprint(rng, 200.0f);
        const int reps = 200000;
        volatile float sink = 0.0f;
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; r++) sink = Ordered(t);
        auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
        printf("COST %3d terms: %.0f ns per GetUsedSpace (sort + double sum only)\n", n, ns);
    }

    printf("ALL PASS roadspace\n");
    return 0;
}
