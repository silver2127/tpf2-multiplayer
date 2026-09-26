// town_trace.h -- the town development trace's platform-independent part.
//
// A diagnostic, OFF by default (Windows: `towntrace=1` in tpf2_slice.cfg;
// native Linux: TPF2MP_TOWN_TRACE=1), written so two peers' files can be diffed
// line by line (tools/town_trace_diff.py). Nothing here changes what the game
// decides: it reads the engine's own memory and formats text.
//
// TT -- one line per TownDeveloper::Develop call made by the TownSystem update
//       (Windows 0xab253e -> 0x91d910, native 0x1747917 -> 0x14f5b80):
//         TT t=<GameTime+0x34> town=<entity> i=<index in the Town node list>
//            n=<list length> list=<FNV-1a32 of the list's entity ids, in order>
//            mt0=<FNV-1a64 of the shared mt19937 before> mt1=<... after> e=<engine>
//       The TownSystem develops town i when t % 120 == (i % 30) * 4 and hands
//       every town of that tick ONE mt19937 seeded from t, in list order. So:
//         list differs         -> the towns are staggered/ordered differently
//                                 (node-list order: the 2026-09-23 root cause)
//         mt0 differs, list same -> an earlier town of the tick drew differently
//         mt0 same, mt1 differs  -> THIS town's Develop decided differently
//       The first differing TT line names the town and the tick.
// TF -- every 600 TownSystem iterations, per engine, after the per-iteration
//       family sort: `TF t=<t> e=<engine> lists=<k> <count>:<fnv32> ...`, one
//       token per ECS node list, sorted, so any peer whose lists are not in the
//       same (entity) order shows a different token.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <algorithm>

// boost::random::mt19937: uint32_t x[624]; std::size_t i -- the same layout in
// the MSVC and the GCC build (the TownSystem update keeps it on its stack and
// passes &x[0] to Develop).
static const size_t TOWN_TRACE_MT_BYTES = 624 * 4 + 8;

static inline uint64_t TownTraceFnv64(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

static inline uint32_t TownTraceFnv32(const void* p, size_t n, uint32_t h = 0x811c9dc5u)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193u; }
    return h;
}

// Digest of a node list's entity ids, in list order (node = int32 entity first,
// `stride` bytes apart). The component indices after the entity differ per
// engine and per history, so only the entity is hashed.
static inline uint32_t TownTraceListDigest(const uint8_t* begin, size_t count, size_t stride)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < count; i++) h = TownTraceFnv32(begin + i * stride, 4, h);
    return h;
}

// Position of `entity` in a node list, -1 when absent.
static inline int TownTraceIndexOf(const uint8_t* begin, size_t count, size_t stride, int32_t entity)
{
    for (size_t i = 0; i < count; i++) {
        int32_t e;
        memcpy(&e, begin + i * stride, 4);
        if (e == entity) return (int)i;
    }
    return -1;
}

// Engines by first appearance (each peer runs two, alternating per batch).
struct TownTraceEngines {
    uintptr_t seen[4] = {0, 0, 0, 0};
    int IndexOf(uintptr_t engine)
    {
        for (int k = 0; k < 4; k++) {
            if (seen[k] == engine) return k;
            if (!seen[k]) { seen[k] = engine; return k; }
        }
        return 9;
    }
};

struct TownTraceDevelop {
    int64_t time;          // GameTime+0x34 of the tick, -1 when unknown
    int32_t town;
    int index, count;      // -1 when unknown
    uint32_t list;         // 0 when unknown
    uint64_t mt0, mt1;
    int engine;
};

static inline int TownTraceFormatTT(char* out, size_t cap, const TownTraceDevelop& d)
{
    return snprintf(out, cap, "TT t=%lld town=%d i=%d n=%d list=%08x mt0=%016llx mt1=%016llx e=%d\n",
                    (long long)d.time, (int)d.town, d.index, d.count, (unsigned)d.list,
                    (unsigned long long)d.mt0, (unsigned long long)d.mt1, d.engine);
}

// tokens[i] = (count << 32) | digest; sorted here, so the line does not depend
// on the order the families are stored in (a hash map, platform specific).
static inline int TownTraceFormatTF(char* out, size_t cap, int64_t time, int engine, uint64_t* tokens, size_t n)
{
    std::sort(tokens, tokens + n);
    int w = snprintf(out, cap, "TF t=%lld e=%d lists=%u", (long long)time, engine, (unsigned)n);
    for (size_t i = 0; i < n && w > 0 && (size_t)w < cap; i++)
        w += snprintf(out + w, cap - (size_t)w, " %u:%08x", (unsigned)(tokens[i] >> 32), (unsigned)tokens[i]);
    if (w > 0 && (size_t)w + 1 < cap) { out[w++] = '\n'; out[w] = 0; }
    return w;
}

// The TF cadence: one line per engine each time the TownSystem clock enters a
// new 600-iteration window (120 game units). `last` holds the window each
// engine last logged.
static inline bool TownTraceFamiliesDue(int64_t time, int engine, int64_t* last, int engines)
{
    if (time < 0 || engine < 0 || engine >= engines) return false;
    const int64_t window = time / 600;
    if (last[engine] == window) return false;
    last[engine] = window;
    return true;
}
