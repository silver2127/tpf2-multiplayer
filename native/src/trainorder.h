// Included by slice_hook.cpp after Readable/Log, and by tools/trainorder_test.cpp
// with those two stubbed. Build 35924. See slice_hook.cpp "TRAIN RESERVATION
// ORDER" for what this is for and where it is hooked; this header holds the
// part that can be tested without a game: turn one key per train into the order
// they reserve track in.
//
// THE ARRAY. ecs::TrainMoveSystem::Update2 builds idx[0..n-1] = iota over its
// train node list and then reserves track in that order. Each idx entry is a
// POSITION in a 12-byte record array:
//
//     +0  int32  entity id        <- lockstep-ish (see slice_hook.cpp)
//     +4  int32  index into the Train component array    (x 0x20)
//     +8  int32  index into the MovePath component array (x 0x88)
//
// The positions are the order the engine happened to register the entities in,
// which two peers with identical worlds can disagree about. The caller reads
// one TrainOrderKey per position (entity id + the NAME component's text) and
// hands them here; nothing below ever looks at a position again except to move
// it, so the result is a pure function of the keys and the seed.
//
// THE ORDER.
//   1. rank r = the train's place when sorted by name (ASCII-case-insensitive,
//      byte-wise, no locale) then by entity id. Missing and empty names sort
//      first, ties broken by id, so the rank is total and peer-independent.
//   2. jitter j = the next draw of a minstd_rand seeded exactly as the engine
//      seeds its own (GameTime+0x30), taken in rank order, modulo
//      J = max(2, n / TRAIN_ORDER_JITTER_DIV).
//   3. the trains reserve in order of (r + j), ties by entity id.
//
// WHY NOT STRICT ALPHABETICAL. A fixed priority order is deterministic but
// cruel: at a junction two trains contest every step, the alphabetically first
// one wins every step, and the other can be starved for as long as the traffic
// lasts -- the engine shuffles precisely to avoid that. Adding a bounded random
// offset keeps the intent (a train named early generally goes first) while
// letting anyone through sometimes: with J = n/3 a train can overtake about a
// third of the field, so a queue resolves in a few steps instead of never. Make
// J too small and it is strict priority again; too large and it is the engine's
// uniform shuffle with extra steps.
//
// HEAPSORT, NOT std::sort. Keys are read out of live engine memory (the name
// pointers are into the game's own std::strings). std::sort's introsort trusts
// its comparator: a key that moved under it mid-sort can walk the partition
// pointer past the end of the array. Heapsort only ever indexes 2i+1/2i+2 under
// an explicit bound, so the worst an inconsistent comparator can do is produce
// a badly ordered array.
//
// WHAT IT COSTS. Both sorts, /O2, measured by tools/trainorder_test.cpp:
// 15 us at 100 trains, 85 us at 500, 510 us at 2000 -- per sim step, on the sim
// thread, before the caller's name lookups. slice_hook.cpp times the two
// together and logs a SLOW line the first time they pass 1 ms, so a world big
// enough to matter says so instead of quietly costing everyone a millisecond a
// step. (Should that day come, the name compares are the cost: a precomputed
// sort key per train would pay for itself.)

#ifndef TPF2MP_TRAINORDER_H
#define TPF2MP_TRAINORDER_H

static const int TRAINORDER_REC = 12;              // bytes per ECS node-list record
static const int64_t TRAINORDER_MAX_N = 1 << 20;   // sanity bound on the train count
// n / this = how far a train can jump the queue. 3 keeps the alphabet in charge
// (a train stays within a third of the field of its rank) while making
// starvation at a busy junction impossible; see "WHY NOT STRICT ALPHABETICAL".
static const int64_t TRAIN_ORDER_JITTER_DIV = 3;
static const uint32_t TRAINORDER_NAME_MAX = 4096;  // refuse to read a longer name

// One per node-list POSITION. `name` points into the game's own component and
// is only valid for the duration of the call; `len` is 0 for a train with no
// name (or none we could read), which sorts first.
struct TrainOrderKey {
    const char* name;
    uint32_t len;
    int32_t id;        // entity id
    int32_t score;     // filled in by TrainOrderArrange: rank + jitter
};

// What one call did, for the log. `refused` is null when the order was set.
struct TrainOrderOutcome {
    const char* refused;   // why nothing was touched (nullptr = ordered)
    bool changed;          // the result differs from the engine's iota
    bool duplicates;       // two records the rule cannot tell apart: same name
                           // AND same entity id, so which of them goes first is
                           // down to the sort and not to lockstep state
    int64_t named;         // how many trains had a name to sort on
};

static inline int32_t TrainOrderRecId(const uint8_t* recs, int32_t pos)
{
    int32_t v;
    memcpy(&v, recs + (size_t)pos * TRAINORDER_REC, sizeof(v));
    return v;
}

// std::minstd_rand, seeded the way the engine seeds it at 0xabe03d
// (`s %= 2147483647; if (!s) s = 1`) and advanced the way it advances it at
// 0xabe0f3 (`imul 0xbc8f`, mod 0x7fffffff).
static inline uint32_t TrainOrderSeedFix(uint32_t seed)
{
    const uint32_t s = seed % 2147483647u;
    return s ? s : 1u;
}
static inline uint32_t TrainOrderNext(uint32_t* s)
{
    *s = (uint32_t)(((uint64_t)*s * 48271ull) % 2147483647ull);
    return *s;
}

