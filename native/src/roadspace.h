// Included by slice_hook.cpp after Readable/Log, and by tools/roadspace_test.cpp
// with those stubbed. Build 35924. See slice_hook.cpp "ROAD FREE SPACE" for what
// this is for and where it is hooked; this header holds the part that can be
// tested without a game: turn a bag of per-vehicle footprints into one float
// that does not depend on the order they were handed over in.
//
// THE FINDING. transport::EdgeUseManager::GetUsedSpace walks the `entries`
// vector of one edge, clips each vehicle's footprint to the edge, and
// accumulates the clipped lengths in SINGLE precision, in vector order
// (0x142117448 subss xmm1,xmm0; 0x142117450 addss xmm1,xmm7). `entries` order is
// the order the ECS registered those vehicles in -- which two peers with
// identical worlds can disagree about, because registration order is thread and
// load-order dependent and the mod's world hash is geometric. Float + is not
// associative, so the same multiset in a different order can come out 1 ULP
// apart, and the consumer is a bare compare:
//
//     0x142214bcd  call GetUsedSpace ; subss xmm6,xmm0     (space left on edge)
//     0x142214e76  comiss xmm12,xmm6 ; jbe                 -> BrakePoint STOP
//
// One ULP on the wrong side of that `jbe` and one peer's bus enters the junction
// while the other's waits. No command, no divergence the hash can see, and from
// then on two different worlds.
//
// THE RULE. Same algorithm, same clipping, same rounding per term -- then:
//
//   1. collect the per-entry terms in `entries` order (so every GetPos call the
//      engine makes, and every intermediate rounding, still happens exactly when
//      and as it did),
//   2. sort the collected terms ascending,
//   3. accumulate in DOUBLE, ascending, and round once to float at the end.
//
// WHY SORT AND NOT JUST WIDEN. Widening the accumulator to double is what the
// obvious fix does, and it is nearly enough: the drift it leaves over n terms is
// about n * 2^-53 * (largest partial sum), some eight orders of magnitude below
// one float ULP of the result, so the final round-to-float almost always lands
// on the same float whatever the order. Almost. "Almost" is exactly the word
// that got us here -- the bug being fixed is a one-in-a-million rounding that
// happens a million times an hour. Sorting first makes the summation order a
// function of the MULTISET alone, so the result is provably identical on both
// peers instead of overwhelmingly likely to be. Equal terms may swap places in
// the sort; adding equal values in a different order gives the same double, so
// that is not a hole. (Ascending is also the accurate order, which is free.)
//
// The sort is over the vehicles standing on ONE edge -- single digits in a
// normal game -- so an insertion sort is both the fastest thing available and
// the only one that cannot walk off the end of the array if a term is a NaN.
// Past ROADSPACE_MAX_TERMS the terms are added in arrival order into a separate
// double and the result is flagged: order-dependent again, but an edge with 512
// vehicles on it is not a road, and the caller says so in the log rather than
// pretending.
//
// WHAT IT COSTS. tools/roadspace_test.cpp measures the summation alone, /O2:
// 16 ns at 8 terms and 450 ns at 64 -- against two calls into the engine's
// own GetPos per surviving entry (three in the filtered overload), which is
// what actually dominates. The same test finds that one bag in five of 3..8
// real footprints already sums to a different float in two orders the
// engine's way.
#ifndef TPF2MP_ROADSPACE_H
#define TPF2MP_ROADSPACE_H

// The terms are summed in a fixed order, so no compiler is allowed to
// reassociate them back into the problem. /fp:precise (the default, and what
// native\build.bat compiles with) already forbids that; this makes it explicit
// and survives someone adding /fp:fast to the build later.
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif

// One EdgeUseManager entry. Measured at 0x1421173b0-0x142117448 and against
// EdgeUseManager::Add (0x2115f80), which push_backs one of these per vehicle.
struct RoadUseEntry {
    int32_t entity;      // +0x00  the vehicle
    int32_t comp;        // +0x04  its component index, the argument to GetPos
    float   boundsBack;  // +0x08
    float   boundsFront; // +0x0c  Add asserts boundsFront >= boundsBack
    uint8_t forward;     // +0x10  which way along the edge it faces
    uint8_t pad[3];
};
static_assert(sizeof(RoadUseEntry) == 0x14, "EdgeUseManager entry is 20 bytes");

