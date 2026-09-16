// Included by slice_hook.cpp after trainorder.h, and by tools/moveorder_test.cpp
// with Readable/Log stubbed. Build 35924. See slice_hook.cpp "SHIP AND AIRCRAFT
// CLAIM ORDER" for what this is for; this header holds the part that can be
// tested without a game.
//
// WHAT IS DIFFERENT FROM TRAINS. TrainMoveSystem::Update2 builds its own iota
// array and shuffles it, so the train patch reorders a scratch array the engine
// made a moment earlier and nothing else can see. ShipMoveSystem::Update2
// (0xa6c1e0) and AircraftMoveSystem::Update2 (0xa2bc60) have no such array: they
// walk the ECS family's node vector directly -- ComponentGroupFamily4<Ship,
// MovePathAircraft, ModelInstanceList, BoundingVolume> and its Aircraft twin,
// 20-byte records:
//
//     +0   int32  entity id
//     +4   int32  index into the Ship/Aircraft component array
//     +8   int32  index into the MovePathAircraft array
//     +0xc int32  index into the ModelInstanceList array
//     +0x10 int32 index into the BoundingVolume array
//
// That vector is engine-owned and lives across frames, so this header does NOT
// reorder anything. It only works out what the name order WOULD be and hands
// the caller two hashes to put in the log. See slice_hook.cpp for why.
//
// The ordering rule itself is the train rule minus the jitter: rank by NAME
// (ASCII-case-insensitive, byte-wise -- TrainOrderNameCmp), ties by entity id.
// No jitter, because jitter exists to stop a fixed priority starving somebody at
// a junction, and nothing here is being enforced: a measurement wants the plain,
// reproducible rank.
#ifndef TPF2MP_MOVEORDER_H
#define TPF2MP_MOVEORDER_H

// Requires trainorder.h for TrainOrderKey, TrainOrderNameCmp and
// TrainOrderHeapSort (same reasons apply: the names point into live engine
// memory, so the sort has to be one that cannot run off the end if a key moves
// underneath it).
#ifndef TPF2MP_TRAINORDER_H
#error "moveorder.h needs trainorder.h first"
#endif

static const int     MOVEORDER_REC = 20;            // bytes per family node record
static const int64_t MOVEORDER_MAX_N = 1 << 20;     // sanity bound on the vehicle count

struct MoveOrderOutcome {
    const char* refused;   // why nothing was computed (nullptr = ranked)
    bool changed;          // the engine's node order is NOT already name order
    bool duplicates;       // two records the rule cannot tell apart (same name, same id)
    int64_t named;         // how many had a name to sort on
};

static inline int32_t MoveOrderRecId(const uint8_t* recs, int32_t pos)
{
    int32_t v;
    memcpy(&v, recs + (size_t)pos * MOVEORDER_REC, sizeof(v));
    return v;
}

// Put idx[0..n-1] in name order. On entry idx must still be the iota the caller
// built, i.e. the engine's own node order, so "changed" means exactly "the
// engine is not already claiming in name order". Refuses -- and leaves idx
// alone -- on anything it does not recognise.
static MoveOrderOutcome MoveOrderRank(int32_t* idx, int64_t n, TrainOrderKey* keys)
{
    MoveOrderOutcome out = { nullptr, false, false, 0 };
    if (!idx || n < 0 || n > MOVEORDER_MAX_N) { out.refused = "bad count"; return out; }
    if (n < 2) return out;
    if (!keys) { out.refused = "no keys"; return out; }
    for (int64_t i = 0; i < n; i++)
        if (idx[i] != (int32_t)i) { out.refused = "not iota"; return out; }
    for (int64_t i = 0; i < n; i++)
        if (keys[i].len && !keys[i].name) { out.refused = "key without text"; return out; }
    for (int64_t i = 0; i < n; i++) if (keys[i].len) out.named++;

    TrainOrderHeapSort(idx, n, [&](int32_t a, int32_t b) {
        const int c = TrainOrderNameCmp(keys[a], keys[b]);
        if (c) return c < 0;
        return keys[a].id < keys[b].id;
    });
    for (int64_t i = 1; i < n; i++)
        if (keys[idx[i - 1]].id == keys[idx[i]].id &&
            TrainOrderNameCmp(keys[idx[i - 1]], keys[idx[i]]) == 0) out.duplicates = true;
    for (int64_t i = 0; i < n; i++)
        if (idx[i] != (int32_t)i) { out.changed = true; break; }
    return out;
}

// FNV-1a over the NAMES, folded to lower ASCII exactly the way the comparison
// folds them, each one length-prefixed so "ab","c" cannot hash as "a","bc".
//
// This is the value two peers compare. Entity ids cannot be compared across
// peers -- replication ships positions, not ids, and each peer's allocator runs
// ahead on its own town growth (see slice_hook.cpp, "WHAT THIS ASSUMES") -- but
// names ARE replicated, so an equal hash over the same n means the two engines
// are holding the same ships in the same order, and an unequal one means they
// are not and the first to reach a lock wins it differently on each machine.
static uint32_t MoveOrderNameHash(const int32_t* idx, int64_t n, const TrainOrderKey* keys)
{
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < n; i++) {
        const TrainOrderKey& k = keys[idx[i]];
        uint32_t len = k.len;
        for (int b = 0; b < 4; b++) { h ^= (len >> (b * 8)) & 0xffu; h *= 16777619u; }
        for (uint32_t j = 0; j < k.len; j++) {
            unsigned char c = (unsigned char)k.name[j];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
            h ^= c; h *= 16777619u;
        }
    }
    return h;
}

// FNV-1a over the entity ids in the given order. Not comparable between peers;
// it is here so one peer's own log shows when its node vector changed shape
// between two steps that report the same n.
static uint32_t MoveOrderIdHash(const int32_t* idx, int64_t n, const TrainOrderKey* keys)
{
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < n; i++) {
        const uint32_t k = (uint32_t)keys[idx[i]].id;
        for (int b = 0; b < 4; b++) { h ^= (k >> (b * 8)) & 0xffu; h *= 16777619u; }
    }
    return h;
}

#endif
