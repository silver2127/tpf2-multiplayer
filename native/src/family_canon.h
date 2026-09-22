// family_canon.h -- bring one ECS NodeList<N> to ascending entity order and
// rewrite its entity->position index (slice/hotjoin_order.inl, site "step").
// Pure memory work, no engine calls: tools/family_canon_test.cpp runs it on
// built lists. The includer supplies Readable(const void*, size_t).
//
// NodeList<N> (build 35924; Add 0x21d660, Remove 0x241030):
//   +0x08 node* begin   +0x10 node* end   +0x18 node* cap
//   node = { int32 entity, int32 componentIndex[N] }, stride 4 + 4N
//   +0x20 phmap flat_hash_map<Entity,int> m_entity2index:
//         +0x20 int8* ctrl   +0x28 slot* slots   +0x30 size   +0x38 capacity
//         slot = { int32 entity, int32 position }; ctrl[i] >= 0 = full
// Only Remove reads the index (to find the hole); keys and ctrl bytes stay as
// they are, only the positions change.
#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

struct FamilyCanonScratch {
    std::vector<uint64_t> pairs, kept, extracted, merged;   // (biased key << 32) | old position
    std::vector<uint32_t> perm;                             // old position -> new position
    std::vector<uint8_t> nodes;                             // the old nodes, while they are written back
};

enum FamilyCanonResult { FC_SORTED = 0, FC_REORDERED = 1, FC_REFUSED = 2 };

static const size_t FC_MAX_NODES = (size_t)1 << 26;

static inline uint64_t FcPair(int32_t key, size_t pos)
{
    return ((uint64_t)((uint32_t)key ^ 0x80000000u) << 32) | (uint64_t)(uint32_t)pos;
}
static inline uint32_t FcKeyBits(uint64_t p) { return (uint32_t)(p >> 32); }