// Terms past this are added in arrival order and the result is flagged.
static const int ROADSPACE_MAX_TERMS = 512;

// One entry's contribution, forward. Two GetPos results are passed in because
// the engine calls GetPos twice per entry and we keep it that way -- the values
// are equal (GetPos is a pure read), but "equal" is a thing to be handed, not
// assumed. From 0x1421173b0:
//     hi = min(pos + boundsFront, length)      via comiss/ja, so a NaN hi_raw
//     lo = pos + boundsBack                     survives as the result
static inline float RoadSpaceTermForward(float pos1, float pos2, float boundsBack,
                                         float boundsFront, float length)
{
    const float hiRaw = pos1 + boundsFront;
    const float lo = pos2 + boundsBack;
    const float hi = (hiRaw > length) ? length : hiRaw;
    return hi - lo;
}

// ...and backward, from 0x14211740d:
//     hi = pos - boundsBack
//     lo = max(0, pos - boundsFront)            via `xorps xmm0; maxss xmm0,v`,
//                                               i.e. (0 > v) ? 0 : v
static inline float RoadSpaceTermBackward(float pos1, float pos2, float boundsBack,
                                          float boundsFront)
{
    const float loRaw = pos1 - boundsFront;
    const float hi = pos2 - boundsBack;
    const float lo = (0.0f > loRaw) ? 0.0f : loRaw;
    return hi - lo;
}

// Ascending, by value. Insertion sort: n is the vehicles on one edge, and it
// indexes only within [0,i] under an explicit bound, so a NaN term (which makes
// every compare false) leaves the array in SOME order rather than reading past
// the end the way a partitioning sort can.
static inline void RoadSpaceSortTerms(float* t, int n)
{
    for (int i = 1; i < n; i++) {
        const float v = t[i];
        int j = i - 1;
        while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; j--; }
        t[j + 1] = v;
    }
}

// The accumulator the hook fills. Deliberately a plain struct with a fixed
// buffer: it lives on the stack of a function the engine calls thousands of
// times a step, so it must not allocate, and 512 floats is 2 KB -- under the
// 4 KB that would make MSVC emit a stack probe.
struct RoadSpaceAcc {
    float  terms[ROADSPACE_MAX_TERMS];
    int    n;          // terms collected
    int    total;      // terms offered, including any past the cap
    double spill;      // the ones past the cap, in arrival order
    float  asEngine;   // the same terms summed the way the engine does it
};

static inline void RoadSpaceBegin(RoadSpaceAcc* a)
{
    a->n = 0; a->total = 0; a->spill = 0.0; a->asEngine = 0.0f;
}

static inline void RoadSpaceAdd(RoadSpaceAcc* a, float term)
{
    a->total++;
    a->asEngine = a->asEngine + term;             // what GetUsedSpace returns today
    if (a->n < ROADSPACE_MAX_TERMS) a->terms[a->n++] = term;
    else a->spill += (double)term;
}

static inline bool RoadSpaceOverflowed(const RoadSpaceAcc* a)
{
    return a->total > ROADSPACE_MAX_TERMS;
}

// Sorts the collected terms and rounds once. After this the accumulator's terms
// are in ascending order, which is all the caller ever wants to do with them.
static inline float RoadSpaceResult(RoadSpaceAcc* a)
{
    RoadSpaceSortTerms(a->terms, a->n);
    double s = 0.0;
    for (int i = 0; i < a->n; i++) s += (double)a->terms[i];
    s += a->spill;
    return (float)s;
}

// The engine's own summation, kept so the hook can tell -- and log -- when it
// has actually changed an answer, and so the test can show a case where the two
// disagree. Single precision, in the order given.
static inline float RoadSpaceEngineSum(const float* t, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) s = s + t[i];
    return s;
}

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

#endif
