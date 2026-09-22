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

    // the index must describe exactly these nodes before anything moves
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
        const int32_t key = slots[2 * j];
        const int32_t pos = slots[2 * j + 1];
        if (pos < 0 || (size_t)pos >= n || *(int32_t*)(b + (size_t)pos * stride) != key) { *why = "index entry"; return FC_REFUSED; }
        full++;
    }
    if (full != n) { *why = "index count"; return FC_REFUSED; }

    // (key, old position) pairs; pull out the few that break the order, sort
    // those, merge them back. A list that is mostly out of order is sorted whole.
    s.pairs.resize(n);
    for (size_t j = 0; j < n; j++) s.pairs[j] = FcPair(*(int32_t*)(b + j * stride), j);
    s.kept.clear();
    s.extracted.clear();
    for (size_t j = 0; j < n; j++) {
        const uint64_t p = s.pairs[j];
        const bool aboveKept = s.kept.empty() || FcKeyBits(p) > FcKeyBits(s.kept.back());
        const bool belowNext = j + 1 == n || FcKeyBits(p) < FcKeyBits(s.pairs[j + 1]);
        if (aboveKept && belowNext) s.kept.push_back(p);
        else s.extracted.push_back(p);
    }
    const std::vector<uint64_t>* order;
    if (s.extracted.size() * 4 > n) {
        std::sort(s.pairs.begin(), s.pairs.end());
        order = &s.pairs;
    } else {
        std::sort(s.extracted.begin(), s.extracted.end());
        s.merged.resize(n);
        std::merge(s.kept.begin(), s.kept.end(), s.extracted.begin(), s.extracted.end(), s.merged.begin());
        order = &s.merged;
    }
    for (size_t j = 1; j < n; j++)
        if (FcKeyBits((*order)[j]) == FcKeyBits((*order)[j - 1])) { *why = "duplicate entity"; return FC_REFUSED; }

    // write back: nodes in the new order, then every full slot's position
    s.perm.resize(n);
    s.nodes.resize(n * stride);
    memcpy(s.nodes.data(), b, n * stride);
    size_t m = 0;
    for (size_t j = 0; j < n; j++) {
        const uint32_t old = (uint32_t)(*order)[j];
        s.perm[old] = (uint32_t)j;
        if (old != j) {
            memcpy(b + j * stride, s.nodes.data() + (size_t)old * stride, stride);
            m++;
        }
    }
    for (size_t j = 0; j < cap; j++)
        if (ctrl[j] >= 0) slots[2 * j + 1] = (int32_t)s.perm[(size_t)slots[2 * j + 1]];
    *moved = m;
    return FC_REORDERED;
}