// Returns FC_SORTED (untouched), FC_REORDERED, or FC_REFUSED (untouched; *why
// says what did not look like a node list). *moved = nodes whose position changed.
static int FamilyCanonList(uint8_t* nl, size_t stride, FamilyCanonScratch& s, size_t* moved, const char** why)
{
    *moved = 0;
    *why = nullptr;
    uint8_t* b = *(uint8_t**)(nl + 0x08);
    uint8_t* e = *(uint8_t**)(nl + 0x10);
    uint8_t* c = *(uint8_t**)(nl + 0x18);
    if (b == nullptr && e == nullptr) return FC_SORTED;
    if (!b || e < b || c < e || ((size_t)(e - b) % stride)) { *why = "vector"; return FC_REFUSED; }
    const size_t n = (size_t)(e - b) / stride;
    if (n > FC_MAX_NODES) { *why = "size"; return FC_REFUSED; }
    if (n < 2) return FC_SORTED;

    // fast path: already strictly ascending
    size_t i = 1;
    for (int32_t prev = *(int32_t*)b; i < n; i++) {
        const int32_t k = *(int32_t*)(b + i * stride);
        if (k <= prev) break;
        prev = k;
    }
    if (i == n) return FC_SORTED;

    // Only the tail can change. [0, lo) is ascending (i is the first break;
    // node i-1 may be the displaced one, so it joins the region). The region
    // [lo, n) is put in order: the few nodes that break it are pulled out,
    // sorted and merged back (a region mostly out of order is sorted whole).
    const size_t lo = i - 1;
    const size_t rn = n - lo;
    s.pairs.resize(rn);
    for (size_t j = 0; j < rn; j++) s.pairs[j] = FcPair(*(int32_t*)(b + (lo + j) * stride), lo + j);
    s.kept.clear();
    s.extracted.clear();
    for (size_t j = 0; j < rn; j++) {
        const uint64_t p = s.pairs[j];
        const bool aboveKept = s.kept.empty() || FcKeyBits(p) > FcKeyBits(s.kept.back());
        const bool belowNext = j + 1 == rn || FcKeyBits(p) < FcKeyBits(s.pairs[j + 1]);
        if (aboveKept && belowNext) s.kept.push_back(p);
        else s.extracted.push_back(p);
    }
    const std::vector<uint64_t>* region;
    if (s.extracted.size() * 4 > rn) {
        std::sort(s.pairs.begin(), s.pairs.end());
        region = &s.pairs;
    } else {
        std::sort(s.extracted.begin(), s.extracted.end());
        s.merged.resize(rn);
        std::merge(s.kept.begin(), s.kept.end(), s.extracted.begin(), s.extracted.end(), s.merged.begin());
        region = &s.merged;
    }
    // prefix nodes below the region's smallest key stay where they are;
    // the rest of the prefix merges with the region
    const uint32_t minKey = FcKeyBits((*region)[0]);
    size_t start = 0, hi = lo;
    while (start < hi) {
        const size_t mid = (start + hi) / 2;
        if (FcKeyBits(FcPair(*(int32_t*)(b + mid * stride), 0)) < minKey) start = mid + 1; else hi = mid;
    }
    const size_t tn = n - start;
    s.nodes.resize(tn * 8);   // (reused as the merged tail order, 8 bytes per entry)
    uint64_t* order = (uint64_t*)s.nodes.data();
    {
        size_t a = start, r = 0, o = 0;
        while (a < lo || r < rn) {
            const uint64_t pa = a < lo ? FcPair(*(int32_t*)(b + a * stride), a) : ~(uint64_t)0;
            const uint64_t pr = r < rn ? (*region)[r] : ~(uint64_t)0;
            if (pa < pr) { order[o++] = pa; a++; } else { order[o++] = pr; r++; }
        }
    }
    for (size_t j = 1; j < tn; j++)
        if (FcKeyBits(order[j]) == FcKeyBits(order[j - 1])) { *why = "duplicate entity"; return FC_REFUSED; }

    // the index must describe these nodes before anything moves: every slot
    // counted, every slot that points into the tail checked against its node
    int8_t* ctrl = *(int8_t**)(nl + 0x20);
    int32_t* slots = *(int32_t**)(nl + 0x28);
    const size_t size = *(size_t*)(nl + 0x30);
    const size_t cap = *(size_t*)(nl + 0x38);
    if (size != n) { *why = "index size"; return FC_REFUSED; }
    if (!ctrl || !slots || cap < n || cap > (FC_MAX_NODES << 2) || !Readable(ctrl, cap) || !Readable(slots, cap * 8)) {
        *why = "index";
        return FC_REFUSED;
    }
    size_t full = 0;
    for (size_t j = 0; j < cap; j++) {
        if (ctrl[j] < 0) continue;
        full++;
        const int32_t pos = slots[2 * j + 1];
        if (pos < 0 || (size_t)pos >= n) { *why = "index entry"; return FC_REFUSED; }
        if ((size_t)pos >= start && *(int32_t*)(b + (size_t)pos * stride) != slots[2 * j]) { *why = "index entry"; return FC_REFUSED; }
    }
    if (full != n) { *why = "index count"; return FC_REFUSED; }

    // write back the tail: nodes in the new order, then the slots pointing into it
    s.perm.resize(tn);
    s.merged.resize(tn * ((stride + 7) / 8));   // the old tail nodes (the region is consumed: order holds it)
    uint8_t* old = (uint8_t*)s.merged.data();
    memcpy(old, b + start * stride, tn * stride);
    size_t m = 0;
    for (size_t j = 0; j < tn; j++) {
        const size_t from = (size_t)(uint32_t)order[j] - start;
        s.perm[from] = (uint32_t)(start + j);
        if (from != j) {
            memcpy(b + (start + j) * stride, old + from * stride, stride);
            m++;
        }
    }
    for (size_t j = 0; j < cap; j++) {
        if (ctrl[j] < 0) continue;
        const size_t pos = (size_t)slots[2 * j + 1];
        if (pos >= start) slots[2 * j + 1] = (int32_t)s.perm[pos - start];
    }
    *moved = m;
    return FC_REORDERED;
}