// Case-insensitive ordinal compare of two UTF-8 names: byte-wise, with only
// ASCII A-Z folded. No locale, no collation, no code-page -- every peer must
// get the same answer for the same bytes, and anything that consults a locale
// is a machine setting, not lockstep state. Two names that differ only outside
// ASCII compare by their raw bytes, which is fine: it is still a total order.
static int TrainOrderNameCmp(const TrainOrderKey& a, const TrainOrderKey& b)
{
    const uint32_t n = a.len < b.len ? a.len : b.len;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a.name[i], cb = (unsigned char)b.name[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.len != b.len) return a.len < b.len ? -1 : 1;
    return 0;
}

// idx[] holds positions; `less` compares two positions.
template <class Less>
static void TrainOrderHeapSort(int32_t* idx, int64_t n, Less less)
{
    auto sift = [&](int64_t root, int64_t hi) {
        for (;;) {
            const int64_t child = 2 * root + 1;
            if (child >= hi) return;
            int64_t big = child;
            if (child + 1 < hi && less(idx[child], idx[child + 1])) big = child + 1;
            if (!less(idx[root], idx[big])) return;
            const int32_t t = idx[root]; idx[root] = idx[big]; idx[big] = t;
            root = big;
        }
    };
    for (int64_t i = n / 2 - 1; i >= 0; i--) sift(i, n);
    for (int64_t i = n - 1; i > 0; i--) {
        const int32_t t = idx[0]; idx[0] = idx[i]; idx[i] = t;
        sift(0, i);
    }
}

// Put idx[0..n-1] in reservation order. keys is indexed by POSITION, so
// keys[idx[k]] is the train that reserves k-th once this returns. Refuses (and
// leaves idx untouched) on anything it does not recognise: the engine's own
// order then stands, which is always better than a half-sorted array.
static TrainOrderOutcome TrainOrderArrange(int32_t* idx, int64_t n, TrainOrderKey* keys, uint32_t seed)
{
    TrainOrderOutcome out = { nullptr, false, false, 0 };
    if (!idx || n < 0 || n > TRAINORDER_MAX_N) { out.refused = "bad count"; return out; }
    if (n < 2) return out;                       // nothing to rank
    if (!keys) { out.refused = "no keys"; return out; }

    // The array must still be the iota the engine just built: every position in
    // [0,n) exactly once. Anything else means this is not the code we measured
    // (or someone else got here first), and a key fetch would read out of bounds.
    for (int64_t i = 0; i < n; i++)
        if (idx[i] != (int32_t)i) { out.refused = "not iota"; return out; }
    for (int64_t i = 0; i < n; i++)
        if (keys[i].len && !keys[i].name) { out.refused = "key without text"; return out; }
    for (int64_t i = 0; i < n; i++) if (keys[i].len) out.named++;

    // 1. rank: name, then entity id.
    TrainOrderHeapSort(idx, n, [&](int32_t a, int32_t b) {
        const int c = TrainOrderNameCmp(keys[a], keys[b]);
        if (c) return c < 0;
        return keys[a].id < keys[b].id;
    });
    // After that sort, two entries the rule cannot separate are adjacent: the
    // pair is equal on name and on id. Duplicate ids under DIFFERENT names are
    // not a problem -- the name already decided -- so they are not reported.
    for (int64_t i = 1; i < n; i++)
        if (keys[idx[i - 1]].id == keys[idx[i]].id &&
            TrainOrderNameCmp(keys[idx[i - 1]], keys[idx[i]]) == 0) out.duplicates = true;

    // 2. jitter, drawn in rank order so the draw a train gets depends only on
    //    its rank and the seed. Rejection-free: the draw is taken modulo J,
    //    which biases the low residues by at most one part in 2^31/J -- far
    //    below anything a junction could notice, and identical on every peer,
    //    which is the only property that matters here.
    const int64_t jdiv = n / TRAIN_ORDER_JITTER_DIV;
    const int64_t J = jdiv < 2 ? 2 : jdiv;
    uint32_t s = TrainOrderSeedFix(seed);
    for (int64_t r = 0; r < n; r++)
        keys[idx[r]].score = (int32_t)(r + (int64_t)(TrainOrderNext(&s) % (uint32_t)J));

    // 3. the order they actually reserve in.
    TrainOrderHeapSort(idx, n, [&](int32_t a, int32_t b) {
        if (keys[a].score != keys[b].score) return keys[a].score < keys[b].score;
        return keys[a].id < keys[b].id;
    });

    for (int64_t i = 0; i < n; i++)
        if (idx[i] != (int32_t)i) { out.changed = true; break; }
    return out;
}

// FNV-1a over the entity ids in the order they came out. Two peers that log the
// same value at the same seed are provably about to let the same trains through
// in the same order; a difference is only a hint, because the ids themselves can
// legitimately differ in VALUE between peers (see slice_hook.cpp, "WHAT THIS
// ASSUMES"). Walked only for the lines that are actually logged.
static uint32_t TrainOrderIdHash(const int32_t* idx, int64_t n, const uint8_t* recs)
{
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < n; i++) {
        const uint32_t k = (uint32_t)TrainOrderRecId(recs, idx[i]);
        for (int b = 0; b < 4; b++) { h ^= (k >> (b * 8)) & 0xffu; h *= 16777619u; }
    }
    return h;
}

#endif
